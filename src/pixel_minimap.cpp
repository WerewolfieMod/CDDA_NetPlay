#if defined(TILES)

#include "pixel_minimap.h"

#include <algorithm>
#include <bitset>
#include <cmath>
#include <cstdio>
#include <memory>
#include <optional>

#include "cached_options.h"
#include "cata_utility.h"
#include "character.h"
#include "color.h"
#include "creature.h"
#include "creature_tracker.h"
#include "debug.h"
#include "game.h"
#include "level_cache.h"
#include "lightmap.h"
#include "map.h"
#include "map_scale_constants.h"
#include "mapdata.h"
#include "math_defines.h"
#include "mdarray.h"
#include "monster.h"
#include "mtype.h"
#include "netplay/netplay.h"
#include "pixel_minimap_projectors.h"
#include "sdl_utils.h"
#include "type_id.h"
#include "vehicle.h"
#include "viewer.h"
#include "vpart_position.h"

namespace
{

const point total_tiles_count = { MAX_VIEW_DISTANCE * 2 + 1, MAX_VIEW_DISTANCE * 2 + 1 };

/* DDA MP v4.0a-242: the host streams its pixel-minimap block as one of 16
 * base colours per cell. This is the same table the host used to encode them
 * (netplay_world.cpp, mp_base_color_index), so decoded cells look identical. */
nc_color mp_minimap_base_color( uint32_t index )
{
    static const nc_color base16[] = {
        c_black, c_red, c_green, c_brown, c_blue, c_magenta, c_cyan,
        c_light_gray, c_dark_gray, c_light_red, c_light_green, c_yellow,
        c_light_blue, c_pink, c_light_cyan, c_white
    };
    return base16[std::min<uint32_t>( index, 15 )];
}

// Enemy-beacon flicker sweeps brightness between these two percentages.
constexpr int beacon_flicker_dim = 25;
constexpr int beacon_flicker_full = 100;
// Milliseconds contributed by one unit of the beacon_blink_interval setting.
constexpr int beacon_blink_ms_per_step = 200;
// Edge pixels of the beacon diamond are darkened by this divisor to outline it.
constexpr int beacon_edge_divisor = 3;

point get_pixel_size( const point &tile_size, pixel_minimap_mode mode )
{
    switch( mode ) {
        case pixel_minimap_mode::solid:
            return tile_size;

        case pixel_minimap_mode::squares:
            return { std::max( tile_size.x - 1, 1 ), std::max( tile_size.y - 1, 1 ) };

        case pixel_minimap_mode::dots:
            return { point::south_east };
    }

    cata_fatal( "Invalid pixel_minimap_mode %d", static_cast<int>( mode ) );
}

/// Returns a number in range [0..1]. The range lasts for @param phase_length_ms (milliseconds).
float get_animation_phase( int phase_length_ms )
{
    if( phase_length_ms == 0 ) {
        return 0.0f;
    }

    return std::fmod<float>( GetTicks(), phase_length_ms ) / phase_length_ms;
}

SDL_Color get_map_color_at( const tripoint_bub_ms &p )
{
    const map &here = get_map();
    if( const optional_vpart_position vp = here.veh_at( p ) ) {
        const vpart_display vd = vp->vehicle().get_display_of_tile( vp->mount_pos() );
        return curses_color_to_SDL( vd.color );
    }

    if( const furn_id &furn = here.furn( p ) ) {
        return curses_color_to_SDL( furn->color() );
    }

    return curses_color_to_SDL( here.ter( p )->color() );
}

SDL_Color get_critter_color( Creature *critter, int flicker, int mixture )
{
    SDL_Color result = curses_color_to_SDL( critter->symbol_color() );

    if( const monster *m = dynamic_cast<monster *>( critter ) ) {
        //faction status (attacking or tracking) determines if red highlights get applied to creature
        const monster_attitude matt = m->attitude( &get_player_character() );

        if( ( MATT_ATTACK == matt || MATT_FOLLOW == matt ) &&
            !m->has_flag( mon_flag_APPEARS_NEUTRAL ) ) {
            const SDL_Color red_pixel = SDL_Color{ 0xFF, 0x0, 0x0, 0xFF };
            result = adjust_color_brightness( mix_colors( result, red_pixel, mixture ), flicker );
        }
    }

    return result;
}

} // namespace

pixel_minimap::pixel_minimap( const SDL_Renderer_Ptr &renderer,
                              const GeometryRenderer_Ptr &geometry ) :
    renderer( renderer ),
    geometry( geometry ),
    type( pixel_minimap_type::ortho ),
    screen_rect{ 0, 0, 0, 0 }
{
}

pixel_minimap::~pixel_minimap() = default;

void pixel_minimap::set_type( pixel_minimap_type type )
{
    if( this->type != type ) {
        this->type = type;
        reset();
    }
}

