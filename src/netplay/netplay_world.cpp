// CataDDA Multiplayer ("DDA MP") — host glue between the world simulation
// and the network transport (v1.5 "watch-and-remote-control"):
//  * per-turn ascii snapshots of the host's own view, streamed to sessions,
//  * remote KEYS decoded into DEFAULTMODE action ids for input injection.
//
// The snapshot is produced by temporarily flipping the tiles renderer to the
// ascii path (map::draw is skipped entirely in tiles mode), drawing into a
// private offscreen window, and reading the character/color grid back. The
// host's own UI is refreshed normally afterwards, so nothing is visible.

#include "netplay/netplay.h"

#include <array>
#include <cstdio>
#include <string>
#include <vector>

#include "avatar.h"
#include "calendar.h"
#include "catacharset.h"
#include "color.h"
#include "creature.h"
#include "creature_tracker.h"
#include "game.h"
#include "item.h"
#include "itype.h"
#include "lightmap.h"
#include "map.h"
#include "map_memory.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "cata_utility.h"
#include "string_formatter.h"
#include "smooth_lighting.h"
#include "submap.h"
#include "translations.h"
#include "trap.h"
#include "weather.h"

#if defined(TILES)
#   include "cached_options.h"
#   include "cata_tiles.h"
#   include "cursesport.h"
#   include "enums.h"       // MULTITILE_TYPE (open_ / broken subtiles)
#   include "sdltiles.h"    // tilecontext (cata_tiles instance, TILES builds)
#   include "units_utility.h" // angle_to_dir4
#   include "vehicle.h"
#endif

namespace netplay
{

/* ------------------------------------------------------------------ */
/* constants                                                           */
/* ------------------------------------------------------------------ */

/// Frame geometry. Must fit inside the 2x2 submap bubble (48x48 tiles) so
/// that lookups around the center stay in-bounds.
static constexpr int FRAMEW = 45 + 4; // +2 tiles left / right of the game view
static constexpr int FRAMEH = 25 + 6; // +3 tiles above / below the game view

/// File-local corpse marker, same pattern as cata_tiles.cpp / item.cpp.
static const itype_id itype_corpse( "corpse" );

/* ------------------------------------------------------------------ */
/* helpers                                                             */
/* ------------------------------------------------------------------ */

#if defined(TILES)
/// Map an nc_color to its base-color index (0..15; bold encodes as +8) by
/// exploiting that catacurses::wattron stores the resolved color in the
/// window's cursor state.
static int nccolor_to_base( const nc_color &c )
{
    static catacurses::window probe = catacurses::newwin( 1, 1, point( 0, 0 ) );
    wattron( probe, c );
    const int base = static_cast<int>( probe.get<cata_cursesport::WINDOW>()->FG );
    wattroff( probe, c );
    return base;
}
#else
static int nccolor_to_base( const nc_color & )
{
    return 7;
}
#endif

static std::string status_line_for( const avatar &u )
{
    const int hp = u.get_hp();
    const int hp_max = u.get_hp_max();
    const std::string timestr = to_string( calendar::turn );
    const std::string wname = get_weather().weather_id->name.translated();
    return string_format( _( "HP: %d/%d | %s | %s" ), hp, hp_max, timestr, wname );
}

#if defined(TILES)
/* DDA MP v3.7: authoritative light. mine_tiles previously sampled the
 * renderer's last-uploaded smooth-lighting texels, which lag this turn's
 * state by one render frame, while the lit_level byte of the same snapshot
 * comes from a visibility cache that map::draw refreshed moments earlier.
 * Whenever anything moved, the two disagreed: cells the host had already
 * hidden stayed "seen in detail" on the client (gray patches over a zero
 * light value), and still-lit cells cut to the memory look a turn early.
 * Encoding here, on the game thread, from the same fresh caches the next
 * render frame would encode from makes the texel and the lit_level byte
 * describe exactly the same state. */
static std::vector<smooth_lighting::lightmap_texel> mp_host_auth_light;
static bool mp_host_auth_light_ok = false;

static void mp_host_encode_light( map &here, const tripoint_bub_ms &center )
{
    mp_host_auth_light_ok = false;
    // the render frame encodes its own fill area with the same threshold;
    // our area is the mined bubble frame plus a margin so the client's
    // smooth filter never cross-samples cells we did not encode (its
    // assign() zero fill covers anything outside)
    smooth_lighting::lightmap_fill_settings s;
    static constexpr int frame_margin = 4;
    s.area = half_open_rectangle<point>(
        point( std::max( 0, center.x() - FRAMEW / 2 - frame_margin ),
               std::max( 0, center.y() - FRAMEH / 2 - frame_margin ) ),
        point( std::min( MAPSIZE_X, center.x() - FRAMEW / 2 + FRAMEW + frame_margin ),
               std::min( MAPSIZE_Y, center.y() - FRAMEH / 2 + FRAMEH + frame_margin ) ) );
    s.vision_threshold = here.get_visibility_variables_cache().vision_threshold;
    s.tint = true;
    // per tile sampling never reads the reach masks
    s.masks = false;
    mp_host_auth_light_ok = smooth_lighting::encode_lightmap_layer( here, center.z(), s, mp_host_auth_light );
}

static bool mp_host_fetch_auth_texel( const tripoint_bub_ms &p,
                                      smooth_lighting::lightmap_texel &t )
{
    if( !mp_host_auth_light_ok || mp_host_auth_light.empty() ) {
        return false;
    }
    const point pp = p.xy().raw();
    if( pp.x < 0 || pp.x >= MAPSIZE_X || pp.y < 0 || pp.y >= MAPSIZE_Y ) {
        return false;
    }
    t = mp_host_auth_light[smooth_lighting::light_index( pp )];
    return true;
}
#endif

/// Mine per-cell tileset sprite ids for the client's tileset replay renderer.
/// Mirrors what cata_tiles::draw would have resolved for the same cells:
///  - visible cells: terrain/furniture/trap right off the map, plus monsters
///    and the uppermost ground item;
///  - unseen cells: the viewer's map memory (ter + f_/vp_/tr_ decoration id).
/// Vehicles are not mined in v1 (they fall back to the black cell / the
/// host's ascii frame for referenced glyphs).
static void mine_tiles( snapshot &out, avatar &u, map &here,
                        const tripoint_bub_ms &center )
{
    out.tiles.reserve( static_cast<size_t>( FRAMEW ) * FRAMEH );
    // Ensure the memory regions this frame touches are valid before reading.
    u.prepare_map_memory_region(
        here.get_abs( center + tripoint_rel_ms( -FRAMEW / 2, -FRAMEH / 2, 0 ) ),
        here.get_abs( center + tripoint_rel_ms( FRAMEW / 2, FRAMEH / 2, 0 ) ) );
    const auto &viscache = here.get_cache_ref( center.z() ).visibility_cache;
    // DDA MP debug: per-cell trace of the mined light facts for the first 2
    // snapshots, one line into netplay_light_trace.log per cell, to compare
    // cell for cell with the client's rows (prefix H) and the blue-tile issue.
    static int mp_light_trace_frame = 0;
    static std::string mp_light_trace;
    const bool mp_light_trace_this = mp_light_trace_frame < 4 ||
        ( mp_light_trace_frame < 400 && mp_light_trace_frame % 96 < 2 );
    for( int dy = 0; dy < FRAMEH; dy++ ) {
        for( int dx = 0; dx < FRAMEW; dx++ ) {
            const tripoint_bub_ms p( center + tripoint_rel_ms( dx - FRAMEW / 2,
                                     dy - FRAMEH / 2, 0 ) );
            tile_cell t;
            if( here.inbounds( p ) ) {
                const lit_level ll = viscache[p.x()][p.y()];
                t.light = static_cast<uint8_t>( static_cast<int>( ll ) );
#if defined( TILES )
                // DDA MP v3.7: sample the authoritative texel encoded from
                // this turn's fresh visibility/light caches (see
                // mp_host_encode_light). The a channel rides the same
                // renderer-state facts as the color bytes.
                smooth_lighting::lightmap_texel lt{};
                if( mp_host_fetch_auth_texel( p, lt ) ) {
                    // the color bytes are always the authoritative state of
                    // this turn: an empty texel is a valid "not seen"
                    t.flags |= 16;
                    if( lt.a & smooth_lighting::texel_barrier ) {
                        t.flags |= 8;
                    }
                    if( lt.a & smooth_lighting::texel_detail ) {
                        t.flags |= 32;
                    }
                    t.light_r = lt.r;
                    t.light_g = lt.g;
                    t.light_b = lt.b;
                    if( mp_light_trace_this ) {
                        mp_light_trace += string_format(
                                              "H f=%02d y=%02d x=%02d ll=%d r=%3d g=%3d b=%3d a=%d\n",
                                              mp_light_trace_frame, dy, dx,
                                              static_cast<int>( ll ), lt.r, lt.g, lt.b, lt.a );
                    }
                } else if( mp_light_trace_this ) {
                    mp_light_trace += string_format(
                                          "H f=%02d y=%02d x=%02d ll=%d miss\n",
                                          mp_light_trace_frame, dy, dx,
                                          static_cast<int>( ll ) );
                }
#endif
                const bool visible = ll != lit_level::BLANK && ll != lit_level::DARK &&
                                     ll != lit_level::MEMORIZED;
                if( visible ) {
                    const ter_id &tid = here.ter( p );
                    if( tid ) {
                        t.ter = tid.id().str();
#if defined(TILES)
                        // Exact multi-tile variant the host renderer resolves.
                        if( tilecontext ) {
                            int sub = 0;
                            int rot = 0;
                            tilecontext->netplay_probe_terrain( p, sub, rot );
                            t.ter_sub = static_cast<uint8_t>( sub );
                            t.ter_rot = static_cast<uint8_t>( rot );
                        }
#endif
                    }
                    const furn_id &fid = here.furn( p );
                    if( fid ) {
                        t.furn = fid.id().str();
#if defined(TILES)
                        if( tilecontext ) {
                            int sub = 0;
                            int rot = 0;
                            tilecontext->netplay_probe_furniture( p, sub, rot );
                            t.furn_sub = static_cast<uint8_t>( sub );
                            t.furn_rot = static_cast<uint8_t>( rot );
                        }
#endif
                    }
                    const trap &tr = here.tr_at( p );
                    if( !tr.is_null() && tr.can_see( p, u ) ) {
                        t.trap = tr.loadid.id().str();
                    }
                    monster *m = get_creature_tracker().creature_at<monster>( p );
                    if( m != nullptr && m->type != nullptr && m->type->id.is_valid() ) {
                        t.critter = m->type->id.str();
                    } else {
                        // DDA MP: NPC sprites (non-hostile humans are not in
                        // the monster tracker as mon_* ids).
                        const Creature *cr = get_creature_tracker().creature_at( p );
                        const npc *n = cr != nullptr && cr->is_npc()
                                       ? static_cast<const npc *>( cr )
                                       : nullptr;
                        if( n != nullptr && !n->symbol().empty() ) {
                            t.critter = n->male ? "npc_male" : "npc_female";
                        }
                    }
                    // DDA MP: the host avatar is not tracked as a critter.
                    if( p == u.pos_bub() ) {
                        t.critter = u.male ? "player_male" : "player_female";
                    }
                    if( here.sees_some_items( p, u ) ) {
                        const maptile &tile = here.maptile_at( p );
                        if( tile.get_item_count() > 0 ) {
                            // DDA MP: the game would also paint the
                            // highlight_item marker here; stream that fact.
                            t.flags |= 1;
                            if( tile.get_item_count() > 1 ) {
                                t.flags |= 2;
                            }
                            const item &itm = tile.get_uppermost_item();
                            const mtype *mon = itm.get_mtype();
                            t.item = ( itm.typeId() == itype_corpse && mon != nullptr )
                                     ? "corpse_" + mon->id.str()
                                     : itm.typeId().str();
                        }
                    }
                    // DDA MP v3.3: vehicle part on top of everything else.
                    // Mirrors cata_tiles::draw_vpart: display part of the tile,
                    // open/broken subtile and the 4-way rotation of the hull.
                    const optional_vpart_position ovp = here.veh_at( p );
                    if( static_cast<bool>( ovp ) ) {
                        const vehicle &veh = ovp->vehicle();
                        const vpart_display vd = veh.get_display_of_tile( ovp->mount_pos() );
                        if( !vd.id.is_null() ) {
                            t.vpart = vd.get_tileset_id();
                            t.vpart_sub = vd.is_open ? static_cast<uint8_t>( open_ )
                                          : vd.is_broken ? static_cast<uint8_t>( broken ) : 0;
                            t.vpart_rot =
                                static_cast<uint8_t>( angle_to_dir4( veh.face.dir() - 270_degrees ) );
                            // Savegame-invisible detail the game still draws:
                            // a cargo part with items gets the highlight_item
                            // badge on the part tile (cata_tiles::draw_vpart).
                            if( vd.has_cargo ) {
                                t.flags |= 4;
                            }
                        }
                    }
                } else {
                    // Unseen: replay the viewer's map memory the same way
                    // cata_tiles::get_terrain_memory_at family would.
                    const memorized_tile &mt = u.get_memorized_tile( here.get_abs( p ) );
                    if( !mt.get_ter_id().empty() ) {
                        t.ter = mt.get_ter_id();
                        t.ter_sub = static_cast<uint8_t>( mt.get_ter_subtile() );
                        t.ter_rot = static_cast<uint8_t>( mt.get_ter_rotation() );
                        t.light = static_cast<uint8_t>( static_cast<int>( lit_level::MEMORIZED ) );
                    }
                    const std::string &dec = mt.get_dec_id();
                    if( string_starts_with( dec, "vp_" ) ) {
                        // Vehicle part memory: tileset id keeps the variant
                        // ("vp_{id}#{variant}"); the client splits it. Drawing
                        // this through the furniture slot used to trigger
                        // "invalid variant ''" on the client.
                        t.vpart = dec;
                        t.vpart_sub = static_cast<uint8_t>( mt.get_dec_subtile() );
                        t.vpart_rot = static_cast<uint8_t>( mt.get_dec_rotation() );
                    } else if( string_starts_with( dec, "f_" ) ) {
                        t.furn = dec;
                        t.furn_sub = static_cast<uint8_t>( mt.get_dec_subtile() );
                        t.furn_rot = static_cast<uint8_t>( mt.get_dec_rotation() );
                    } else if( string_starts_with( dec, "tr_" ) ) {
                        t.trap = dec;
                    }
                }
            }
            out.tiles.push_back( std::move( t ) );
        }
    }
    if( mp_light_trace_this && !mp_light_trace.empty() ) {
        if( std::FILE *f = std::fopen( "netplay_light_trace.log", "a" ) ) {
            std::fputs( mp_light_trace.c_str(), f );
            std::fclose( f );
        }
        mp_light_trace.clear();
    }
    ++mp_light_trace_frame;
}

/* ------------------------------------------------------------------ */
/* snapshot builder                                                    */
/* ------------------------------------------------------------------ */

static void build_host_snapshot( snapshot &out )
{
    avatar &u = get_avatar();
    map &here = get_map();
    const tripoint_bub_ms center = u.pos_bub();

#if defined(TILES)
    const bool prev_tiles = use_tiles;
    use_tiles = false;
#endif
    // Private render target, never refreshed on screen.
    catacurses::window win = catacurses::newwin( FRAMEH, FRAMEW, point( 0, 0 ) );

    // Terrain, furniture, traps, fields, ground items and vehicles: the same
    // pipeline the game's own ascii renderer uses (memory of unseen tiles
    // included). It also updates the visibility cache and map memory.
    here.draw( win, center );
#if defined(TILES)
    // DDA MP v3.7: encode the authoritative light texels now, right after
    // map::draw refreshed the visibility/light caches this snapshot reads,
    // so its texels and its lit_level byte describe the same turn state
    // (see mp_host_encode_light).
    mp_host_encode_light( here, center );
#endif

    // Critter overlay: map::draw does not render creatures; draw visible
    // monsters/NPCs over the freshly painted cells.
    const auto &viscache = here.get_cache_ref( center.z() ).visibility_cache;
    for( int dy = 0; dy < FRAMEH; dy++ ) {
        for( int dx = 0; dx < FRAMEW; dx++ ) {
            const tripoint_bub_ms p( center + tripoint_rel_ms( dx - FRAMEW / 2,
                                     dy - FRAMEH / 2, 0 ) );
            if( !here.inbounds( p ) ) {
                continue;
            }
            const lit_level ll = viscache[p.x()][p.y()];
            if( ll == lit_level::BLANK || ll == lit_level::DARK ||
                ll == lit_level::MEMORIZED ) {
                continue;
            }
            const Creature *cr = get_creature_tracker().creature_at( p );
            if( cr == nullptr ) {
                continue;
            }
            const std::string &sym = cr->symbol();
            if( sym.empty() ) {
                continue;
            }
            mvwputch( win, point( dx, dy ), cr->symbol_color(),
                      static_cast<int>( UTF8_getch( sym ) ) );
        }
    }
    // The avatar itself is not tracked as a critter.
    mvwputch( win, point( FRAMEW / 2, FRAMEH / 2 ), c_white, '@' );

    // Read back the character grid.
    out.width = FRAMEW;
    out.height = FRAMEH;
    out.cells.reserve( FRAMEW * FRAMEH );
#if defined(TILES)
    cata_cursesport::WINDOW *wraw = win.get<cata_cursesport::WINDOW>();
    for( int y = 0; y < FRAMEH; y++ ) {
        for( int x = 0; x < FRAMEW; x++ ) {
            cell c;
            const cata_cursesport::cursecell &raw = wraw->line[y].chars[x];
            c.glyph = static_cast<int>( UTF8_getch( raw.ch ) );
            c.color = static_cast<int>( raw.FG );
            out.cells.push_back( c );
        }
    }
#endif

#if defined(TILES)
    // Sprite layer for the client's tileset replay renderer (optional; the
    // ascii cells above remain the universal fallback). Built straight off of
    // the map/memory, independent of the ascii window above.
    if( tiles_replay_enabled() ) {
        mine_tiles( out, u, here, center );
    }
#endif

    // Restore the tiles renderer before the window is destructed (RAII).
#if defined(TILES)
    use_tiles = prev_tiles;
#endif

    out.status_line = status_line_for( u );
}

void host_game_tick()
{
    if( !host_is_running() || host_session_count() == 0 ) {
        return;
    }
    if( !get_map().inbounds( get_avatar().pos_bub() ) ) {
        return; // no world yet
    }
    snapshot snap;
    build_host_snapshot( snap );
    // DDA MP debug: log what the host broadcasts (w/h and tile count) so the
    // netplay test can compare it with what the client's replay loop sees.
    {
        static int mp_snap_log = 0;
        if( mp_snap_log < 200 ) {
            ++mp_snap_log;
            try {
                std::ofstream f( "netplay_tiles.log", std::ios::app );
                if( f ) {
                    f << "host snap #" << mp_snap_log << " w=" << snap.width
                      << " h=" << snap.height << " cells=" << snap.cells.size()
                      << " tiles=" << snap.tiles.size() << "\n";
                }
            } catch( ... ) { }
        }
    }
    host_broadcast_snapshot( snap );
}

/* ------------------------------------------------------------------ */
/* remote input                                                        */
/* ------------------------------------------------------------------ */

bool host_pop_action( std::string &action )
{
    uint32_t code = 0;
    if( !host_pop_key( code ) ) {
        return false;
    }
    switch( code ) {
        case 1:
            action = "LEFT";
            break;
        case 2:
            action = "RIGHT";
            break;
        case 3:
            action = "UP";
            break;
        case 4:
            action = "DOWN";
            break;
        case 10:
            // CONFIRM in DEFAULTMODE == wait one turn
            action = "CONFIRM";
            break;
        case 12:
            action = "RIGHTUP";
            break;
        case 13:
            action = "RIGHTDOWN";
            break;
        case 14:
            action = "LEFTUP";
            break;
        case 15:
            action = "LEFTDOWN";
            break;
        default:
            return false;
    }
    return true;
}

} // namespace netplay