void pixel_minimap::set_settings( const pixel_minimap_settings &settings )
{
    this->settings = settings;
    reset();
}

void pixel_minimap::set_screen_rect( const SDL_Rect &screen_rect )
{
    if( this->screen_rect == screen_rect && projector ) {
        return;
    }

    this->screen_rect = screen_rect;
    projector = create_projector( screen_rect );
    pixel_size = get_pixel_size( projector->get_tile_size(), settings.mode );
    tf_ = compute_minimap_transform( projector->get_tiles_size( total_tiles_count ),
                                     screen_rect, settings.scale_to_fit );
}

void pixel_minimap::reset()
{
    projector.reset();
    screen_rect = SDL_Rect{ 0, 0, 0, 0 };
}

void pixel_minimap::build_batches( const tripoint_bub_ms &center )
{
    terrain_batch_.clear();
    beacon_batch_.clear();
    has_blinking_beacons_ = false;

    terrain_batch_.reserve_quads( total_tiles_count.x * total_tiles_count.y );

    const map &m = get_map();
    const level_cache &access_cache = m.access_cache( center.z() );
    const bool nv_goggle = get_player_character().get_vision_modes()[NV_GOGGLES];
    creature_tracker &creatures = get_creature_tracker();

    // DDA MP v4.0a-242: a netplay client only holds the current frame's tiles
    // in its mirror map, so the panel minimap was blank outside it. The host
    // streams exactly the block this minimap draws (terrain colours plus
    // what its own player can see); prefer those cells when connected.
    static netplay::minimap_frame mp_minimap;
    const bool mp_minimap_on = netplay::client_is_connected() &&
                               netplay::client_minimap( mp_minimap );
    if( mp_minimap_on ) {
        static bool mp_mm_used_logged = false;
        if( !mp_mm_used_logged && netplay::mp_diag_enabled() ) {
            mp_mm_used_logged = true;
            if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
                std::fprintf( f, "minimap panel: streamed block %dx%d center=%d,%d\n",
                              mp_minimap.width, mp_minimap.height,
                              mp_minimap.center_x, mp_minimap.center_y );
                std::fclose( f );
            }
        }
    }

    // Full blink period in milliseconds; default is 2000.
    const int indicator_length = settings.beacon_blink_interval * beacon_blink_ms_per_step;

    int flicker = beacon_flicker_full;
    int mixture = 0;

    if( indicator_length > 0 ) {
        const float t = get_animation_phase( 2 * indicator_length );
        const float s = std::sin( 2 * M_PI * t );

        flicker = lerp_clamped( beacon_flicker_dim, beacon_flicker_full, std::abs( s ) );
        mixture = lerp_clamped( 0, beacon_flicker_full, std::max( s, 0.0f ) );
    }

    const point_rel_ms start( center.x() - total_tiles_count.x / 2,
                              center.y() - total_tiles_count.y / 2 );
    // Scaled so beacons keep their proportion to the map under scale_to_fit.
    const point beacon_size = {
        std::max( static_cast<int>( projector->get_tile_size().x *tf_.scale_x *
                                    settings.beacon_size / 2 ), 2 ),
        std::max( static_cast<int>( projector->get_tile_size().y *tf_.scale_y *
                                    settings.beacon_size / 2 ), 2 )
    };
    // A constant size, or the fractional pitch lands as a mix of sizes and bands.
    const bool fills_cell = pixel_size == projector->get_tile_size();
    const int fixed_w = std::max( 1, static_cast<int>( std::lround( pixel_size.x * tf_.scale_x ) ) );
    const int fixed_h = std::max( 1, static_cast<int>( std::lround( pixel_size.y * tf_.scale_y ) ) );

    for( int y = 0; y < total_tiles_count.y; y++ ) {
        for( int x = 0; x < total_tiles_count.x; x++ ) {
            const tripoint_bub_ms p = start + tripoint_bub_ms( x, y, center.z() );
            lit_level lighting = lit_level::BLANK;
            SDL_Color color{};
            bool mp_have_cell = false;
            if( mp_minimap_on ) {
                const int mmx = mp_minimap.center_x + p.x() - center.x();
                const int mmy = mp_minimap.center_y + p.y() - center.y();
                if( mmx >= 0 && mmy >= 0 && mmx < mp_minimap.width &&
                    mmy < mp_minimap.height ) {
                    const size_t mi = static_cast<size_t>( mmy ) * mp_minimap.width + mmx;
                    if( mi < mp_minimap.color.size() && mi < mp_minimap.seen.size() ) {
                        const uint8_t mm_seen = mp_minimap.seen[mi];
                        if( mm_seen == 0 ) {
                            // The host's minimap also skips cells its player
                            // cannot see right now.
                            continue;
                        }
                        lighting = mm_seen == 1 ? lit_level::LOW : lit_level::LIT;
                        color = curses_color_to_SDL(
                                    mp_minimap_base_color( mp_minimap.color[mi] ) );
                        mp_have_cell = true;
                    }
                }
            }
            if( !mp_have_cell ) {
                if( !m.inbounds( p ) ) {
                    // p might be out-of-bounds when peeking at submap boundary. Example: center=(64,59,-5), start=(4,-1) -> p=(4,-1,-5)
                    continue;
                }
                lighting = access_cache.visibility_cache[p.x()][p.y()];
                if( lighting == lit_level::BLANK || lighting == lit_level::DARK ) {
                    // TODO: Map memory?
                    continue;
                }
                color = get_map_color_at( p );
            }

            if( nv_goggle ) {
                if( lighting == lit_level::LOW ) {
                    color = color_pixel_nightvision( color );
                } else {
                    color = color_pixel_overexposed( color );
                }
            } else if( lighting == lit_level::LOW ) {
                color = color_pixel_grayscale( color );
            }

            color = adjust_color_brightness( color, settings.brightness );

            const point pos = projector->get_tile_pos( { x, y }, total_tiles_count );
            const point origin_px( snap_to_pixel( tf_.origin_x, tf_.scale_x, pos.x ),
                                   snap_to_pixel( tf_.origin_y, tf_.scale_y, pos.y ) );
            const point extent_px(
                fills_cell ? std::max( 1, snap_to_pixel( tf_.origin_x, tf_.scale_x,
                                       pos.x + pixel_size.x ) - origin_px.x )
                : fixed_w,
                fills_cell ? std::max( 1, snap_to_pixel( tf_.origin_y, tf_.scale_y,
                                       pos.y + pixel_size.y ) - origin_px.y )
                : fixed_h );
            terrain_batch_.append_quad( origin_px.x, origin_px.y,
                                        extent_px.x, extent_px.y, to_fcolor( color ) );

            Creature *critter = creatures.creature_at( p, true );
            if( critter != nullptr && get_player_view().sees( m, *critter ) ) {
                if( indicator_length > 0 ) {
                    has_blinking_beacons_ = true;
                }
                const SDL_Rect critter_rect = SDL_Rect{ origin_px.x, origin_px.y,
                                                        beacon_size.x, beacon_size.y };
                append_beacon( beacon_batch_, critter_rect,
                               get_critter_color( critter, flicker, mixture ),
                               beacon_edge_divisor );
            }
        }
    }
}

void pixel_minimap::present()
{
    const bool had_clip = RenderIsClipEnabled( renderer );
    SDL_Rect prior_clip = { 0, 0, 0, 0 };
    if( had_clip ) {
        RenderGetClipRect( renderer, &prior_clip );
    }
    // A caller may already be clipping to something narrower than the
    // minimap rect, so draw within both.
    SDL_Rect clip = tf_.dest_rect;
    if( had_clip && !GetRectIntersection( prior_clip, tf_.dest_rect, clip ) ) {
        return;
    }

    SDL_BlendMode prior_blend = SDL_BLENDMODE_NONE;
    GetRenderDrawBlendMode( renderer, prior_blend );
    SetRenderDrawBlendMode( renderer, SDL_BLENDMODE_BLEND );
    RenderSetClipRect( renderer, &clip );

    geometry->rect( renderer, tf_.dest_rect,
                    SDL_Color{ static_cast<Uint8>( pixel_minimap_r ),
                               static_cast<Uint8>( pixel_minimap_g ),
                               static_cast<Uint8>( pixel_minimap_b ),
                               static_cast<Uint8>( pixel_minimap_a ) } );
    render_quad_batch( renderer, terrain_batch_ );
    render_quad_batch( renderer, beacon_batch_ );

    RenderSetClipRect( renderer, had_clip ? &prior_clip : nullptr );
    SetRenderDrawBlendMode( renderer, prior_blend );
}

void pixel_minimap::draw( const SDL_Rect &screen_rect, const tripoint_bub_ms &center )
{
    if( !g ) {
        return;
    }

    if( screen_rect.w <= 0 || screen_rect.h <= 0 ) {
        return;
    }

    set_screen_rect( screen_rect );
    build_batches( center );
    present();
}

std::unique_ptr<pixel_minimap_projector> pixel_minimap::create_projector(
    const SDL_Rect &max_screen_rect )
const
{
    switch( type ) {
        case pixel_minimap_type::ortho:
            return std::make_unique<pixel_minimap_ortho_projector> ( total_tiles_count, max_screen_rect,
                    settings.square_pixels );

        case pixel_minimap_type::iso:
            return std::make_unique<pixel_minimap_iso_projector>( total_tiles_count, max_screen_rect,
                    settings.square_pixels );
    }

    cata_fatal( "Invalid pixel_minimap_type %d", static_cast<int>( type ) );
}

#endif // TILES
