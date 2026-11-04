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
#include "netplay/inventory_source.h"

#include <array>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "avatar.h"
#include "action.h"
#include "activity_actor_definitions.h"
#include "calendar.h"
#include "catacharset.h"
#include "color.h"
#include "creature.h"
#include "creature_tracker.h"
#include "craft_reservation.h"
#include "effect.h"
#include "field.h"
#include "field_type.h"
#include "game.h"
#include "item.h"
#include "itype.h"
#include "json.h"
#include "json_loader.h"
#include "lightmap.h"
#include "map.h"
#include "map_memory.h"
#include "map_selector.h"
#include "memory_fast.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "ranged.h"
#include "panels.h"
#include "projectile.h"
#include "messages.h"
#include "cata_utility.h"
#include "string_formatter.h"
#include "smooth_lighting.h"
#include "submap.h"
#include "sounds.h"
#include "translations.h"
#include "trap.h"
#include "weather.h"
#include "overmapbuffer.h"
#include "worldfactory.h"

#include <chrono>
#include <cstdlib>

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
/// that lookups around the center stay in-bounds. DDA MP v4.0a-122: one extra
/// row keeps the client's native canvas taller than the display buffer, so the
/// map reaches the bottom edge instead of leaving a black strip below it.
static constexpr int FRAMEW = 45 + 4; // +2 tiles left / right of the game view
static constexpr int FRAMEH = 25 + 7; // +3 tiles above / below, +1 to fill the screen

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
/* DDA MP: local trait handle for the INATTENTIVE attitude badge check. */
static const trait_id trait_INATTENTIVE( "INATTENTIVE" );

/* DDA MP v3.9: diagnostics master switch. When false, no netplay_tiles.log /
 * netplay_light_trace.log lines are produced by this TU (mine traces,
 * snapshot size logs). Flip to true and rebuild to re-enable. */
/* DDA MP v3.9: diagnostics master switch — built-in (mp_diag_dir).
 * Default ON without any env; DDA_MP_DIAG=0|off disables. */
static const bool mp_netplay_diag = mp_diag_enabled();

/* DDA MP v4.0a-2: each netplay avatar keeps its OWN map memory on the host,
 * keyed by its stable unique id ("netplay_avatar:<nick>"). Freshly seen
 * tiles are memorized here and replayed to the owning client, so an avatar
 * never sees through the host player's eyes; memory is RAM-only and starts
 * fresh after a host restart. */
static std::map<std::string, map_memory> av_mem_store;

static map_memory &av_mem_for( const npc &n )
{
    return av_mem_store[n.get_unique_id()];
}

static void mp_render_memory( const memorized_tile &mt, tile_cell &t )
{
    // DDA MP v4.0a-114: ship the memory itself as well. The native renderer
    // reads map memory (not the live tile) for anything outside the viewer's
    // sight, so the client rebuilds it in its own avatar's memory.
    t.mem_ter = mt.get_ter_id();
    t.mem_dec = mt.get_dec_id();
    t.mem_ter_sub = static_cast<uint8_t>( mt.get_ter_subtile() );
    t.mem_ter_rot = static_cast<uint8_t>( mt.get_ter_rotation() );
    t.mem_dec_sub = static_cast<uint8_t>( mt.get_dec_subtile() );
    t.mem_dec_rot = static_cast<uint8_t>( mt.get_dec_rotation() );
    t.mem_sym = static_cast<uint32_t>( mt.symbol );
    if( !mt.get_ter_id().empty() ) {
        t.ter = mt.get_ter_id();
        t.ter_sub = static_cast<uint8_t>( mt.get_ter_subtile() );
        t.ter_rot = static_cast<uint8_t>( mt.get_ter_rotation() );
        t.light = static_cast<uint8_t>( static_cast<int>( lit_level::MEMORIZED ) );
    }
    const std::string &dec = mt.get_dec_id();
    if( string_starts_with( dec, "vp_" ) ) {
        // Vehicle part memory: tileset id keeps the variant
        // ("vp_{id}#{variant}"); the client splits it.
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

static void mine_tiles( snapshot &out, avatar &u, Character *viewer, map &here,
                        const tripoint_bub_ms &center )
{
    out.tiles.reserve( static_cast<size_t>( FRAMEW ) * FRAMEH );
    // DDA MP v4.0a-2: memory bookkeeping per viewer. The host player uses
    // its own avatar memory; every netplay avatar npc gets its own entry in
    // av_mem_store. Watcher sessions (no viewer) keep the v3.x behavior.
    npc *npc_viewer = viewer != nullptr ? dynamic_cast<npc *>( viewer ) : nullptr;
    if( viewer == nullptr ) {
        // Ensure the memory regions this frame touches are valid before reading.
        u.prepare_map_memory_region(
            here.get_abs( center + tripoint_rel_ms( -FRAMEW / 2, -FRAMEH / 2, 0 ) ),
            here.get_abs( center + tripoint_rel_ms( FRAMEW / 2, FRAMEH / 2, 0 ) ) );
    } else if( npc_viewer != nullptr ) {
        map_memory &mm = av_mem_for( *npc_viewer );
        mm.prepare_region(
            here.get_abs( center + tripoint_rel_ms( -FRAMEW / 2, -FRAMEH / 2, 0 ) ),
            here.get_abs( center + tripoint_rel_ms( FRAMEW / 2, FRAMEH / 2, 0 ) ) );
    }
    // DDA MP v4.0a-2: sight facts and attitude badges belong to the viewer
    // (a bound client's avatar npc); watcher sessions keep the v3.x behavior
    // of checking everything against the host player.
    Character &self = viewer != nullptr ? *viewer : u;
    const auto &viscache = here.get_cache_ref( center.z() ).visibility_cache;
    // DDA MP debug: per-cell trace of the mined light facts for the first 2
    // snapshots, one line into netplay_light_trace.log per cell, to compare
    // cell for cell with the client's rows (prefix H) and the blue-tile issue.
    static int mp_light_trace_frame = 0;
    static std::string mp_light_trace;
    const bool mp_light_trace_this = mp_netplay_diag &&
        ( mp_light_trace_frame < 4 ||
        ( mp_light_trace_frame < 400 && mp_light_trace_frame % 96 < 2 ) );
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
                // DDA MP v4.0a-2: for a bound client's own POV, the tile its
                // avatar stands on is always visible (the engine always
                // renders the player sprite at the screen center; some tile
                //美术 have their position LOS-filtered otherwise).
                const bool centered = viewer != nullptr && p == viewer->pos_bub();
                const bool visible = ( ll != lit_level::BLANK && ll != lit_level::DARK &&
                                       ll != lit_level::MEMORIZED ) || centered;
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
                    // DDA MP v4.0a-2: remember what the avatar's own eyes
                    // just saw so later darkness replays ITS memory.
                    if( npc_viewer != nullptr ) {
                        map_memory &mm = av_mem_for( *npc_viewer );
                        if( !t.ter.empty() ) {
                            mm.set_tile_terrain( here.get_abs( p ), t.ter,
                                                 t.ter_sub, t.ter_rot );
                        }
                        if( !t.furn.empty() ) {
                            mm.set_tile_decoration( here.get_abs( p ), t.furn,
                                                    t.furn_sub, t.furn_rot );
                        }
                        if( !t.trap.empty() ) {
                            mm.set_tile_decoration( here.get_abs( p ), t.trap, 0, 0 );
                        }
                    }
                    // DDA MP v4.0a-135: graffiti sprayed on the tile.
                    if( here.has_graffiti_at( p ) ) {
                        t.graffiti = here.graffiti_at( p );
                    }
                    const trap &tr = here.tr_at( p );
                    if( !tr.is_null() && tr.can_see( p, self ) ) {
                        t.trap = tr.loadid.id().str();
                    }
                    // DDA MP v4.0a-114: the field the host's renderer draws
                    // here (displayed_field_type is the same choice the ascii
                    // draw makes; intensity selects its sprite).
                    const field &fld = here.field_at( p );
                    if( fld.field_count() > 0 ) {
                        const field_type_id &ft = fld.displayed_field_type();
                        const field_entry *fe = ft ? fld.find_field( ft, false ) : nullptr;
                        if( fe != nullptr ) {
                            t.field = ft.id().str();
                            t.field_intensity = static_cast<uint8_t>(
                                                    std::min( fe->get_field_intensity(), 255 ) );
                        }
                    }
                    monster *m = get_creature_tracker().creature_at<monster>( p );
                    if( m != nullptr && m->type != nullptr && m->type->id.is_valid() ) {
                        t.critter = m->type->id.str();
#if defined(TILES)
                        // DDA MP v3.8: facing flip and effect overlays the
                        // host's own renderer would draw.
                        if( tilecontext ) {
                            int rot = -2;
                            std::vector<std::string> ovls;
                            tilecontext->netplay_probe_monster( *m, rot, ovls );
                            t.critter_rot = static_cast<int8_t>( rot );
                            for( size_t oi = 0; oi < ovls.size(); oi++ ) {
                                if( oi != 0 ) {
                                    t.critter_overlays += ';';
                                }
                                t.critter_overlays += ovls[oi];
                            }
                            // DDA MP v3.8.1: the attitude icon the host's
                            // draw_critter_at paints last (fixed light, no
                            // flip). Bit0 = sees the host viewer (after the
                            // INATTENTIVE check), high bits = Attitude.
                            if( show_creature_overlay_icons ) {
                                const bool sees_p = m->sees( here, self ) &&
                                                    !self.has_trait( trait_INATTENTIVE );
                                t.critter_badge = static_cast<uint8_t>(
                                                      static_cast<int>( m->attitude_to( self ) ) << 1 |
                                                      ( sees_p ? 1 : 0 ) );
                            }
                            if( mp_light_trace_this ) {
                                mp_light_trace += string_format(
                                                      "HC f=%02d y=%02d x=%02d src=mon id=%s rot=%d badge=%d ovl=%s\n",
                                                      mp_light_trace_frame, dy, dx, t.critter.c_str(),
                                                      static_cast<int>( t.critter_rot ),
                                                      static_cast<int>( t.critter_badge ),
                                                      t.critter_overlays.c_str() );
                            }
                        }
#endif
                    } else {
                        // DDA MP: NPC sprites (non-hostile humans are not in
                        // the monster tracker as mon_* ids).
                        const Creature *cr = get_creature_tracker().creature_at( p );
                        const npc *n = cr != nullptr && cr->is_npc()
                                       ? static_cast<const npc *>( cr )
                                       : nullptr;
                        if( n != nullptr && !n->symbol().empty() ) {
#if defined(TILES)
                            // DDA MP v3.8: exact appearance (base id honored
                            // below, plus facing flip and clothing overlays).
                            std::string base_id;
                            std::vector<std::string> ovls;
                            if( tilecontext ) {
                                int rot = -2;
                                tilecontext->netplay_probe_character( *n, base_id, rot, ovls );
                                t.critter = n->male ? "npc_male" : "npc_female";
                                t.critter_rot = static_cast<int8_t>( rot );
                                for( size_t oi = 0; oi < ovls.size(); oi++ ) {
                                    if( oi != 0 ) {
                                        t.critter_overlays += ';';
                                    }
                                    t.critter_overlays += ovls[oi];
                                }
                                // DDA MP v3.8.1: attitude icon, same fact as
                                // draw_critter_at paints over the npc sprite.
                                if( show_creature_overlay_icons ) {
                                    const bool sees_p = n->sees( here, self ) &&
                                                        !self.has_trait( trait_INATTENTIVE );
                                    t.critter_badge = static_cast<uint8_t>(
                                                          static_cast<int>( n->attitude_to( self ) ) << 1 |
                                                          ( sees_p ? 1 : 0 ) );
                                }
                                if( mp_light_trace_this ) {
                                    mp_light_trace += string_format(
                                                          "HC f=%02d y=%02d x=%02d src=npc base=%s rot=%d badge=%d ovl=%s\n",
                                                          mp_light_trace_frame, dy, dx, base_id.c_str(),
                                                          static_cast<int>( t.critter_rot ),
                                                          static_cast<int>( t.critter_badge ),
                                                          t.critter_overlays.c_str() );
                                }
                            } else {
                                t.critter = n->male ? "npc_male" : "npc_female";
                            }
#else
                            t.critter = n->male ? "npc_male" : "npc_female";
#endif
                        }
                    }
                    // DDA MP: the host avatar is not tracked as a critter.
                    if( p == u.pos_bub() ) {
#if defined(TILES)
                        std::string base_id = u.male ? "player_male" : "player_female";
                        std::vector<std::string> ovls;
                        if( tilecontext ) {
                            int rot = -2;
                            tilecontext->netplay_probe_character( u, base_id, rot, ovls );
                            t.critter_rot = static_cast<int8_t>( rot );
                            for( size_t oi = 0; oi < ovls.size(); oi++ ) {
                                if( oi != 0 ) {
                                    t.critter_overlays += ';';
                                }
                                t.critter_overlays += ovls[oi];
                            }
                            if( mp_light_trace_this ) {
                                mp_light_trace += string_format(
                                                      "HC f=%02d y=%02d x=%02d src=ava base=%s rot=%d ovl=%s\n",
                                                      mp_light_trace_frame, dy, dx, base_id.c_str(),
                                                      static_cast<int>( t.critter_rot ),
                                                      t.critter_overlays.c_str() );
                            }
                        }
                        t.critter = base_id;
#else
                        t.critter = u.male ? "player_male" : "player_female";
#endif
                    }
                    {
                        const maptile &tile = here.maptile_at( p );
                    if( tile.get_item_count() > 0
                        && here.field_at( p ).displayed_field_type().obj().display_items ) {
                        // DDA MP v3.9.1: the tileset's item-layer sprites (the
                        // "layering.json" contexts) draw for ANY rendered cell
                        // with matching items - draw_field_or_item gates the
                        // layers on plain cell visibility only. Items seen as
                        // plain sprites need the stricter sees_some_items
                        // radius (that is why the host shows the dish rack
                        // content from farther away than the item itself).
                        if( here.sees_some_items( p, self ) ) {
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
                            // DDA MP v3.9.1: the variant the host renderer
                            // draws this item with (draw_item passes
                            // itype_variant().id into the tile lookup).
                            t.item_variant = itm.has_itype_variant()
                                             ? itm.itype_variant().id : "";
                        }
                        // DDA MP v3.9.1: stream the layer draws resolved + the
                        // skip-top fact for the seen-item case.
                        bool drawtop = true;
                        std::vector<cata_tiles::netplay_item_layer_draw> layers;
                        tilecontext->netplay_probe_item_layers( p, drawtop, layers );
                        for( const auto &d : layers ) {
                            if( t.item_layers.size() >= 255 ) {
                                break;
                            }
                            tile_item_layer l;
                            l.sprite = d.sprite;
                            l.variant = d.variant;
                            l.dx = static_cast<int8_t>( d.dx );
                            l.dy = static_cast<int8_t>( d.dy );
                            t.item_layers.push_back( l );
                        }
                        if( !drawtop ) {
                            t.flags |= 64;
                        }
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
                } else if( viewer == nullptr ) {
                    // Unseen: replay the host player's map memory the same way
                    // cata_tiles::get_terrain_memory_at family would. Only the
                    // host has this memory; an avatar client shows darkness
                    // instead (or its OWN memory, below).
                    mp_render_memory( u.get_memorized_tile( here.get_abs( p ) ), t );
                } else if( npc_viewer != nullptr ) {
                    // DDA MP v4.0a-2: unseen tiles replay the avatar's OWN
                    // host-stored memory (fresh tiles not yet seen stay dark).
                    const map_memory &mm = av_mem_for( *npc_viewer );
                    const memorized_tile &mt = mm.get_tile( here.get_abs( p ) );
                    const bool known = !mt.get_ter_id().empty() || !mt.get_dec_id().empty();
                    if( known ) {
                        mp_render_memory( mt, t );
                        t.light = static_cast<uint8_t>( static_cast<int>( lit_level::MEMORIZED ) );
                    }
                }
            }
            out.tiles.push_back( std::move( t ) );
        }
    }
    if( mp_netplay_diag && mp_light_trace_this && !mp_light_trace.empty() ) {
        if( std::FILE *f = netplay::mp_diag_open( "netplay_light_trace.log" ) ) {
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

static void build_host_snapshot( snapshot &out, const tripoint_bub_ms &center,
                                 npc *viewer = nullptr )
{
    avatar &u = get_avatar();
    map &here = get_map();

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
    // DDA MP v4.0a-2: for a bound client's own POV, recast the LOS/light
    // caches from THAT avatar's position (build_map_cache only casts from
    // the host player) and recompute its visibility caches, so tiles, light
    // texels and critters follow its eyes instead of the host player's. The
    // engine's dirty flags re-cast for the host player on its own next draw.
    if( viewer != nullptr ) {
        here.build_map_cache_for( center.z(), false, *viewer );
        // map::draw (just above) marks unseen-but-remembered cells as MEMORIZED
        // in the shared visibility cache. update_visibility_cache() skips the
        // rebuild when its inputs did not change, so those markers used to leak
        // into the snapshot: the client then drew the whole frame from memory
        // (all-black light) instead of the avatar's own view.
        here.invalidate_visibility_cache();
        here.update_visibility_cache( center.z(), viewer );
    }
    if( mp_netplay_diag ) {
        // Compact per-snapshot visibility histogram: shows whether the frame is
        // being mined from the avatar's view or from map memory.
        static int mp_snap_no = 0;
        ++mp_snap_no;
        if( mp_snap_no <= 40 || mp_snap_no % 20 == 0 ) {
            const auto &vc = here.get_cache_ref( center.z() ).visibility_cache;
            int hist[8] = {};
            for( int dy = 0; dy < FRAMEH; dy++ ) {
                for( int dx = 0; dx < FRAMEW; dx++ ) {
                    const tripoint_bub_ms p( center + tripoint_rel_ms( dx - FRAMEW / 2,
                                             dy - FRAMEH / 2, 0 ) );
                    if( here.inbounds( p ) ) {
                        ++hist[static_cast<int>( vc[p.x()][p.y()] )];
                    }
                }
            }
            if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
                std::fprintf( f,
                              "snap #%d viewer=%d center=%d,%d raw=%d,%d ll 0=%d 1=%d 2=%d 3=%d 4=%d 5=%d 6=%d\n",
                              mp_snap_no, viewer != nullptr ? 1 : 0, center.x(), center.y(),
                              viewer != nullptr ? viewer->pos_bub().x() : -1,
                              viewer != nullptr ? viewer->pos_bub().y() : -1,
                              hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6] );
                std::fclose( f );
            }
        }
    }
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

    // DDA MP v4.0a-2: the avatar's own heard sounds ("Where is that noise
    // coming from?" markers) — computed on the host from the same event
    // queue the host player's "?" markers come from, but against the
    // avatar's ears and eyes. Painted straight into the ascii frame and
    // streamed as a per-cell overlay id for the tileset replay renderer
    // (applied right after mine_tiles built the sprite payload below);
    // positions are deterministic, so the marker does not jump between
    // snapshots while the sound stays pending.
    std::vector<std::pair<std::size_t, uint8_t>> av_tile_markers;
    if( viewer != nullptr ) {
        std::vector<tripoint_bub_ms> av_markers;
        sounds::process_avatar_markers( viewer, av_markers );
        for( const tripoint_bub_ms &mk : av_markers ) {
            const tripoint_rel_ms rel = mk - center;
            const int dx = rel.x() + FRAMEW / 2;
            const int dy = rel.y() + FRAMEH / 2;
            if( dx < 0 || dy < 0 || dx >= FRAMEW || dy >= FRAMEH ) {
                continue;
            }
            const int glyph = mk.z() > center.z() ? '^' : mk.z() < center.z() ? 'v' : '?';
            mvwputch( win, point( dx, dy ), c_yellow, glyph );
            const std::size_t frame_idx = static_cast<std::size_t>( dy ) * FRAMEW + dx;
            av_tile_markers.emplace_back( frame_idx, mk.z() > center.z() ? 2 :
                                          mk.z() < center.z() ? 3 : 1 );
        }
    }

    // Read back the character grid.
    out.width = FRAMEW;
    out.height = FRAMEH;
    out.cells.reserve( FRAMEW * FRAMEH );
    // DDA MP v4.0a-4: stream the frame's absolute world origin — the pivot
    // cell is the client avatar's own cell, so the streamed cell coordinates
    // change every step; random-weighted sprite variants seed off position,
    // and this offset keeps them stable per world tile.
    {
        const tripoint_abs_ms oabs = here.get_abs( center );
        out.has_origin = true;
        out.origin_x = oabs.raw().xy().x;
        out.origin_y = oabs.raw().xy().y;
        out.origin_z = center.z();
    }
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
        mine_tiles( out, u, viewer, here, center );
        // DDA MP v4.0a-2: stamp the heard-sound overlay ids on top of the
        // freshly mined sprite payload.
        if( out.tiles.size() == out.cells.size() ) {
            for( const auto &mk : av_tile_markers ) {
                out.tiles[mk.first].marker = mk.second;
            }
        }
    }
#endif

    // Restore the tiles renderer before the window is destructed (RAII).
#if defined(TILES)
    use_tiles = prev_tiles;
#endif

    out.status_line = status_line_for( u );
    // DDA MP v4.2: the client's native renderer derives light and the clock
    // from the same world state, so ship the host's clock and weather.
    out.has_time = true;
    out.turn = to_turn<int>( calendar::turn );
    out.weather = get_weather().weather_id.str();
    // DDA MP v4.2: raw creature state for the native renderer. Only what the
    // session avatar can see (or everything for a watcher), and only within
    // the streamed frame + a small margin.
    auto mp_in_frame = [&]( const tripoint_bub_ms & p ) {
        const tripoint_rel_ms rel = p - center;
        return std::abs( rel.x() ) <= FRAMEW / 2 + 2 &&
               std::abs( rel.y() ) <= FRAMEH / 2 + 2;
    };
    for( monster &mon : g->all_monsters() ) {
        if( mon.is_dead_state() || !here.inbounds( mon.pos_bub() ) ||
            !mp_in_frame( mon.pos_bub() ) ) {
            continue;
        }
        if( viewer != nullptr && !viewer->sees( here, mon.pos_bub() ) ) {
            continue;
        }
        std::ostringstream os;
        JsonOut js( os );
        mon.serialize( js );
        const tripoint_abs_ms ap = mon.pos_abs();
        out.creatures.push_back( { 0, ap.x(), ap.y(), ap.z(), os.str() } );
    }
    for( npc &guy : g->all_npcs() ) {
        if( &guy == viewer || !here.inbounds( guy.pos_bub() ) ||
            !mp_in_frame( guy.pos_bub() ) ) {
            continue;
        }
        if( viewer != nullptr && !viewer->sees( here, guy.pos_bub() ) ) {
            continue;
        }
        std::ostringstream os;
        JsonOut js( os );
        guy.serialize( js );
        const tripoint_abs_ms ap = guy.pos_abs();
        out.creatures.push_back( { 1, ap.x(), ap.y(), ap.z(), os.str() } );
    }
    // DDA MP v4.2: the host player's own avatar is not in the NPC list or the
    // creature tracker, so ship it separately as kind 2 for the client's
    // native renderer.
    if( here.inbounds( u.pos_bub() ) && mp_in_frame( u.pos_bub() ) &&
        ( viewer == nullptr || viewer->sees( here, u.pos_bub() ) ) ) {
        std::ostringstream os;
        JsonOut js( os );
        u.serialize( js );
        const tripoint_abs_ms ap = u.pos_abs();
        out.creatures.push_back( { 2, ap.x(), ap.y(), ap.z(), os.str() } );
    }
}

/* DDA MP v4.0a-2: one snapshot per client session, centered on that
 * client's own avatar (the host player's view when the session has no
 * bound avatar). The frame is always FRAMEW*FRAMEH around the center, so
 * the wire format and the client's renderer need no changes: the client's
 * avatar simply stays painted at the frame's exact center. */
namespace {
/* registry helpers are defined in the avatar-registry section below */
shared_ptr_fast<npc> av_find_npc( int npc_id );
bool av_npc_alive( const shared_ptr_fast<npc> &p );
}
/* session id -> the npc id of the avatar that session drives (see the
 * avatar-registry section; main thread only) */
static std::map<int, int> av_sid_to_npc;
/* DDA MP v4.0a-18: per-session item locations matching the last INVENTORY
 * payload row order. Client actions reference a row index; only the host
 * main thread rebuilds/consumes this map. */
static std::map<int, std::vector<item_location>> av_inv_nodes;
static std::map<int, int> av_inv_gen;
/* DDA MP v4.0a-90: ground rows for the client pickup selector. The rows are
 * described by absolute tile + itype id, not item_location: the listing can sit
 * on the client for a while while the host's bubble is somewhere else, and a
 * stored item_location would be invalidated by the submap unload. */
struct av_pick_row {
    tripoint_abs_ms pos;
    std::string type;
};
static std::map<int, std::vector<av_pick_row>> av_pick_nodes;
static std::map<int, int> av_pick_gen;
/* DDA MP v4.0a-97: generation of the pending "which tile?" question per session. */
static std::map<int, int> av_pick_tile_gen;

/* the living avatar npc a session is bound to, or nullptr */
static npc *av_session_npc( int sid )
{
    const auto it = av_sid_to_npc.find( sid );
    if( it != av_sid_to_npc.end() ) {
        shared_ptr_fast<npc> npc_ptr = av_find_npc( it->second );
        if( npc_ptr && av_npc_alive( npc_ptr ) ) {
            return npc_ptr.get();
        }
    }
    return nullptr;
}

void host_relay_sees_message( const tripoint_bub_ms &target, const std::string &text,
                              int msg_type )
{
    if( !host_is_running() ) {
        return;
    }
    const map &here = get_map();
    if( !here.inbounds( target ) ) {
        return;
    }
    for( int sid : host_session_ids() ) {
        npc *av = av_session_npc( sid );
        if( av != nullptr && here.inbounds( av->pos_bub() ) &&
            av->sees( here, target ) ) {
            host_send_event_to_sid( sid, text, msg_type );
        }
    }
}

void host_relay_sees_message( const Creature &target, const std::string &text, int msg_type )
{
    if( !host_is_running() ) {
        return;
    }
    const map &here = get_map();
    for( int sid : host_session_ids() ) {
        npc *av = av_session_npc( sid );
        if( av != nullptr && here.inbounds( av->pos_bub() ) &&
            av->sees( here, target ) ) {
            host_send_event_to_sid( sid, text, msg_type );
        }
    }
}

static tripoint_bub_ms av_session_center( int sid )
{
    if( npc *n = av_session_npc( sid ) ) {
        return n->pos_bub();
    }
    return get_avatar().pos_bub();
}

/* DDA MP v4.0a-141 (bubble split, step 2): live monster preservation across
 * reality-bubble moves. The engine drops monsters that leave the bubble; for
 * multiple regions (or two players far apart inside one map) they must survive
 * the move and come back when their area is loaded again. The store is keyed
 * by the monster's stable id and keeps its absolute position and serialized
 * state; only monsters inside the new bubble are re-added, so the tracker
 * never holds off-region creatures. */
namespace
{
struct mp_saved_monster {
    int ax = 0;
    int ay = 0;
    int az = 0;
    std::string json;
};
std::vector<mp_saved_monster> mp_region_monster_store;
} // namespace

void host_capture_region_monsters()
{
    if( !host_is_running() ) {
        return;
    }
    for( const shared_ptr_fast<monster> &m : get_creature_tracker().get_monsters_list() ) {
        if( !m || m->is_dead_state() ) {
            continue;
        }
        const tripoint_abs_ms ap = m->pos_abs();
        std::ostringstream os;
        try {
            JsonOut js( os );
            m->serialize( js );
        } catch( ... ) {
            continue;
        }
        mp_saved_monster sm;
        sm.ax = ap.x();
        sm.ay = ap.y();
        sm.az = ap.z();
        sm.json = os.str();
        mp_region_monster_store.push_back( std::move( sm ) );
    }
}

void host_restore_region_monsters()
{
    if( !host_is_running() ) {
        return;
    }
    map &here = get_map();
    creature_tracker &tracker = get_creature_tracker();
    std::vector<mp_saved_monster> keep;
    keep.reserve( mp_region_monster_store.size() );
    for( const mp_saved_monster &sm : mp_region_monster_store ) {
        const tripoint_abs_ms ap( sm.ax, sm.ay, sm.az );
        const tripoint_bub_ms bub = here.get_bub( ap );
        if( !here.inbounds( bub ) ) {
            keep.push_back( sm );
            continue;
        }
        // Still live (the shift did not unload it): do not duplicate it.
        bool live = false;
        for( const shared_ptr_fast<monster> &m : tracker.get_monsters_list() ) {
            if( m && m->pos_abs() == ap ) {
                live = true;
                break;
            }
        }
        if( live ) {
            continue;
        }
        try {
            JsonValue jv = json_loader::from_string( sm.json );
            auto mon = std::make_shared<monster>();
            mon->deserialize( jv.get_object() );
            mon->setpos( ap, false );
            tracker.add( mon );
        } catch( ... ) {
            // Broken saved state: drop it rather than blocking the bubble.
        }
    }
    mp_region_monster_store = std::move( keep );
}
bool host_relay_character_message( const Character *who, const std::string &text,
                                   int msg_type )
{
    if( who == nullptr || !who->is_npc() || !host_is_running() ) {
        return false;
    }
    for( const auto &entry : av_sid_to_npc ) {
        if( av_session_npc( entry.first ) == who ) {
            host_send_event_to_sid( entry.first, text, msg_type );
            return true;
        }
    }
    return false;
}

static void av_send_session_picture( int sid, const tripoint_bub_ms &center,
                                     npc *viewer = nullptr )
{
    if( !get_map().inbounds( center ) ) {
        return;
    }
    snapshot snap;
    build_host_snapshot( snap, center, viewer );
    if( netplay::mp_diag_enabled() ) {
        const long long wms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch() ).count();
        const tripoint_bub_ms vp = viewer != nullptr ? viewer->pos_bub() : get_avatar().pos_bub();
        if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
            std::fprintf( f, "srv snap wms=%lld sid=%d center=%d,%d viewer=%d,%d\n",
                          wms, sid, center.x(), center.y(), vp.x(), vp.y() );
            std::fclose( f );
        }
    }
    netplay::host_send_snapshot_to_sid( sid, snap );
}

void host_game_tick()
{
    if( !host_is_running() || host_session_count() == 0 ) {
        return;
    }
    if( !get_map().inbounds( get_avatar().pos_bub() ) ) {
        return; // no world yet
    }
    for( int sid : netplay::host_session_ids() ) {
        npc *viewer = av_session_npc( sid );
        if( viewer != nullptr && !get_map().inbounds( viewer->pos_bub() ) ) {
            // DDA MP v4.0a-2: the avatar is outside the reality bubble right
            // now (the bubble lives with the host player between steps; the
            // avatar holds it only during its own step). There is no valid
            // map around it, so keep the client's last painted frame instead
            // of painting darkness over it.
            continue;
        }
        tripoint_bub_ms center = av_session_center( sid );
        bool mp_re_anchored = false;
        if( viewer != nullptr ) {
            // DDA MP v4.0a-6: the streamed frame must fit inside the map.
            // Between the avatar's own steps the bubble lives with the host
            // player, and a roaming avatar ends near the bubble edge — the
            // frame's outer cells then mine outside of the map and arrive
            // empty: the black edge-ring that only cleared after the client
            // stepped onto the dark area. Before building, re-anchor the
            // reality bubble on the avatar with the same light anchor the
            // step pump uses, then hand it back so the host player keeps its
            // own vanilla anchor between snapshots.
            if( center.x() < FRAMEW / 2 || center.y() < FRAMEH / 2 ||
                center.x() + FRAMEW / 2 >= MAPSIZE_X ||
                center.y() + FRAMEH / 2 >= MAPSIZE_Y ) {
                g->mp_anchor_map_on( *viewer, true );
                center = viewer->pos_bub();
                mp_re_anchored = true;
            }
        }
        av_send_session_picture( sid, center, viewer );
        if( mp_re_anchored ) {
            g->mp_anchor_map_on( get_avatar() );
        }
    }
}

/* DDA MP v3.9: the host streams snapshots only on its own turn, so an idle
 * host would leave a freshly-loaded client staring at its menu until somebody
 * pressed a key. The client polls with SNAP_REQ instead; the reader threads
 * must not touch game state, so this consumes the flag on the host main
 * thread and answers with the current picture. No-op without a request. */
static void av_send_status( int sid );

void host_pump_snapshot_requests(){
    if( !host_pop_snapshot_request() ) {
        return;
    }
    host_game_tick();
    // DDA MP v4.0a-9: a freshly joined client asks for its first picture right
    // away; push the sidebar and sheet along with it instead of waiting for
    // the first world turn.
    for( int sid : netplay::host_session_ids() ) {
        av_send_status( sid );
    }
}

/* ------------------------------------------------------------------ */
/* remote input                                                        */
/* ------------------------------------------------------------------ */

/* DDA MP v4.0a-2: wire key code -> engine action name (shared by the
 * legacy watcher injection and the per-session avatar routing). */
static std::string mp_key_action( uint32_t code )
{
    switch( code ) {
        case 1:
            return "LEFT";
        case 2:
            return "RIGHT";
        case 3:
            return "UP";
        case 4:
            return "DOWN";
        case 10:
            return "CONFIRM";
        case 11:
            // DDA MP v4.0a-2: one clean engine turn ("pause" == . / 5): the
            // host player stalls while the calendar/weather/NPC AI run the
            // normal turn pipeline
            return "pause";
        case 12:
            return "RIGHTUP";
        case 13:
            return "RIGHTDOWN";
        case 14:
            return "LEFTUP";
        case 15:
            return "LEFTDOWN";
        case 20:
            // DDA MP v4.0a-14: pass the turn without moving.
            return "WAIT";
        default:
            return {};
    }
}

bool host_pop_action( std::string &action )
{
    uint32_t code = 0;
    if( !host_pop_key( code ) ) {
        return false;
    }
    action = mp_key_action( code );
    return !action.empty();
}

/* ------------------------------------------------------------------ */
/* DDA MP v4.0: per-session avatars                                    */
/* ------------------------------------------------------------------ */

namespace {

static const efftype_id effect_sleep( "sleep" );

/* one netplay-controlled avatar in the host's world: we only need its
 * display name (the key clients use to resume) and the character_id of
 * the world npc that carries the avatar */
struct av_rec {
    std::string name;
    int npc_id = 0;
};

bool av_registry_path( cata_path &out )
{
    if( world_generator == nullptr || world_generator->active_world == nullptr ) {
        return false;
    }
    out = world_generator->active_world->folder_path() / "netplay_avatars.json";
    return true;
}

shared_ptr_fast<npc> av_find_npc( int npc_id )
{
    if( npc_id <= 0 ) {
        return nullptr;
    }
    return overmap_buffer.find_npc( character_id( npc_id ) );
}

bool av_npc_alive( const shared_ptr_fast<npc> &p )
{
    return p && !p->is_dead();
}

/* The registry stores npc ids, but a restarted host resets its id counter
 * and ids in the file can go stale. Every avatar npc carries the world
 * marker "netplay_avatar:<nick>", which is the real source of truth. */
shared_ptr_fast<npc> av_find_marked_npc( const std::string &nick )
{
    const std::string marker = "netplay_avatar:" + nick;
    for( auto &elem : overmap_buffer.get_npcs_near_player( 600 ) ) {
        if( elem && av_npc_alive( elem ) && elem->get_unique_id() == marker ) {
            return elem;
        }
    }
    return nullptr;
}

std::vector<av_rec> av_registry_load();
void av_registry_save( const std::vector<av_rec> &recs );

std::vector<av_rec> av_registry_load()
{
    std::vector<av_rec> out;
    cata_path path;
    if( !av_registry_path( path ) ) {
        return out;
    }
    std::string text;
    // optional: a fresh world has no registry yet and that is not an error —
    // a blocking debug error panel here freezes the host main loop and makes
    // the connecting client miss its avatar answer.
    read_from_file_optional( path, [&]( std::istream &is ) {
        try {
            std::ostringstream ss;
            ss << is.rdbuf();
            JsonValue jsin = json_loader::from_string( ss.str() );
            JsonArray arr = jsin.get_array();
            while( arr.has_more() ) {
                JsonObject jo = arr.next_object();
                av_rec rec;
                jo.read( "name", rec.name );
                jo.read( "npc_id", rec.npc_id );
                if( rec.name.empty() || rec.npc_id <= 0 ) {
                    continue;
                }
                // older hosts restarted with a reset id counter and could
                // save several entries pointing at the same npc id; the
                // first wins, the rest are duplicates
                bool dup = false;
                for( const av_rec &prev : out ) {
                    if( prev.npc_id == rec.npc_id ) {
                        dup = true;
                        break;
                    }
                }
                if( dup ) {
                    continue;
                }
                out.push_back( rec );
            }
        } catch( const JsonError & ) {
            // corrupt registry: treat as empty, it will be rewritten on save
        }
    } );
    // v4.0a-1: prune entries whose world npc no longer exists or is dead (a
    // restarted host with a reset id counter would keep stale ids forever).
    std::vector<av_rec> live;
    for( const av_rec &rec : out ) {
        if( av_npc_alive( av_find_npc( rec.npc_id ) ) ) {
            live.push_back( rec );
        }
    }
    if( live.size() != out.size() ) {
        av_registry_save( live );
    }
    return live;
}

void av_registry_save( const std::vector<av_rec> &recs )
{
    cata_path path;
    if( !av_registry_path( path ) ) {
        return;
    }
    write_to_file( path, [&]( std::ostream &os ) {
        JsonOut json( os );
        json.start_array();
        for( const av_rec &rec : recs ) {
            json.start_object();
            json.member( "name", rec.name );
            json.member( "npc_id", rec.npc_id );
            json.end_object();
        }
        json.end_array();
    } );
}

void av_reply_deny( int sid, const std::string &why )
{
    host_send_avatar_reply( sid, frame::AVATAR_DENIED, encode_avatar_text( why ) );
}

} // namespace

/* DDA MP v4.0a-2: bind bookkeeping. av_sid_to_npc itself lives next to
 * av_session_center (snapshot section). Written on bind (resume/create),
 * consumed by the key pump on the same main thread — no locking needed.
 * Rebinding kicks the previous session off the avatar (last client with
 * the nick wins). */
static void av_bind_session( int sid, int npc_id )
{
    for( auto it = av_sid_to_npc.begin(); it != av_sid_to_npc.end(); ) {
        if( it->second == npc_id && it->first != sid ) {
            it = av_sid_to_npc.erase( it );
        } else {
            ++it;
        }
    }
    av_sid_to_npc[sid] = npc_id;
}

static void av_wake( const shared_ptr_fast<npc> &npc_ptr )
{
    npc_ptr->remove_effect( effect_sleep );
    npc_ptr->wake_up();
}

static void host_handle_one_avatar_op( const avatar_op &op )
{
    // The host must actually be running a game to add an avatar to the world.
    map &here = get_map();
    avatar &host = get_avatar();
    if( !here.inbounds( host.pos_bub() ) ) {
        av_reply_deny( op.sid, _( "Host is not running a game yet." ) );
        return;
    }

    std::vector<av_rec> recs = av_registry_load();

    if( op.kind == 1 ) {
        // Announce the join in the host's message log: "{Name} connected".
        // The list request arrives exactly once per connect, right after
        // the client finished loading the host's world data.
        add_msg( _( "%s connected." ), op.nick );

        // list request: report the avatar owned by the requesting nick. This
        // is also the self-heal point: if the registry's npc id went stale
        // but a marked npc exists, re-point the registry entry at it.
        std::vector<avatar_entry> list;
        for( const av_rec &rec : recs ) {
            if( rec.name != op.nick ) {
                continue;
            }
            shared_ptr_fast<npc> npc_ptr = av_find_npc( rec.npc_id );
            if( !av_npc_alive( npc_ptr ) ) {
                npc_ptr = av_find_marked_npc( op.nick );
                if( npc_ptr ) {
                    // heal the registry file (the resume/creates read it
                    // again below)
                    for( av_rec &fix : recs ) {
                        if( &fix == &rec ) {
                            fix.npc_id = npc_ptr->getID().get_value();
                        }
                    }
                    av_registry_save( recs );
                }
            }
            avatar_entry e;
            e.name = rec.name;
            e.alive = av_npc_alive( npc_ptr );
            // v4.0a-1: session ownership is not tracked yet; the host serves
            // one client at a time (the accept loop holds a single session).
            e.in_session = false;
            list.push_back( e );
        }
        host_send_avatar_reply( op.sid, frame::AVATAR_LIST, encode_avatar_list( list ) );
        return;
    }

    if( op.kind == 2 ) {
        // resume: bind this session to an existing living avatar owned by
        // this nick
        const av_rec *found = nullptr;
        for( const av_rec &rec : recs ) {
            if( rec.name == op.nick ) {
                found = &rec;
                break;
            }
        }
        const shared_ptr_fast<npc> npc_ptr = found ? av_find_npc( found->npc_id ) : nullptr;
        if( found == nullptr || !av_npc_alive( npc_ptr ) ) {
            av_reply_deny( op.sid,
                           string_format( _( "No living avatar for nick \"%s\"." ),
                                          op.nick ) );
            return;
        }
        // v4.0a-2: the client takes the wheel — wake the avatar up and
        // route this session's keystrokes to it.
        av_wake( npc_ptr );
        av_bind_session( op.sid, npc_ptr->getID().get_value() );
        host_send_avatar_reply( op.sid, frame::AVATAR_OK, encode_avatar_text( found->name ) );
        return;
    }

    if( op.kind == 3 ) {
        // create: one avatar per nick; a client that owns a live avatar must
        // resume it instead of recreating.
        if( op.nick.empty() || op.nick.size() > 40 ) {
            av_reply_deny( op.sid, _( "Invalid nickname." ) );
            return;
        }
        for( const av_rec &rec : recs ) {
            if( rec.name == op.nick && av_npc_alive( av_find_npc( rec.npc_id ) ) ) {
                av_reply_deny( op.sid,
                               _( "This nickname already owns a live avatar. "
                                  "Reconnect to resume it instead of creating a new one." ) );
                return;
            }
        }
        // rebuild the npc from the profile json (avatar::serialize output)
        shared_ptr_fast<npc> npc_ptr;
        try {
            JsonValue jv = json_loader::from_string( op.json );
            JsonObject jo = jv.get_object();
            // The profile was serialized from the client's avatar and carries
            // avatar-specific members npc::load never reads (profession and
            // friends); allow them to remain unconsumed.
            jo.allow_omitted_members();
            npc_ptr = make_shared_fast<npc>();
            npc_ptr->deserialize( jo );
            // DDA MP: player profiles carry no NPC class, leaving `myclass` at
            // NC_NONE; dialogue and related lookups then resolve an invalid NPC
            // template ("Tried to get invalid npc: NC_NONE"). Assign a valid
            // class so talking to NPCs works.
            if( npc_ptr->myclass.is_null() || npc_ptr->myclass == npc_class_id( "NC_NONE" ) ) {
                npc_ptr->myclass = npc_class_id( "NC_SHOPKEEP" );
            }
            if( !npc_ptr->get_fac_id().str().empty() ) {
                npc_ptr->set_fac( npc_ptr->get_fac_id() );
            }
        } catch( const JsonError &e ) {
            npc_ptr = nullptr;
            av_reply_deny( op.sid, string_format( _( "Bad avatar profile: %s" ), e.what() ) );
            return;
        }
        // find a free passable tile on a ring around the host avatar
        bool ok = false;
        tripoint_bub_ms spot;
        const tripoint_bub_ms hp = host.pos_bub();
        for( int r = 1; r <= 6 && !ok; ++r ) {
            for( int dy = -r; dy <= r && !ok; ++dy ) {
                for( int dx = -r; dx <= r && !ok; ++dx ) {
                    if( std::abs( dx ) != r && std::abs( dy ) != r ) {
                        continue; // walk the ring only
                    }
                    const tripoint_bub_ms cand( hp.x() + dx, hp.y() + dy, hp.z() );
                    if( !here.inbounds( cand ) || !here.passable( cand ) ||
                        get_creature_tracker().creature_at( cand, true ) ) {
                        continue;
                    }
                    spot = cand;
                    ok = true;
                }
            }
        }
        if( !ok ) {
            av_reply_deny( op.sid, _( "No free tile near the host avatar to spawn in." ) );
            return;
        }
        character_id new_id = g->assign_npc_id();
        // Protect against a reset id counter across host restarts: an npc
        // with this id may already be loaded in the overmap, keep drawing
        // until a free id shows up.
        while( av_find_npc( new_id.get_value() ) != nullptr ) {
            new_id = g->assign_npc_id();
        }
        npc_ptr->setID( new_id, true );
        npc_ptr->set_unique_id( "netplay_avatar:" + op.nick );
        // v4.0a-1: the avatar rests while its player is away; the a2 step
        // wakes it right away — the client starts driving it immediately.
        npc_ptr->add_effect( effect_sleep, time_duration::from_days( 3000 ) );
        npc_ptr->spawn_at_precise( here.get_abs( spot ) );
        overmap_buffer.insert_npc( npc_ptr );
        g->load_npcs();
        recs.push_back( av_rec{ op.nick, new_id.get_value() } );
        av_registry_save( recs );
        av_wake( npc_ptr );
        av_bind_session( op.sid, new_id.get_value() );
        host_send_avatar_reply( op.sid, frame::AVATAR_OK, encode_avatar_text( op.nick ) );
        return;
    }
}

void host_pump_avatar_ops()
{
    avatar_op op;
    int budget = 16;
    while( budget-- > 0 && host_pop_avatar_op( op ) ) {
        if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
            std::fprintf( f, "avatar op sid=%d kind=%d nick=%s\n", op.sid, op.kind, op.nick.c_str() );
            std::fclose( f );
        }
        host_handle_one_avatar_op( op );
    }
}

/* DDA MP v4.0a-2: per-session keyboard. Bound sessions steer their own
 * avatar — one step per movement key, standard collision rules (no forced
 * moves, no pathfinding); any non-movement key is swallowed for now.
 * Sessions without a bound avatar fall back onto the legacy queue so the
 * watchers keep the v3.x "drive the host player" behavior unchanged. */
/* DDA MP v4.0a-2: turn parity. The engine's world turn runs once every
 * bound avatar has stepped since the previous tick, so one player's movement
 * does not fast-forward the world under the others; the host player keeps
 * its own do_turn ticks whenever it acts. Flag map is main-thread only. */
static std::map<int, bool> av_acted_since_tick;
/* DDA MP v4.0a-7: per-session move pool, owned by netplay. The avatar NPC is
 * unloaded and reloaded whenever the reality bubble flips between the host and
 * the client, and the NPC loader resets `moves` to 100 on every load — the
 * engine pool alone therefore let a client walk without limit. */
static std::map<int, int> av_move_pool;
/* DDA MP v4.0a-7: session is holding an action that cannot pay until the next
 * world turn — do not retry (and re-hint) it every pump cycle. */
static std::map<int, bool> av_waiting_for_turn;

static bool av_all_acted()
{
    // Drop avatars whose session is gone: a client that disconnected between
    // turns must not block the parity gate for the remaining players.
    const std::vector<int> sessions = netplay::host_session_ids();
    for( auto it = av_sid_to_npc.begin(); it != av_sid_to_npc.end(); ) {
        if( std::find( sessions.begin(), sessions.end(), it->first ) == sessions.end() ) {
            av_acted_since_tick.erase( it->first );
            av_waiting_for_turn.erase( it->first );
            av_move_pool.erase( it->first );
            it = av_sid_to_npc.erase( it );
        } else {
            ++it;
        }
    }
    for( const auto &entry : av_sid_to_npc ) {
        // DDA MP v4.0a-88: an AFK (long wait) session must not block the
        // others; it does not take part in the turn round at all.
        if( netplay::host_session_afk( entry.first ) ) {
            continue;
        }
        // DDA MP v4.0a-128: a sleeping or busy avatar (activity in progress)
        // does not block the round either; the world turns while it rests.
        const npc *npc_ptr = av_session_npc( entry.first );
        if( npc_ptr == nullptr ) {
            // Dead or unbound avatar: it cannot act, do not block the others.
            continue;
        }
        if( npc_ptr->has_effect( effect_sleep ) || !npc_ptr->activity.is_null() ) {
            continue;
        }
        const auto it = av_acted_since_tick.find( entry.first );
        if( it == av_acted_since_tick.end() || !it->second ) {
            return false;
        }
    }
    return true;
}

/* DDA MP v4.0a-88: a session avatar did something (step or wait) since the
 * last world turn. */

void host_pump_avatar_keys()
{
    netplay::sid_key sk;
    int mp_steps = 0;
    while( netplay::host_pop_sid_key( sk ) ) {
        // DDA MP v4.0a-90: with long wait the step is refunded, so a queued
        // burst could move the avatar many tiles in one pump. Keep the burst
        // to one step per pump; the input loop calls this ~100/s.
        if( netplay::long_wait_active() && ++mp_steps > 1 ) {
            netplay::host_put_back_sid_key( sk );
            break;
        }
        // Never gate on the "waiting" hint flag: it must only suppress repeated
        // messages. Gating on it deadlocked both sides (host waits for the
        // client's order, the client's keys were being ignored).
        const auto it = av_sid_to_npc.find( sk.sid );
        if( it == av_sid_to_npc.end() ) {
            // watcher: legacy host player injection
            netplay::host_push_legacy_key( sk.code );
            continue;
        }
        shared_ptr_fast<npc> npc_ptr = av_find_npc( it->second );
        if( !av_npc_alive( npc_ptr ) ) {
            av_sid_to_npc.erase( it );
            av_acted_since_tick.erase( sk.sid );
            continue;
        }
        const std::string act = mp_key_action( sk.code );
        // DDA MP v4.0a-128: any key wakes a sleeping avatar / cancels its
        // current activity, like the stock game's "get up" handling.
        if( npc_ptr->has_effect( effect_sleep ) || !npc_ptr->activity.is_null() ) {
            npc_ptr->cancel_activity();
            if( npc_ptr->has_effect( effect_sleep ) ) {
                npc_ptr->remove_effect( effect_sleep );
            }
            netplay::host_send_event_to_sid(
                sk.sid, _( "You wake up." ), static_cast<int>( m_info ) );
            av_send_session_picture( sk.sid, npc_ptr->pos_bub(), npc_ptr.get() );
            continue;
        }
        if( act == "WAIT" ) {
            // Pass the turn: spend the pool, mark the order, refresh the picture.
            av_acted_since_tick[sk.sid] = true;
            av_move_pool[sk.sid] = 0;
            netplay::host_send_hover_to_sid( sk.sid, "" );
            av_send_session_picture( sk.sid, av_session_center( sk.sid ), npc_ptr.get() );
            continue;
        }
        static const std::map<std::string, tripoint_rel_ms> dirs = {
            { "LEFT",  tripoint_rel_ms( -1, 0, 0 ) },
            { "RIGHT", tripoint_rel_ms( 1, 0, 0 ) },
            { "UP",    tripoint_rel_ms( 0, -1, 0 ) },
            { "DOWN",  tripoint_rel_ms( 0, 1, 0 ) },
            { "LEFTUP",   tripoint_rel_ms( -1, -1, 0 ) },
            { "LEFTDOWN", tripoint_rel_ms( -1, 1, 0 ) },
            { "RIGHTUP",  tripoint_rel_ms( 1, -1, 0 ) },
            { "RIGHTDOWN", tripoint_rel_ms( 1, 1, 0 ) },
        };
        const auto dir_it = dirs.find( act );
        if( dir_it == dirs.end() ) {
            continue; // CONFIRM and unknown codes: swallow
        }
        map &here = get_map();
        // DDA MP diag: per-step timing by distance — the far-roaming client
        // slowdown needs facts about which stage grows with distance.
        const auto mp_t0 = std::chrono::steady_clock::now();
        auto mp_ms_since = []( const std::chrono::steady_clock::time_point &t ) {
            return static_cast<long long>(
                       std::chrono::duration_cast<std::chrono::milliseconds>(
                           std::chrono::steady_clock::now() - t ).count() );
        };
        // DDA MP v4.0a-2: the avatar roams the world like the host player
        // does, so keep the reality bubble anchored around it (the same way
        // the engine anchors it around the player after a move). This also
        // re-activates an avatar the bubble had scrolled away from. Light
        // mode: only the cheap shifts run, no cache/spawn work — a burst of
        // client steps must not pay the full anchor price on every step.
        g->mp_anchor_map_on( *npc_ptr, true );
        const long long mp_anchor_ms = mp_ms_since( mp_t0 );
        const tripoint_bub_ms mp_from = npc_ptr->pos_bub();
        const tripoint_bub_ms dst = mp_from + dir_it->second;
        // DDA MP v4.0a-132: driving. The movement keys steer/thrust the
        // vehicle instead of moving the avatar on foot.
        if( npc_ptr->controlling_vehicle ) {
            if( const optional_vpart_position vp = here.veh_at( npc_ptr->pos_bub() ) ) {
                vp->vehicle().pldrive( here, *npc_ptr, dir_it->second.x(),
                                       dir_it->second.y(), 0 );
            }
            av_move_pool[sk.sid] = std::max( 0, npc_ptr->get_moves() );
            npc_ptr->set_moves( av_move_pool[sk.sid] );
            av_acted_since_tick[sk.sid] = true;
            if( av_move_pool[sk.sid] <= 0 && !netplay::long_wait_active() ) {
                av_waiting_for_turn[sk.sid] = true;
                netplay::host_send_hover_to_sid( sk.sid, _( "Waiting for other players…" ) );
            } else {
                netplay::host_send_hover_to_sid( sk.sid, "" );
            }
            av_send_session_picture( sk.sid, npc_ptr->pos_bub(), npc_ptr.get() );
            break;
        }
        if( npc_ptr->in_vehicle ) {
            netplay::host_send_event_to_sid(
                sk.sid, _( "You are in a vehicle. Use the vehicle controls to get out." ),
                static_cast<int>( m_info ) );
            break;
        }
        // DDA MP v4.0a-116: walking into a creature is a melee attack, the
        // same rule game::plmove applies (a blocked step would silently drop
        // the key). The attack spends the avatar's own attack cost; the
        // netplay pool follows the engine's move points after the swing.
        {
            Creature *cr = get_creature_tracker().creature_at( dst );
            if( cr != nullptr && cr != npc_ptr.get() ) {
                auto pool_it = av_move_pool.find( sk.sid );
                if( pool_it == av_move_pool.end() ) {
                    pool_it = av_move_pool.insert( { sk.sid, npc_ptr->get_speed() } ).first;
                }
                if( pool_it->second <= 0 && !netplay::long_wait_active() ) {
                    if( !av_waiting_for_turn[sk.sid] ) {
                        av_waiting_for_turn[sk.sid] = true;
                        netplay::host_send_hover_to_sid( sk.sid, _( "Waiting for other players…" ) );
                    }
                    av_acted_since_tick[sk.sid] = true;
                    break;
                }
                const std::string target_name = cr->disp_name();
                const int hp_before = cr->get_hp();
                bool attacked = false;
                if( monster *mon = dynamic_cast<monster *>( cr ) ) {
                    attacked = npc_ptr->melee_attack( *mon, true );
                } else if( npc *other = dynamic_cast<npc *>( cr ) ) {
                    if( other->is_enemy() ) {
                        attacked = npc_ptr->melee_attack( *other, true );
                        other->make_angry();
                    }
                }
                if( attacked ) {
                    const int hp_after = cr->is_dead_state() ? 0 : cr->get_hp();
                    if( cr->is_dead_state() ) {
                        netplay::host_send_event_to_sid(
                            sk.sid, string_format( _( "You kill the %s!" ), target_name ),
                            static_cast<int>( m_good ) );
                    } else if( hp_after < hp_before ) {
                        netplay::host_send_event_to_sid(
                            sk.sid,
                            string_format( _( "You hit the %s for %d damage." ),
                                           target_name, hp_before - hp_after ),
                            static_cast<int>( m_good ) );
                    } else {
                        netplay::host_send_event_to_sid(
                            sk.sid, string_format( _( "You miss the %s." ), target_name ),
                            static_cast<int>( m_info ) );
                    }
                    av_move_pool[sk.sid] = std::max( 0, npc_ptr->get_moves() );
                    npc_ptr->set_moves( av_move_pool[sk.sid] );
                    av_acted_since_tick[sk.sid] = true;
                    if( av_move_pool[sk.sid] <= 0 && !netplay::long_wait_active() ) {
                        av_waiting_for_turn[sk.sid] = true;
                        netplay::host_send_hover_to_sid( sk.sid, _( "Waiting for other players…" ) );
                    } else {
                        netplay::host_send_hover_to_sid( sk.sid, "" );
                    }
                    av_send_session_picture( sk.sid, npc_ptr->pos_bub(), npc_ptr.get() );
                } else {
                    netplay::host_send_event_to_sid(
                        sk.sid, string_format( _( "You bump into %s." ), target_name ),
                        static_cast<int>( m_info ) );
                }
                break;
            }
        }
        // DDA MP v4.0a-118: bumping a closed door opens it, exactly like the
        // host player's walk. Without this the client's move was swallowed by
        // can_move_to() and doors could not be used at all.
        {
            const bool inside = !here.is_outside( npc_ptr->pos_bub() );
            if( here.open_door( *npc_ptr, dst, inside, /*check_only=*/true ) ) {
                if( here.open_door( *npc_ptr, dst, inside, /*check_only=*/false ) ) {
                    const int mp_open_cost = to_moves<int>( 1_seconds );
                    auto pool_it = av_move_pool.find( sk.sid );
                    if( pool_it == av_move_pool.end() ) {
                        pool_it = av_move_pool.insert( { sk.sid, npc_ptr->get_speed() } ).first;
                    }
                    pool_it->second = std::max( 0, pool_it->second - mp_open_cost );
                    npc_ptr->set_moves( pool_it->second );
                    av_acted_since_tick[sk.sid] = true;
                    netplay::host_send_event_to_sid(
                        sk.sid, _( "You open the door." ), static_cast<int>( m_info ) );
                    if( pool_it->second <= 0 && !netplay::long_wait_active() ) {
                        av_waiting_for_turn[sk.sid] = true;
                        netplay::host_send_hover_to_sid( sk.sid, _( "Waiting for other players…" ) );
                    } else {
                        netplay::host_send_hover_to_sid( sk.sid, "" );
                    }
                    av_send_session_picture( sk.sid, npc_ptr->pos_bub(), npc_ptr.get() );
                }
                break;
            }
        }
        // DDA MP v4.0a-2: a step pays the price the engine charges an NPC's
        // own walk move, out of the avatar's move pool (refilled every world
        // turn). Speed, terrain and obstacles all cost real move points, so
        // the player's keyrate is naturally limited to what the body can do
        // per turn instead of flooding the world with steps.
        int mp_step_cost = 0;
        {
            const tripoint_bub_ms pos = npc_ptr->pos_bub();
            const bool diag = trigdist && pos.x() != dst.x() && pos.y() != dst.y();
            mp_step_cost = npc_ptr->run_cost( here.combined_movecost( pos, dst ), diag );
            auto pool_it = av_move_pool.find( sk.sid );
            if( pool_it == av_move_pool.end() ) {
                pool_it = av_move_pool.insert( { sk.sid, npc_ptr->get_speed() } ).first;
            }
            // DDA MP v4.0a-11: a step may cost more than one turn's pool (bench,
            // dresser, rubble...). Vanilla lets the player spend what they have
            // for such a move; requiring cost <= pool made furniture impassable.
            if( pool_it->second <= 0 ) {
                // pool exhausted: the move waits for the next world turn;
                // the key is NOT eaten — the session keeps its single action
                // in flight until the pool can pay for it. Break, not
                // continue: the key is back at the front of its slot and
                // would spin this pump forever otherwise; the next pump run
                // (after the next world turn refills the pool) retries it.
                // The step cannot be paid: DROP the key (do not queue it for a
                // later turn — a delayed automatic move reads as "my input was
                // ignored and then happened by itself"). The player presses
                // again when the host's turn starts a new round.
                if( !netplay::long_wait_active() && !av_waiting_for_turn[sk.sid] ) {
                    av_waiting_for_turn[sk.sid] = true;
                    netplay::host_send_hover_to_sid( sk.sid, _( "Waiting for other players…" ) );
                }
                // No action point left for this order: the player is done for
                // this round. Do not block the host's turn on an exhausted
                // pool (that deadlocked both sides until a manual wait).
                av_acted_since_tick[sk.sid] = true;
                break;
            }
        }
        if( here.inbounds( dst ) && npc_ptr->can_move_to( dst ) ) {
            const tripoint_bub_ms mp_from = npc_ptr->pos_bub();
            npc_ptr->move_to( dst );
            // DDA MP v4.0a-7: actually spend the pool (owned by netplay — the
            // NPC loader resets the engine moves on every bubble flip).
            av_move_pool[sk.sid] = std::max( 0, av_move_pool[sk.sid] - mp_step_cost );
            if( netplay::long_wait_active() ) {
                // DDA MP v4.0a-90: while the host long-waits it takes no turns,
                // so it must not pace the moving player. The step is refunded
                // and the world time stands still.
                av_move_pool[sk.sid] += mp_step_cost;
            }
            npc_ptr->set_moves( av_move_pool[sk.sid] );
            // DDA MP v4.0a-11: same rule as the host's game::walk_move — tell the
            // client only when the destination tile is slower than the tile it
            // left ("Moving onto this %s is slow!"), never for leaving it.
            if( here.move_cost( dst ) > here.move_cost( mp_from ) ) {
                netplay::host_send_event_to_sid(
                    sk.sid, string_format( _( "Moving onto this %s is slow!" ),
                                           here.name( dst ) ),
                    static_cast<int>( m_warning ) );            }
            // DDA MP: this noise came from another player's avatar — the
            // host's "?" placement must check whether it sees the avatar,
            // not the tile it just left (which may peek around a corner).
            sounds::tag_last_sound_as_remote_player( npc_ptr->getID().get_value(), mp_from );
            // show the step to its own session immediately: without this the
            // movement would wait for the parity tick below
            av_send_session_picture( sk.sid, dst, npc_ptr.get() );
            if( netplay::mp_diag_enabled() ) {
                const long long mp_total_ms = mp_ms_since( mp_t0 );
                const int mp_dist = rl_dist( get_avatar().pos_bub(), npc_ptr->pos_bub() );
                    if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
                    const long long wms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                              std::chrono::system_clock::now().time_since_epoch() ).count();
                    std::fprintf( f, "srv step wms=%lld sid=%d dist=%d anchor=%lldms total=%lldms dst=%d,%d\n",
                                  wms, sk.sid, mp_dist, mp_anchor_ms, mp_total_ms,
                                  dst.x(), dst.y() );
                    std::fclose( f );
                }
            }
            // parity: this bind acted since the last world turn
            av_acted_since_tick[sk.sid] = true;
            // The step went through; tell the client whether its round is over.
            if( av_move_pool[sk.sid] <= 0 && !netplay::long_wait_active() ) {
                av_waiting_for_turn[sk.sid] = true;
                netplay::host_send_hover_to_sid( sk.sid, _( "Waiting for other players…" ) );
            } else {
                netplay::host_send_hover_to_sid( sk.sid, "" );
            }
            // DDA MP v4.0a-7: the shared world turn is NOT synthesized here
            // any more. The client's step spends its move pool, and the world
            // advances only when the host takes a turn — so a client can never
            // act twice while the host stands still (and the host's own turn
            // is gated on the client having acted).
            break;
        }
    }
    // DDA MP v4.0a-2: when the burst is over, hand the bubble back to the
    // host player so its own turn machinery (input, rendering, do_turn) always
    // runs vanilla. DDA MP v4.0a-138: always restore it — leaving the bubble
    // on a client while more keys were queued made the host render its own
    // screen from the client's POV, so the host player could fall outside the
    // bubble and disappear for a frame.
    g->mp_anchor_map_on( get_avatar() );
}

/* DDA MP v4.0a-8: raw key=value character data for the client's own sidebar.
 * The host only reads values; layout, labels and bars are the client's job, so
 * the client can restyle the panel without any host changes. */
static std::string build_character_sheet( npc &av )
{    std::string s;
    auto add = [&]( const char *k, int v ) {
        s += k;
        s += '=';
        s += std::to_string( v );
        s += '\n';
    };
    auto add_str = [&]( const char *k, const std::string &v ) {
        s += k;
        s += '=';
        s += v;
        s += '\n';
    };
    add_str( "name", av.get_name() );
    add( "hp", av.get_hp() );
    add( "hp_max", av.get_hp_max() );
    add( "stam", av.get_stamina() );
    add( "stam_max", av.get_stamina_max() );
    add( "pain", av.get_pain() );
    add( "sleepy", av.get_sleepiness() );
    add( "hunger", av.get_hunger() );
    add( "thirst", av.get_thirst() );
    add( "str", av.get_str() );
    add( "dex", av.get_dex() );
    add( "int", av.get_int() );
    add( "per", av.get_per() );
    add( "speed", av.get_speed() );
    add( "moves", av.get_moves() );
    add( "morale", av.get_morale_level() );
    // Body parts: current/maximum HP, the same rows the vanilla sidebar lists.
    {
        static const std::pair<const char *, const char *> parts[] = {
            { "head", "head" }, { "torso", "torso" },
            { "arm_l", "arm_l" }, { "arm_r", "arm_r" },
            { "leg_l", "leg_l" }, { "leg_r", "leg_r" },
        };
        for( const auto &pr : parts ) {
            const bodypart_id bp( pr.second );
            s += "hp_";
            s += pr.first;
            s += '=';
            s += std::to_string( av.get_part_hp_cur( bp ) );
            s += '/';
            s += std::to_string( av.get_part_hp_max( bp ) );
            s += '\n';
        }
    }
    // Active effects (ids) — the client shows them as a list.
    {
        std::string eff;
        for( const effect &e : av.get_effects() ) {
            if( !eff.empty() ) {
                eff += ';';
            }
            eff += e.get_id().str();
        }
        add_str( "effects", eff );
    }
    const item_location wielded = av.get_wielded_item();
    add_str( "wield", wielded ? wielded->tname() : std::string() );
    add( "wear", static_cast<int>( av.worn.size() ) );
    // World turn, so the client can age its message log in game time.
    add( "turn", to_turn<int>( calendar::turn ) );
    return s;
}

/* DDA MP v4.0a-9: render the host's real sidebar (the player's current panel
 * layout) for an arbitrary character — here the client's avatar NPC — into a
 * cell grid, so the client can show exactly the host's panels for its own
 * character (same widgets, same options, same mods). */
#if defined(TILES)
static void build_sidebar_snapshot( sidebar_frame &out, Character &who,
                                     int max_cols = 0, int max_rows = 0 )
{
    panel_manager &mgr = panel_manager::get_manager();
    const std::vector<window_panel> &panels = mgr.get_current_layout().panels();
    int width = 0;
    for( const window_panel &p : panels ) {
        if( p.render() && p.get_width() > width ) {
            width = p.get_width();
        }
    }
    if( width <= 0 ) {
        return;
    }
    if( max_cols > 0 ) {
        width = std::min( width, max_cols );
    }
    const int height = max_rows > 0 ? std::min( TERMY, max_rows ) : TERMY;
    out.width = width;
    out.height = height;
    out.cells.assign( static_cast<size_t>( width ) * height, sidebar_cell() );
    int y = 0;
    for( const window_panel &p : panels ) {
        if( !p.render() ) {
            continue;
        }
        // The message-log panel (height -2) mirrors the host's own log; a
        // remote player must not see the host's messages. The critter list is
        // likewise computed around the host player.
        if( p.get_height() == -2 ) {
            continue;
        }
        {
            const std::string mp_wid = p.get_widget().str();
            if( mp_wid.find( "monster" ) != std::string::npos ||
                mp_wid.find( "creature" ) != std::string::npos ||
                mp_wid.find( "critter" ) != std::string::npos ||
                mp_wid.find( "npc" ) != std::string::npos ) {
                continue;
            }
        }
        int h = p.get_height();
        h = std::min( h, height - y );
        if( h <= 0 ) {
            break;
        }
        if( p.toggle ) {
            catacurses::window w = catacurses::newwin( h, p.get_width(), point( 0, 0 ) );
            werase( w );
            p.draw( { who, w, p.get_widget() } );
            cata_cursesport::WINDOW *raw = w.get<cata_cursesport::WINDOW>();
            for( int yy = 0; yy < h && y + yy < height; yy++ ) {
                for( int xx = 0; xx < p.get_width() && xx < width; xx++ ) {
                    const cata_cursesport::cursecell &cc = raw->line[yy].chars[xx];
                    sidebar_cell &dst = out.cells[static_cast<size_t>( y + yy ) * width + xx];
                    dst.text = cc.ch;
                    dst.color = static_cast<int>( cc.FG );
                }
            }
        }
        y += h;
    }
}
#endif

/* DDA MP v4.0a-9: push the status payloads (character sheet + rendered
 * sidebar) for one session's avatar. Used on join and once per world turn. */
/* DDA MP v4.0a-12: minimap tiles are drawn from the base-colour index (the
 * same 0..15 encoding the client's cells use), so map nc_color here. */
static int mp_base_color_index( nc_color c )
{
    static const nc_color base16[] = {
        c_black, c_red, c_green, c_brown, c_blue, c_magenta, c_cyan,
        c_light_gray, c_dark_gray, c_light_red, c_light_green, c_yellow,
        c_light_blue, c_pink, c_light_cyan, c_white
    };
    for( int i = 0; i < 16; i++ ) {
        if( c == base16[i] ) {
            return i;
        }
    }
    return 8; // dark gray fallback
}

/* DDA MP v4.1: give every item the session avatar carries a stable id. The id
 * rides in the item's own variables, so it survives serialization into the
 * client's mirror and the client can name the exact host item while acting.
 * "mp_uid" is excluded from item stacking (item.cpp), so tagging never changes
 * whether two items merge. */
static void mp_tag_avatar_items( npc &av )
{
    static unsigned long long seq = 0;
    std::function<void( item_location )> tag = [&]( item_location loc ) {
        if( !loc ) {
            return;
        }
        if( loc->get_var( "mp_uid", std::string() ).empty() ) {
            loc->set_var( "mp_uid", string_format( "%d.%llu",
                                                   to_turn<int>( calendar::turn ), ++seq ) );
        }
        for( item *child : loc->get_contents().all_items_top() ) {
            if( child != nullptr ) {
                tag( item_location( loc, child ) );
            }
        }
    };
    for( item *it : av.inv_dump() ) {
        if( it == nullptr ) {
            continue;
        }
        tag( av.is_wielding( *it ) ? av.get_wielded_item() : item_location( av, it ) );
    }
}

static void av_send_status( int sid )
{
    const auto sid_it = av_sid_to_npc.find( sid );
    shared_ptr_fast<npc> npc_ptr;
    if( sid_it != av_sid_to_npc.end() ) {
        npc_ptr = av_find_npc( sid_it->second );
    }
    if( !npc_ptr || !av_npc_alive( npc_ptr ) || npc_ptr->is_dead_state() ) {
        return;
    }
    npc *av = npc_ptr.get();
    // DDA MP v4.0a-80: every payload below reads the map around the avatar
    // (sidebar widgets, compass, minimap, aim targets). Between the avatar's
    // own steps the bubble lives with the host player, so anchor it here,
    // keeping the shared pointer alive across the anchor, and hand it back.
    const bool mp_re_anchor = !get_map().inbounds( get_map().get_bub( av->pos_abs() ) );
    if( mp_re_anchor ) {
        g->mp_anchor_map_on( *av, true );
    }
    netplay::host_send_sheet_to_sid( sid, build_character_sheet( *av ) );
    // DDA MP v4.1: tag the avatar's items before the raw snapshot so the
    // client's mirror and the row listing carry the same stable ids.
    mp_tag_avatar_items( *av );
    // DDA MP v4.0a-27: the raw NPC saves the client's own @ menu; the client
    // deserializes it into a temp NPC and opens Character::disp_info().
    {
        static const bool mp_send_info = []() {
            const char *env = std::getenv( "DDA_MP_NO_INFO_JSON" );
            return env == nullptr || env[0] == '\0' || std::string( env ) == "0";
        }();
        // Serializing the whole NPC is comparatively expensive, but the client
        // runs its menus on this mirror, so every status push carries a fresh
        // raw snapshot (v4.1).
        if( mp_send_info ) {
            std::ostringstream os;
            JsonOut js( os );
            av->serialize( js );
            netplay::host_send_character_json_to_sid( sid, os.str() );
        }
    }
#if defined(TILES)
    {
        // DDA MP v4.0a-12: the host's pixel minimap colours MAP tiles (terrain,
        // furniture, vehicles) around the player, not overmap tiles. Send the
        // same data: a square of map tiles around the avatar.
        minimap_frame mm;
        constexpr int MP_R = 60; // 121x121 map tiles, like the host's pixel minimap
        mm.width = MP_R * 2 + 1;
        mm.height = MP_R * 2 + 1;
        map &here = get_map();
        const tripoint_bub_ms center = av->pos_bub();
        mm.color.reserve( static_cast<size_t>( mm.width ) * mm.height );
        mm.seen.reserve( static_cast<size_t>( mm.width ) * mm.height );
        for( int dy = -MP_R; dy <= MP_R; dy++ ) {
            for( int dx = -MP_R; dx <= MP_R; dx++ ) {
                const tripoint_bub_ms p = center + tripoint_rel_ms( dx, dy, 0 );
                nc_color col = c_black;
                // Light level from the same cache the host's minimap reads:
                // 0 = DARK/BLANK (not drawn), 1 = LOW (dimmed), 2 = lit.
                uint8_t light = 0;
                if( here.inbounds( p ) ) {
                    const lit_level ll = here.get_cache_ref( center.z() ).visibility_cache[p.x()][p.y()];
                    if( ll != lit_level::BLANK && ll != lit_level::DARK ) {
                        light = ll == lit_level::LOW ? 1 : 2;
                    }
                    if( const optional_vpart_position vp = here.veh_at( p ) ) {
                        col = vp->vehicle().get_display_of_tile( vp->mount_pos() ).color;
                    } else if( const furn_id &furn = here.furn( p ) ) {
                        col = furn->color();
                    } else {
                        col = here.ter( p )->color();
                    }
                }
                mm.color.push_back( static_cast<uint32_t>( mp_base_color_index( col ) ) );
                mm.seen.push_back( light );
            }
        }
        // Critter beacons (same idea as the host's pixel minimap diamonds).
        for( monster &mon : g->all_monsters() ) {
            if( !av->sees( here, mon.pos_bub() ) ) {
                continue;
            }
            const tripoint_bub_ms mp = mon.pos_bub();
            const int bx = mp.x() - center.x();
            const int by = mp.y() - center.y();
            if( std::abs( bx ) <= MP_R && std::abs( by ) <= MP_R ) {
                const monster_attitude matt = mon.attitude( av );
                const bool hostile = matt == MATT_ATTACK || matt == MATT_FOLLOW;
                minimap_frame::beacon be;
                be.dx = bx;
                be.dy = by;
                be.color = static_cast<uint32_t>( hostile ? 1 :
                                                mp_base_color_index( mon.symbol_color() ) );
                be.name = mon.get_name();
                be.sym = mon.symbol();
                be.is_character = 0;
                be.attitude = static_cast<int32_t>( mon.attitude_to( *av ) );
                be.sees_you = mon.sees( here, *av ) ? 1 : 0;
                be.hp_pct = mon.get_hp_max() > 0 ? 100 * mon.get_hp() / mon.get_hp_max() : 0;
                be.name_color = static_cast<uint8_t>( mp_base_color_index( mon.symbol_color() ) );
                mm.beacons.push_back( std::move( be ) );
            }
        }
        for( npc &guy : g->all_npcs() ) {
            if( &guy == av || !av->sees( here, guy.pos_bub() ) ) {
                continue;
            }
            const tripoint_bub_ms mp = guy.pos_bub();
            const int bx = mp.x() - center.x();
            const int by = mp.y() - center.y();
            if( std::abs( bx ) <= MP_R && std::abs( by ) <= MP_R ) {
                minimap_frame::beacon be;
                be.dx = bx;
                be.dy = by;
                be.color = static_cast<uint32_t>( mp_base_color_index( guy.symbol_color() ) );
                be.name = guy.get_name();
                be.sym = "@";
                be.is_character = 1;
                be.attitude = static_cast<int32_t>( guy.attitude_to( *av ) );
                be.npc_attitude = static_cast<int32_t>( guy.get_attitude() );
                be.sees_you = guy.sees( here, *av ) ? 1 : 0;
                be.hp_pct = guy.get_hp_max() > 0 ? 100 * guy.get_hp() / guy.get_hp_max() : 0;
                be.name_color = static_cast<uint8_t>( mp_base_color_index( guy.symbol_color() ) );
                if( guy.is_armed() ) {
                    be.wielded = guy.get_wielded_item()->tname();
                }
                mm.beacons.push_back( std::move( be ) );
            }
        }
        // The host player is an avatar, not an NPC — add it explicitly.
        {
            avatar &host_u = get_avatar();
            if( av->sees( here, host_u.pos_bub() ) ) {
                const tripoint_bub_ms mp = host_u.pos_bub();
                const int bx = mp.x() - center.x();
                const int by = mp.y() - center.y();
                if( std::abs( bx ) <= MP_R && std::abs( by ) <= MP_R ) {
                    minimap_frame::beacon be;
                    be.dx = bx;
                    be.dy = by;
                    be.color = static_cast<uint32_t>(
                                   mp_base_color_index( host_u.symbol_color() ) );
                    be.name = host_u.get_name();
                    be.sym = "@";
                    be.is_character = 1;
                    be.attitude = static_cast<int32_t>( host_u.attitude_to( *av ) );
                    be.npc_attitude = 0;
                    be.sees_you = 1;
                    be.hp_pct = host_u.get_hp_max() > 0 ?
                                100 * host_u.get_hp() / host_u.get_hp_max() : 0;
                    be.name_color = static_cast<uint8_t>(
                                        mp_base_color_index( host_u.symbol_color() ) );
                    if( host_u.is_armed() ) {
                        be.wielded = host_u.get_wielded_item()->tname();
                    }
                    mm.beacons.push_back( std::move( be ) );
                }
            }
        }
        // DDA MP: local-frame creature facts for the client aim panel.
        {
            std::vector<netplay::aim_target> targets;
            const tripoint_bub_ms ac = av->pos_bub();
            auto add_target = [&]( const Creature & cr, bool is_character ) {
                if( !av->sees( here, cr.pos_bub() ) ) {
                    return;
                }
                const tripoint_bub_ms cp = cr.pos_bub();
                const int dx = cp.x() - ac.x();
                const int dy = cp.y() - ac.y();
                if( std::abs( dx ) > 60 || std::abs( dy ) > 60 ) {
                    return;
                }
                netplay::aim_target t;
                t.dx = dx;
                t.dy = dy;
                t.is_character = is_character ? 1 : 0;
                t.attitude = static_cast<int32_t>( cr.attitude_to( *av ) );
                t.npc_attitude = 0;
                t.sees_you = cr.sees( here, *av ) ? 1 : 0;
                t.hp_pct = cr.get_hp_max() > 0 ? 100 * cr.get_hp() / cr.get_hp_max() : 0;
                t.dodge = static_cast<int32_t>( cr.get_dodge() );
                t.name_color = static_cast<uint8_t>(
                                   mp_base_color_index( cr.symbol_color() ) );
                t.name = cr.get_name();
                if( const npc *n = dynamic_cast<const npc *>( &cr ); n != nullptr ) {
                    t.is_character = 1;
                    t.npc_attitude = static_cast<int32_t>( n->get_attitude() );
                    if( n->is_armed() ) {
                        t.wielded = n->get_wielded_item()->tname();
                    }
                }
                targets.push_back( std::move( t ) );
            };
            for( monster &mon : g->all_monsters() ) {
                add_target( mon, false );
            }
            for( npc &guy : g->all_npcs() ) {
                if( &guy != av ) {
                    add_target( guy, true );
                }
            }
            {
                avatar &host_u = get_avatar();
                if( host_u.pos_bub() != ac && av->sees( here, host_u.pos_bub() ) ) {
                    netplay::aim_target t;
                    t.dx = host_u.pos_bub().x() - ac.x();
                    t.dy = host_u.pos_bub().y() - ac.y();
                    t.is_character = 1;
                    t.attitude = static_cast<int32_t>( host_u.attitude_to( *av ) );
                    t.sees_you = 1;
                    t.hp_pct = host_u.get_hp_max() > 0 ?
                               100 * host_u.get_hp() / host_u.get_hp_max() : 0;
                    t.name_color = static_cast<uint8_t>(
                                       mp_base_color_index( host_u.symbol_color() ) );
                    t.name = host_u.get_name();
                    if( host_u.is_armed() ) {
                        t.wielded = host_u.get_wielded_item()->tname();
                    }
                    targets.push_back( std::move( t ) );
                }
            }
            netplay::host_send_aim_targets_to_sid( sid, targets );
        }
        netplay::host_send_minimap_to_sid( sid, mm );
    }
#endif
    // DDA MP v4.1: no row listing is streamed any more. The client's mirror
    // names items by mp_uid, so inventory/throw/pocket intents address the
    // same host item without a synchronized row table.
    // DDA MP v4.0a-115: the rendered sidebar is not streamed either; the
    // client draws the same panels from its character mirror, so only raw
    // state crosses the wire.
    if( mp_re_anchor ) {
        g->mp_anchor_map_on( get_avatar() );
    }
}

/* DDA MP v4.1: resolve a stable item id on the session avatar. The id is
 * carried in the item's own variables, so it survives snapshots and container
 * moves; no row list has to be kept in sync. */
static item_location av_find_by_uid( npc &av, const std::string &uid )
{
    item_location found;
    std::function<void( item_location )> walk = [&]( item_location loc ) {
        if( found || !loc ) {
            return;
        }
        if( loc->get_var( "mp_uid", std::string() ) == uid ) {
            found = loc;
            return;
        }
        for( item *child : loc->get_contents().all_items_top() ) {
            if( child != nullptr ) {
                walk( item_location( loc, child ) );
            }
        }
    };
    for( item *it : av.inv_dump() ) {
        if( it == nullptr || found ) {
            continue;
        }
        walk( av.is_wielding( *it ) ? av.get_wielded_item() : item_location( av, it ) );
    }
    return found;
}

void host_pump_inventory_actions()
{
    if( !host_is_running() ) {
        return;
    }
    // DDA MP v4.0a-80: map-bound work below anchors the bubble on the session
    // avatar; hand it back to the host player when the pump is done.
    bool mp_anchored = false;
    for( int budget = 0; budget < 8; budget++ ) {
        int sid = 0;
        int node = 0;
        int action = 0;
        int generation = 0;
        std::vector<int> targets;
        netplay::inventory_uid_action ua;
        const bool have_uid = netplay::host_pop_inventory_uid( ua );
        if( have_uid ) {
            sid = ua.sid;
            action = ua.action;
        } else if( !netplay::host_pop_inventory_action( sid, node, action, generation, targets ) ) {
            break;
        }
        if( !have_uid && generation != av_inv_gen[sid] ) {
            // The row list the client acted on is stale; applying the index
            // could hit a different item.
            netplay::host_send_event_to_sid(
                sid, _( "The inventory changed. Reopen it and try again." ),
                static_cast<int>( m_info ) );
            continue;
        }
        shared_ptr_fast<npc> npc_ptr;
        {
            const auto sid_it = av_sid_to_npc.find( sid );
            if( sid_it != av_sid_to_npc.end() ) {
                npc_ptr = av_find_npc( sid_it->second );
            }
        }
        if( !npc_ptr || !av_npc_alive( npc_ptr ) ) {
            continue;
        }
        npc *av = npc_ptr.get();
        // Map-bound actions (drop, spill checks) must see the avatar's own
        // reality bubble, exactly like a movement key does. Keep the shared
        // pointer alive: anchoring can unload/reload tracker entries.
        g->mp_anchor_map_on( *av, true );
        mp_anchored = true;
        const auto it = av_inv_nodes.find( sid );
        item_location loc;
        std::vector<item_location> target_locs;
        if( have_uid ) {
            loc = av_find_by_uid( *av, ua.uids.empty() ? std::string() : ua.uids.front() );
            targets = ua.ints;
            for( std::size_t k = 1; k < ua.uids.size(); k++ ) {
                item_location src = av_find_by_uid( *av, ua.uids[k] );
                if( src ) {
                    target_locs.push_back( src );
                }
            }
            if( !loc ) {
                netplay::host_send_event_to_sid(
                    sid, _( "That item moved. Reopen the inventory and try again." ),
                    static_cast<int>( m_info ) );
                continue;
            }
            if( action == 'v' && !ua.json.empty() ) {
                try {
                    JsonValue jv = json_loader::from_string( ua.json );
                    std::vector<JsonObject> settings;
                    for( const JsonValue &entry : jv.get_array() ) {
                        settings.push_back( entry.get_object() );
                    }
                    std::size_t idx = 0;
                    for( item_pocket *pocket : loc->get_pockets( []( const item_pocket & ) {
                    return true;
                    } ) ) {
                        if( idx < settings.size() ) {
                            pocket->settings.deserialize( settings[idx] );
                        }
                        ++idx;
                    }
                    av_send_status( sid );
                } catch( const std::exception &e ) {
                    if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
                        std::fprintf( f, "pocket settings error sid=%d: %s\n", sid, e.what() );
                        std::fclose( f );
                    }
                }
                continue;
            }
        } else {
            if( it == av_inv_nodes.end() || node < 0 ||
                node >= static_cast<int>( it->second.size() ) ) {
                if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
                    std::fprintf( f, "sid=%d node=%d key=%c result=invalid-node\n",
                                  sid, node, static_cast<char>( action ) );
                    std::fclose( f );
                }
                continue;
            }
            loc = it->second[node];
        }
        if( !loc ) {
            continue;
        }
        const std::string item_id = loc->typeId().str();
        auto deny = [&]( const std::string & msg ) {
            if( !msg.empty() ) {
                netplay::host_send_event_to_sid( sid, msg, static_cast<int>( m_bad ) );
            }
        };
        bool changed = false;
        switch( action ) {
            case 'w': { // wield
                const ret_val<void> can = av->can_wield( *loc );
                if( !can.success() ) {
                    deny( can.str() );
                    break;
                }
                changed = av->wield( loc );
                break;
            }
            case 'W': { // wear
                const ret_val<void> can = av->can_wear( *loc );
                if( !can.success() ) {
                    deny( can.str() );
                    break;
                }
                changed = av->wear( loc, false ).has_value();
                break;
            }
            case 'T': { // take off
                const ret_val<void> can = av->can_takeoff( *loc );
                if( !can.success() ) {
                    deny( can.str() );
                    break;
                }
                changed = av->takeoff( loc );
                break;
            }
            case 'd': { // drop
                const ret_val<void> can = av->can_drop( *loc );
                if( !can.success() ) {
                    deny( can.str() );
                    break;
                }
                av->Character::drop( loc, av->pos_bub() );
                changed = true;
                break;
            }
            case 'E': { // eat / drink
                const ret_val<edible_rating> can = av->can_eat( *loc );
                if( !can.success() &&
                    ( can.value() == INEDIBLE || can.value() == INEDIBLE_MUTATION ) ) {
                    deny( can.str() );
                    break;
                }
                changed = av->consume( loc ) != trinary::NONE;
                break;
            }
            case 'f':   // favorite
                loc->is_favorite = !loc->is_favorite;
                changed = true;
                break;
            case 'c':   // change side (worn layer / gun mode)
                changed = av->change_side( loc );
                break;
            case 'U':   // unload
                changed = av->unload( loc );
                break;
            case 'D':   // disassemble: map-bound, unsafe for an unloaded avatar
                deny( _( "Disassembly is not available for netplay clients yet." ) );
                break;
            case 'm':   // mend
                av->mend_item( item_location( loc ), false );
                changed = true;
                break;
            case 'a':   // activate / use
                changed = av->invoke_item( loc.get_item(), av->pos_bub(), -1 );
                break;
            case 'r': { // reload
                item::reload_option opt = av->select_ammo( loc, false );
                if( !opt ) {
                    deny( _( "You have no compatible ammo for that." ) );
                    break;
                }
                av->assign_activity( reload_activity_actor( std::move( opt ) ) );
                changed = true;
                break;
            }
            case 'p': { // part reload (do not consider empty magazines)
                item::reload_option opt = av->select_ammo( loc, false, false );
                if( !opt ) {
                    deny( _( "You have no compatible ammo for that." ) );
                    break;
                }
                av->assign_activity( reload_activity_actor( std::move( opt ) ) );
                changed = true;
                break;
            }
            case '=':   // reassign inventory letter
                if( !targets.empty() ) {
                    const int ch = targets.front();
                    loc->invlet = ch == ' ' ? 0 : ch;
                    changed = true;
                }
                break;
            case '>':   // hide contents
                for( item_pocket *pocket : loc->get_standard_pockets() ) {
                    pocket->settings.set_collapse( true );
                }
                changed = true;
                break;
            case '<':   // show contents
                for( item_pocket *pocket : loc->get_standard_pockets() ) {
                    pocket->settings.set_collapse( false );
                }
                changed = true;
                break;
            case 'R': { // read
                std::vector<std::string> fail_messages;
                const Character *reader = av->get_book_reader( *loc, fail_messages );
                if( reader == nullptr ) {
                    for( const std::string &reason : fail_messages ) {
                        deny( reason );
                    }
                    if( fail_messages.empty() ) {
                        deny( _( "You can't read that." ) );
                    }
                    break;
                }
                item_location ereader;
                av->assign_activity( read_activity_actor(
                                         av->time_to_read( *loc, *reader ), loc, ereader, false ) );
                changed = true;
                break;
            }
            case 't': { // throw at a tile offset chosen on the client
                if( targets.size() < 2 ) {
                    deny( _( "Invalid target." ) );
                    break;
                }
                if( av->is_worn( *loc ) ) {
                    const ret_val<void> can = av->can_takeoff( *loc );
                    if( !can.success() ) {
                        deny( can.str() );
                        break;
                    }
                }
                if( !av->is_wielding( *loc ) && !av->wield( loc ) ) {
                    deny( _( "You can't wield that." ) );
                    break;
                }
                item_location weapon = av->get_wielded_item();
                if( !weapon ) {
                    deny( _( "You aren't holding something you can throw." ) );
                    break;
                }
                const tripoint_bub_ms from = av->pos_bub();
                const tripoint_bub_ms target( from.x() + targets[0],
                                              from.y() + targets[1], from.z() );
                if( !get_map().inbounds( target ) ) {
                    deny( _( "You can't throw there." ) );
                    break;
                }
                item thrown = *weapon;
                if( thrown.count_by_charges() && thrown.charges > 1 ) {
                    thrown.charges = 1;
                    weapon->mod_charges( -1 );
                } else {
                    av->remove_weapon();
                }
                av->throw_item( target, thrown );
                changed = true;
                break;
            }
            case 'i': { // insert source item(s) into this container
                if( loc->will_spill_if_unsealed() && !av->is_wielding( *loc ) ) {
                    deny( string_format(
                              _( "The %s would spill unless it's on the ground or wielded." ),
                              loc->type_name() ) );
                    break;
                }
                if( !target_locs.empty() ) {
                    for( item_location src : target_locs ) {
                        if( !src || src == loc ) {
                            continue;
                        }
                        const item copy = *src.get_item();
                        const ret_val<void> put = loc->put_in( copy, pocket_type::CONTAINER );
                        if( !put.success() ) {
                            deny( put.str() );
                            continue;
                        }
                        src.remove_item();
                        changed = true;
                    }
                    break;
                }
                for( const int target : targets ) {
                    if( target < 0 || target >= static_cast<int>( it->second.size() ) ) {
                        continue;
                    }
                    item_location src = it->second[target];
                    if( !src || src == loc ) {
                        continue;
                    }
                    const item copy = *src.get_item();
                    const ret_val<void> put = loc->put_in( copy, pocket_type::CONTAINER );
                    if( !put.success() ) {
                        deny( put.str() );
                        continue;
                    }
                    src.remove_item();
                    changed = true;
                }
                break;
            }
            default:
                // Actions not wired to the host yet: tell the client instead
                // of silently doing nothing.
                netplay::host_send_event_to_sid(
                    sid, _( "That action is not available to netplay clients yet." ),
                    static_cast<int>( m_info ) );
                break;
        }
        if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
            std::fprintf( f, "sid=%d node=%d key=%c item=%s result=%s\n",
                          sid, node, static_cast<char>( action ), item_id.c_str(),
                          changed ? "ok" : "no-change" );
            std::fclose( f );
        }
        if( changed ) {
            // New packet also rebuilds av_inv_nodes for the next action.
            av_send_status( sid );
        }
    }

    // DDA MP v4.0a-31: pocket settings edited on the client.
    for( int budget = 0; budget < 4; budget++ ) {
        int sid = 0;
        int node = 0;
        int generation = 0;
        std::string json;
        if( !netplay::host_pop_inventory_settings( sid, node, generation, json ) ) {
            break;
        }
        if( generation != av_inv_gen[sid] ) {
            netplay::host_send_event_to_sid(
                sid, _( "The inventory changed. Reopen it and try again." ),
                static_cast<int>( m_info ) );
            continue;
        }
        const auto sid_it = av_sid_to_npc.find( sid );
        if( sid_it == av_sid_to_npc.end() ) {
            continue;
        }
        shared_ptr_fast<npc> npc_ptr = av_find_npc( sid_it->second );
        if( !npc_ptr || !av_npc_alive( npc_ptr ) ) {
            continue;
        }
        g->mp_anchor_map_on( *npc_ptr, true );
        mp_anchored = true;
        const auto nodes_it = av_inv_nodes.find( sid );
        if( nodes_it == av_inv_nodes.end() || node < 0 ||
            node >= static_cast<int>( nodes_it->second.size() ) ) {
            continue;
        }
        item_location loc = nodes_it->second[node];
        if( !loc ) {
            continue;
        }
        try {
            JsonValue jv = json_loader::from_string( json );
            JsonArray ja = jv.get_array();
            std::vector<item_pocket *> pockets = loc->get_pockets(
            []( const item_pocket & ) {
                return true;
            } );
            std::size_t idx = 0;
            while( ja.has_more() && idx < pockets.size() ) {
                JsonObject jo = ja.next_object();
                pockets[idx]->settings.deserialize( jo );
                ++idx;
            }
            av_send_status( sid );
        } catch( ... ) {
            // Malformed payload: ignore rather than break the session.
        }
    }

    // DDA MP v4.0a-43: host-computed aiming panel for the client's cursor.
    for( int budget = 0; budget < 8; budget++ ) {
        int sid = 0;
        int node = 0;
        int generation = 0;
        int dx = 0;
        int dy = 0;
        if( !netplay::host_pop_aim_request( sid, node, generation, dx, dy ) ) {
            break;
        }
        if( generation != av_inv_gen[sid] ) {
            continue;
        }
        const auto sid_it = av_sid_to_npc.find( sid );
        if( sid_it == av_sid_to_npc.end() ) {
            continue;
        }
        shared_ptr_fast<npc> npc_ptr = av_find_npc( sid_it->second );
        if( !npc_ptr || !av_npc_alive( npc_ptr ) ) {
            continue;
        }
        g->mp_anchor_map_on( *npc_ptr, true );
        mp_anchored = true;
        const auto nodes_it = av_inv_nodes.find( sid );
        if( nodes_it == av_inv_nodes.end() || node < 0 ||
            node >= static_cast<int>( nodes_it->second.size() ) ) {
            continue;
        }
        item_location loc = nodes_it->second[node];
        if( !loc ) {
            continue;
        }
        const tripoint_bub_ms target = npc_ptr->pos_bub() +
                                       tripoint_rel_ms( dx, dy, 0 );
        const target_handler::netplay_aim_panel panel = target_handler::netplay_throw_panel(
                    *npc_ptr, *loc.get_item(), target );
        if( panel.width > 0 && panel.height > 0 ) {
            sidebar_frame sb;
            sb.width = panel.width;
            sb.height = panel.height;
            sb.cells.resize( panel.cells.size() );
            for( size_t ci = 0; ci < panel.cells.size(); ci++ ) {
                sb.cells[ci].text = panel.cells[ci].text;
                sb.cells[ci].color = panel.cells[ci].color;
            }
            netplay::host_send_aim_panel_to_sid( sid, sb );
        }
    }
    // Return the bubble to the host player after this pump (see the movement
    // pump: the host must never render from a client's POV).
    if( mp_anchored ) {
        g->mp_anchor_map_on( get_avatar() );
    }
}

/* DDA MP v4.0a-97: stream the ground items of the given tiles for the client's
 * pickup selector. Shared by 'g' (one chosen tile) and 'G'/pickup-all. */
static void mp_send_pickup_listing( int sid, npc &av,
                                    const std::vector<tripoint_bub_ms> &tiles )
{
    map &here = get_map();
    const tripoint_bub_ms pos = av.pos_bub();
    netplay::inv_packet pkt;
    std::vector<av_pick_row> rows;
    for( const tripoint_bub_ms &p : tiles ) {
        if( !here.inbounds( p ) ) {
            continue;
        }
        for( item &it : here.i_at( p ) ) {
            netplay::inv_entry_view e;
            e.id = it.typeId().str();
            e.name = netplay::inv_strip_tags( it.display_name( it.count(), false ) );
            e.count = it.count();
            e.charges = it.charges;
            e.weight_g = static_cast<long>( units::to_gram( it.weight() ) );
            e.volume_ml = static_cast<long>( units::to_milliliter( it.volume() ) );
            e.side_mm = static_cast<int>( units::to_millimeter( it.length() ) );
            e.container = it.is_container();
            e.color_index = netplay::inv_color_index( it.color_in_inventory() );
            const item_category &cat = it.get_category_shallow();
            e.category = cat.name_header();
            e.category_id = cat.get_id().str();
            e.category_rank = cat.sort_rank();
            e.dx = p.x() - pos.x();
            e.dy = p.y() - pos.y();
            // The apply path runs the vanilla pickup, which prompts for
            // unusual cases; offer items that need no prompt.
            e.can_take = av.can_stash( it ) &&
                         !it.made_of_from_type( phase_id::LIQUID ) &&
                         !it.made_of_from_type( phase_id::GAS ) &&
                         !it.is_bucket_nonempty() &&
                         !it.is_frozen_liquid() &&
                         it.is_owned_by( av, true );
            pkt.entries.push_back( e );
            rows.push_back( { here.get_abs( p ), it.typeId().str() } );
        }
    }
    if( pkt.entries.empty() ) {
        netplay::host_send_event_to_sid(
            sid, _( "There is nothing to pick up here." ), static_cast<int>( m_info ) );
        return;
    }
    av_pick_nodes[sid] = std::move( rows );
    pkt.header.generation = ++av_pick_gen[sid];
    if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
        std::fprintf( f, "pickup list sid=%d pos=%d,%d rows=%d gen=%d\n",
                      sid, pos.x(), pos.y(),
                      static_cast<int>( pkt.entries.size() ), pkt.header.generation );
        std::fclose( f );
    }
    netplay::host_send_pickup_to_sid( sid, netplay::serialize_inv_packet( pkt ) );
}

/* DDA MP v4.0a-72: choices made in the client's own action menu. Each order
 * is applied to the session's avatar NPC on the main thread, so the client can
 * do what the host can. Actions that need map-bound or avatar-only UIs are
 * answered with a message until their own bridge lands. */
/* DDA MP v4.0a-119: relay the messages an engine action just produced to the
 * acting session. The host log keeps them too, so a client's action remains
 * visible on the host screen. */
static void mp_send_pickup_listing( int sid, npc &av,
                                    const std::vector<tripoint_bub_ms> &tiles );

static void mp_forward_new_messages( int sid, std::size_t before )
{
    const std::size_t after = Messages::size();
    if( after <= before ) {
        return;
    }
    const std::vector<std::pair<std::string, int>> recent =
        Messages::recent_messages_typed( after - before );
    for( const auto &m : recent ) {
        netplay::host_send_event_to_sid( sid, m.first, m.second );
    }
}

void host_pump_client_actions()
{
    if( !host_is_running() ) {
        return;
    }
    // DDA MP v4.0a-80: return the reality bubble to the host player after this
    // pump moved it onto a session avatar (a pending movement burst keeps its
    // own anchor because this flag stays clear).
    bool mp_anchored = false;
    for( int budget = 0; budget < 8; budget++ ) {
        int sid = 0;
        int action = 0;
        if( !netplay::host_pop_client_action( sid, action ) ) {
            break;
        }
        const auto sid_it = av_sid_to_npc.find( sid );
        if( sid_it == av_sid_to_npc.end() ) {
            continue;
        }
        shared_ptr_fast<npc> npc_ptr = av_find_npc( sid_it->second );
        if( !npc_ptr || !av_npc_alive( npc_ptr ) ) {
            continue;
        }
        npc *av = npc_ptr.get();
        g->mp_anchor_map_on( *av, true );
        mp_anchored = true;
        bool changed = false;
        switch( static_cast<action_id>( action ) ) {
            case ACTION_WAIT:
                // Same contract as the WAIT key: spend the pool, redraw.
                av_acted_since_tick[sid] = true;
                av_move_pool[sid] = 0;
                netplay::host_send_hover_to_sid( sid, "" );
                av_send_session_picture( sid, av_session_center( sid ), npc_ptr.get() );
                continue;
            case ACTION_TOGGLE_RUN: {
                const move_mode_id walk( "walk" );
                const move_mode_id run( "run" );
                const move_mode_id next = av->is_running() ? walk : run;
                const int cost = av->move_mode_switch_cost( av->current_movement_mode(), next );
                av->set_movement_mode( next );
                auto pool_it = av_move_pool.find( sid );
                if( pool_it != av_move_pool.end() ) {
                    pool_it->second = std::max( 0, pool_it->second - cost );
                    av->set_moves( pool_it->second );
                }
                changed = true;
                break;
            }
            case ACTION_TOGGLE_CROUCH: {
                const move_mode_id walk( "walk" );
                const move_mode_id crouch( "crouch" );
                const move_mode_id next = av->is_crouching() ? walk : crouch;
                const int cost = av->move_mode_switch_cost( av->current_movement_mode(), next );
                av->set_movement_mode( next );
                auto pool_it = av_move_pool.find( sid );
                if( pool_it != av_move_pool.end() ) {
                    pool_it->second = std::max( 0, pool_it->second - cost );
                    av->set_moves( pool_it->second );
                }
                changed = true;
                break;
            }
            case ACTION_TOGGLE_PRONE: {
                const move_mode_id walk( "walk" );
                const move_mode_id prone( "prone" );
                const move_mode_id next = av->is_prone() ? walk : prone;
                const int cost = av->move_mode_switch_cost( av->current_movement_mode(), next );
                av->set_movement_mode( next );
                auto pool_it = av_move_pool.find( sid );
                if( pool_it != av_move_pool.end() ) {
                    pool_it->second = std::max( 0, pool_it->second - cost );
                    av->set_moves( pool_it->second );
                }
                changed = true;
                break;
            }
            case ACTION_RESET_MOVE: {
                const move_mode_id walk( "walk" );
                av->set_movement_mode( walk );
                changed = true;
                break;
            }
            case ACTION_MOVE_UP:
            case ACTION_MOVE_DOWN: {
                // DDA MP v4.0a-124: stairs for a session avatar. The map's
                // z slices around the bubble are already loaded, and the
                // client rebuilds its native bubble around the streamed
                // origin, so changing the avatar's absolute z is enough.
                map &here = get_map();
                const int dz = action == ACTION_MOVE_UP ? 1 : -1;
                const tripoint_bub_ms pos = av->pos_bub();
                const bool has_stairs = dz > 0
                                        ? here.has_flag( ter_furn_flag::TFLAG_GOES_UP, pos )
                                        : here.has_flag( ter_furn_flag::TFLAG_GOES_DOWN, pos );
                if( !has_stairs ) {
                    netplay::host_send_event_to_sid(
                        sid, dz > 0 ? _( "You can't go up here!" )
                        : _( "You can't go down here!" ),
                        static_cast<int>( m_info ) );
                    break;
                }
                const tripoint_bub_ms dest = pos + tripoint_rel_ms( 0, 0, dz );
                if( !here.inbounds( dest ) || !here.passable( dest ) ) {
                    netplay::host_send_event_to_sid(
                        sid, _( "The way is blocked." ), static_cast<int>( m_info ) );
                    break;
                }
                tripoint_abs_ms new_abs = av->pos_abs();
                new_abs.z() += dz;
                av->setpos( new_abs, false );
                const int stair_cost = to_moves<int>( 1_seconds );
                auto pool_it = av_move_pool.find( sid );
                if( pool_it == av_move_pool.end() ) {
                    pool_it = av_move_pool.insert( { sid, av->get_speed() } ).first;
                }
                pool_it->second = std::max( 0, pool_it->second - stair_cost );
                av->set_moves( pool_it->second );
                av_acted_since_tick[sid] = true;
                netplay::host_send_event_to_sid(
                    sid, dz > 0 ? _( "You go up the stairs." ) : _( "You go down the stairs." ),
                    static_cast<int>( m_info ) );
                if( pool_it->second <= 0 && !netplay::long_wait_active() ) {
                    av_waiting_for_turn[sid] = true;
                    netplay::host_send_hover_to_sid( sid, _( "Waiting for other players…" ) );
                } else {
                    netplay::host_send_hover_to_sid( sid, "" );
                }
                av_send_session_picture( sid, av->pos_bub(), npc_ptr.get() );
                changed = true;
                break;
            }
            case ACTION_CONTROL_VEHICLE: {
                // DDA MP v4.0a-132: board, drive and leave a vehicle with the
                // session avatar. The engine's board/unboard and vehicle
                // pldrive all take a Character, so the same calls the host
                // player uses work for the npc avatar.
                map &here = get_map();
                const tripoint_bub_ms pos = av->pos_bub();
                const optional_vpart_position vp = here.veh_at( pos );
                if( !vp ) {
                    netplay::host_send_event_to_sid(
                        sid, _( "You can't drive here." ), static_cast<int>( m_info ) );
                    break;
                }
                if( av->controlling_vehicle ) {
                    av->controlling_vehicle = false;
                    netplay::host_send_event_to_sid(
                        sid, _( "You stop controlling the vehicle." ),
                        static_cast<int>( m_info ) );
                    changed = true;
                    break;
                }
                if( av->in_vehicle ) {
                    here.unboard_vehicle( pos, false );
                    static const point leave_offsets[] = {
                        { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 },
                        { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 }
                    };
                    for( const point &off : leave_offsets ) {
                        const tripoint_bub_ms p( pos + tripoint_rel_ms( off.x, off.y, 0 ) );
                        if( here.inbounds( p ) && !here.veh_at( p ) && here.passable( p ) ) {
                            av->setpos( here, p, false );
                            break;
                        }
                    }
                    netplay::host_send_event_to_sid(
                        sid, _( "You get out of the vehicle." ), static_cast<int>( m_info ) );
                    changed = true;
                    break;
                }
                const int controls = vp->vehicle().avail_part_with_feature(
                                         vp->mount_pos(), "CONTROLS" );
                if( controls < 0 ) {
                    netplay::host_send_event_to_sid(
                        sid, _( "This vehicle has no working controls." ),
                        static_cast<int>( m_info ) );
                    break;
                }
                here.board_vehicle( pos, av );
                av->controlling_vehicle = true;
                netplay::host_send_event_to_sid(
                    sid, string_format( _( "You take control of the %s." ),
                                        vp->vehicle().name ),
                    static_cast<int>( m_info ) );
                changed = true;
                break;
            }

            case ACTION_SLEEP: {
                // DDA MP v4.0a-128: the NPC avatar can run the engine's sleep
                // activity; the host keeps turning and the client is skipped
                // by the round gate while it sleeps.
                av->get_comfort_at( av->pos_bub() ).add_try_msgs( *av );
                av->assign_activity( try_sleep_activity_actor( 24_hours ) );
                av_acted_since_tick[sid] = true;
                netplay::host_send_hover_to_sid( sid, "" );
                av_send_session_picture( sid, av->pos_bub(), npc_ptr.get() );
                changed = true;
                break;
            }

            case ACTION_SELECT_FIRE_MODE: {
                item_location gun = av->get_wielded_item();
                if( gun && gun->is_gun() && gun->gun_all_modes().size() > 1 ) {
                    gun->gun_cycle_mode();
                    netplay::host_send_event_to_sid(
                        sid, string_format( _( "Firing mode: %s" ),
                                            gun->gun_current_mode()->tname() ),
                        static_cast<int>( m_info ) );
                    changed = true;
                } else {
                    netplay::host_send_event_to_sid(
                        sid, _( "You are not wielding a multi-mode gun." ),
                        static_cast<int>( m_info ) );
                }
                break;
            }

            case ACTION_SMASH: {
                // DDA MP v4.0a-124: smash the nearest adjacent bashable tile
                // with the avatar's own smash_ability(); the engine resolves
                // damage, sounds, drops and the "you smash" messages, which
                // are relayed to the acting session.
                map &here = get_map();
                const tripoint_bub_ms pos = av->pos_bub();
                static const point smash_offsets[] = {
                    { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 },
                    { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 }
                };
                std::optional<tripoint_bub_ms> target;
                for( const point &off : smash_offsets ) {
                    const tripoint_bub_ms p( pos + tripoint_rel_ms( off.x, off.y, 0 ) );
                    if( here.inbounds( p ) && here.is_bashable( p ) ) {
                        target = p;
                        break;
                    }
                }
                if( !target ) {
                    netplay::host_send_event_to_sid(
                        sid, _( "There's nothing to smash here." ), static_cast<int>( m_info ) );
                    break;
                }
                const std::size_t msg_before = Messages::size();
                const int move_cost = !av->is_armed() ? 80 :
                                      static_cast<int>( av->get_wielded_item()->attack_time( *av ) * 0.8 );
                const bash_params br = here.bash( *target, av->smash_ability(), false, false, false );
                av->mod_moves( -move_cost );
                auto pool_it = av_move_pool.find( sid );
                if( pool_it == av_move_pool.end() ) {
                    pool_it = av_move_pool.insert( { sid, av->get_speed() } ).first;
                }
                pool_it->second = std::max( 0, av->get_moves() );
                av_acted_since_tick[sid] = true;
                if( br.did_bash && br.success ) {
                    netplay::host_send_event_to_sid(
                        sid, string_format( _( "You smash the %s!" ), here.name( *target ) ),
                        static_cast<int>( m_good ) );
                } else if( br.did_bash ) {
                    netplay::host_send_event_to_sid(
                        sid, string_format( _( "You hit the %s, but it holds." ),
                                            here.name( *target ) ),
                        static_cast<int>( m_info ) );
                }
                if( pool_it->second <= 0 && !netplay::long_wait_active() ) {
                    av_waiting_for_turn[sid] = true;
                    netplay::host_send_hover_to_sid( sid, _( "Waiting for other players…" ) );
                } else {
                    netplay::host_send_hover_to_sid( sid, "" );
                }
                mp_forward_new_messages( sid, msg_before );
                av_send_session_picture( sid, av->pos_bub(), npc_ptr.get() );
                changed = true;
                break;
            }

            case ACTION_PICKUP:
            case ACTION_PICKUP_ALL:
            case ACTION_LOOT: {
                // The host's own 'g' asks which adjacent tile to pick from
                // when several have items (choose_adjacent_highlight); only the
                // pickup-all key takes everything around. Mirror that: one tile
                // goes straight to the listing, several make the client choose.
                map &here = get_map();
                const tripoint_bub_ms pos = av->pos_bub();
                std::vector<tripoint_bub_ms> tiles;
                for( const tripoint_bub_ms &p : here.points_in_radius( pos, 1 ) ) {
                    if( here.inbounds( p ) && !here.i_at( p ).empty() ) {
                        tiles.push_back( p );
                    }
                }
                if( tiles.empty() ) {
                    netplay::host_send_event_to_sid(
                        sid, _( "There is nothing to pick up here." ),
                        static_cast<int>( m_info ) );
                    break;
                }
                if( action == ACTION_PICKUP && tiles.size() > 1 ) {
                    const int gen = ++av_pick_tile_gen[sid];
                    if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
                        std::fprintf( f, "pickup tiles sid=%d n=%d gen=%d pos=%d,%d\n",
                                      sid, static_cast<int>( tiles.size() ), gen,
                                      pos.x(), pos.y() );
                        std::fclose( f );
                    }
                    std::string text = std::to_string( gen );
                    for( const tripoint_bub_ms &p : tiles ) {
                        text += "\n" + std::to_string( p.x() - pos.x() ) + " " +
                                std::to_string( p.y() - pos.y() );
                    }
                    netplay::host_send_pickup_tiles_to_sid( sid, text );
                    break;
                }
                mp_send_pickup_listing( sid, *av, tiles );
                break;
            }

            case ACTION_EXAMINE:
            case ACTION_EXAMINE_AND_PICKUP: {
                // DDA MP v4.0a-119: the client cannot run game::examine (it
                // acts on the host player), so examine the nearest adjacent
                // tile the engine itself marks examinable, with the avatar as
                // the examining character. The engine's messages are relayed.
                map &here = get_map();
                const tripoint_bub_ms pos = av->pos_bub();
                static const point examine_offsets[] = {
                    { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 },
                    { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 }
                };
                std::optional<tripoint_bub_ms> target;
                for( const point &off : examine_offsets ) {
                    const tripoint_bub_ms p( pos + tripoint_rel_ms( off.x, off.y, 0 ) );
                    if( !here.inbounds( p ) ) {
                        continue;
                    }
                    if( here.has_furn( p ) && here.furn( p ).obj().can_examine( p ) ) {
                        target = p;
                        break;
                    }
                    if( here.ter( p ).obj().can_examine( p ) ) {
                        target = p;
                        break;
                    }
                    if( !here.tr_at( p ).is_null() ) {
                        target = p;
                        break;
                    }
                }
                if( !target ) {
                    netplay::host_send_event_to_sid(
                        sid, _( "There is nothing that can be examined nearby." ),
                        static_cast<int>( m_info ) );
                    break;
                }
                const std::size_t msg_before = Messages::size();
                const tripoint_bub_ms p = *target;
                if( here.has_furn( p ) && here.furn( p ).obj().can_examine( p ) ) {
                    here.furn( p ).obj().examine( *av, p );
                } else if( here.ter( p ).obj().can_examine( p ) ) {
                    here.ter( p ).obj().examine( *av, p );
                }
                if( !here.tr_at( p ).is_null() ) {
                    here.tr_at( p ).examine( p );
                }
                if( action == ACTION_EXAMINE_AND_PICKUP && !here.i_at( p ).empty() ) {
                    std::vector<tripoint_bub_ms> one{ p };
                    mp_send_pickup_listing( sid, *av, one );
                }
                mp_forward_new_messages( sid, msg_before );
                changed = true;
                break;
            }

            case ACTION_OPEN:
            case ACTION_CLOSE: {
                // Same engine calls the local player's ACTION_OPEN/CLOSE end
                // up in: map::open_door / map::close_door, but for the client's
                // avatar instead of the host player. The target is the nearest
                // adjacent tile the engine itself accepts (cardinals first).
                map &here = get_map();
                const bool opening = action == ACTION_OPEN;
                const tripoint_bub_ms pos = av->pos_bub();
                const bool inside = !here.is_outside( pos );
                static const point offsets[] = {
                    { 0, -1 }, { 1, 0 }, { 0, 1 }, { -1, 0 },
                    { 1, -1 }, { 1, 1 }, { -1, 1 }, { -1, -1 }
                };
                std::optional<tripoint_bub_ms> target;
                for( const point &off : offsets ) {
                    const tripoint_bub_ms p( pos + tripoint_rel_ms( off.x, off.y, 0 ) );
                    if( !here.inbounds( p ) ) {
                        continue;
                    }
                    if( opening ? here.open_door( *av, p, inside, /*check_only=*/true )
                        : here.close_door( p, inside, /*check_only=*/true ) ) {
                        target = p;
                        break;
                    }
                }
                if( !target ) {
                    netplay::host_send_event_to_sid(
                        sid, opening ? _( "There is nothing that can be opened nearby." )
                        : _( "There is nothing that can be closed nearby." ),
                        static_cast<int>( m_info ) );
                    break;
                }
                if( opening ) {
                    here.open_door( *av, *target, inside, /*check_only=*/false );
                } else {
                    here.close_door( *target, inside, /*check_only=*/false );
                }
                // Same turn cost as the local action's activity.
                av->set_moves( std::max( 0, av->get_moves() - to_moves<int>( 1_seconds ) ) );
                auto cpool_it = av_move_pool.find( sid );
                if( cpool_it != av_move_pool.end() ) {
                    cpool_it->second = std::max( 0, cpool_it->second -
                                                 to_moves<int>( 1_seconds ) );
                }
                changed = true;
                break;
            }
            default:
                netplay::host_send_event_to_sid(
                    sid, _( "That action is not available to netplay clients yet." ),
                    static_cast<int>( m_info ) );
                break;
        }
        if( changed ) {
            if( action == ACTION_OPEN || action == ACTION_CLOSE ) {
                // These spend a turn on the host side; count the session as
                // acted so the host's parity gate is not blocked by a door.
                av_acted_since_tick[sid] = true;
            }
            av_send_status( sid );
        }
    }
    // DDA MP v4.0a-97: 'g' with items on several tiles - the client picked one.
    for( int budget = 0; budget < 4; budget++ ) {
        int psid = 0;
        int generation = 0;
        int dx = 0;
        int dy = 0;
        if( !netplay::host_pop_pickup_tile( psid, generation, dx, dy ) ) {
            break;
        }
        const auto gen_it = av_pick_tile_gen.find( psid );
        if( gen_it == av_pick_tile_gen.end() || generation != gen_it->second ) {
            netplay::host_send_event_to_sid(
                psid, _( "The tiles changed. Press pickup again." ),
                static_cast<int>( m_info ) );
            continue;
        }
        const auto tsid_it = av_sid_to_npc.find( psid );
        if( tsid_it == av_sid_to_npc.end() ) {
            continue;
        }
        shared_ptr_fast<npc> tnpc = av_find_npc( tsid_it->second );
        if( !tnpc || !av_npc_alive( tnpc ) ) {
            continue;
        }
        g->mp_anchor_map_on( *tnpc, true );
        mp_anchored = true;
        const tripoint_bub_ms tp = tnpc->pos_bub() + tripoint_rel_ms( dx, dy, 0 );
        std::vector<tripoint_bub_ms> one{ tp };
        mp_send_pickup_listing( psid, *tnpc, one );
    }
    // DDA MP v4.0a-90: pickup rows chosen in the client's own selector.
    for( int budget = 0; budget < 4; budget++ ) {
        int psid = 0;
        int generation = 0;
        int equip = 0;
        std::vector<int> nodes;
        if( !netplay::host_pop_pickup_action( psid, generation, nodes, equip ) ) {
            break;
        }
        const auto nodes_it = av_pick_nodes.find( psid );
        if( nodes_it == av_pick_nodes.end() || generation != av_pick_gen[psid] ) {
            netplay::host_send_event_to_sid(
                psid, _( "The item list changed. Reopen it and try again." ),
                static_cast<int>( m_info ) );
            continue;
        }
        const auto sid_it = av_sid_to_npc.find( psid );
        if( sid_it == av_sid_to_npc.end() ) {
            continue;
        }
        shared_ptr_fast<npc> npc_ptr = av_find_npc( sid_it->second );
        if( !npc_ptr || !av_npc_alive( npc_ptr ) ) {
            continue;
        }
        g->mp_anchor_map_on( *npc_ptr, true );
        mp_anchored = true;
        map &here = get_map();
        drop_locations what;
        for( const int node : nodes ) {
            if( node < 0 || node >= static_cast<int>( nodes_it->second.size() ) ) {
                continue;
            }
            const av_pick_row &row = nodes_it->second[node];
            const tripoint_bub_ms p = here.get_bub( row.pos );
            if( !here.inbounds( p ) ) {
                continue;
            }
            // The map may have changed since the listing; re-find the row by type.
            for( item &it : here.i_at( p ) ) {
                if( it.typeId().str() != row.type ) {
                    continue;
                }
                if( craft_reservation::contains_reserved_or_live_craft( it ) ||
                    !npc_ptr->can_stash( it ) ) {
                    break;
                }
                what.emplace_back( item_location( map_cursor( p ), &it ), it.count() );
                break;
            }
        }
        if( what.empty() ) {
            netplay::host_send_event_to_sid(
                psid, _( "You can't carry anything more." ), static_cast<int>( m_info ) );
            continue;
        }
        if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
            std::fprintf( f, "pickup take sid=%d gen=%d rows=%d what=%d\n",
                          psid, generation, static_cast<int>( nodes.size() ),
                          static_cast<int>( what.size() ) );
            std::fclose( f );
        }
        // The vanilla pickup activity runs for npcs as of v4.0a-90; finish it
        // while the bubble is anchored so no item_location outlives the call.
        static const activity_id mp_act_pickup( "ACT_PICKUP" );
        npc_ptr->pick_up( what );
        npc_ptr->set_moves( std::max( npc_ptr->get_moves(), 1000 ) );
        for( int guard = 0;
             guard < 100 && npc_ptr->activity && npc_ptr->activity.id() == mp_act_pickup;
             guard++ ) {
            npc_ptr->process_activity();
        }
        if( npc_ptr->activity && npc_ptr->activity.id() == mp_act_pickup ) {
            npc_ptr->cancel_activity();
        }
        // v4.0a-101: the client asked for the item in hand / worn right away,
        // like the host's own pickup row (pickup.cpp handle_problematic_pickup).
        if( equip != 0 && !nodes.empty() ) {
            const av_pick_row &frow = nodes_it->second[nodes.front()];
            item *picked = nullptr;
            for( item *cand : npc_ptr->inv_dump() ) {
                if( cand != nullptr && cand->typeId().str() == frow.type ) {
                    picked = cand;
                    break;
                }
            }
            bool equipped = false;
            if( picked != nullptr ) {
                if( equip == 1 ) {
                    equipped = npc_ptr->wield( *picked );
                } else if( equip == 2 ) {
                    equipped = npc_ptr->wear_item( *picked, false ).has_value();
                }
            }
            if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
                std::fprintf( f, "pickup equip sid=%d equip=%d item=%s ok=%d\n",
                              psid, equip, frow.type.c_str(), equipped ? 1 : 0 );
                std::fclose( f );
            }
        }
        // Picking items up takes a turn; count the session as acted.
        av_acted_since_tick[psid] = true;
        av_send_status( psid );
    }
    if( mp_anchored ) {
        g->mp_anchor_map_on( get_avatar() );
    }
}

/* DDA MP v4.1: known intent ids are demultiplexed into the typed queues by the
 * reader thread. Anything left here has no handler yet; report it once instead
 * of dropping the player's order silently. */
void host_pump_intents()
{
    netplay::intent_request in;
    for( int budget = 0; budget < 8; budget++ ) {
        if( !netplay::host_pop_intent( in ) ) {
            break;
        }
        // DDA MP v4.0a-131: crafting recipe chosen in the client's own UI.
        if( ( in.action == "craft" || in.action == "craft.all" ) && !in.data.empty() ) {
            npc *av = av_session_npc( in.sid );
            const recipe_id rid( in.data );
            if( av == nullptr ) {
                continue;
            }
            if( !rid.is_valid() ) {
                netplay::host_send_event_to_sid(
                    in.sid, _( "Unknown recipe." ), static_cast<int>( m_info ) );
            } else {
                const int batch = in.args.empty() ? 1 : std::max( 1, in.args[0] );
                g->mp_anchor_map_on( *av, true );
                const std::size_t msg_before = Messages::size();
                if( in.action == "craft.all" ) {
                    av->make_all_craft( rid, batch, std::nullopt );
                } else {
                    av->make_craft( rid, batch );
                }
                av_acted_since_tick[in.sid] = true;
                av_send_status( in.sid );
                av_send_session_picture( in.sid, av->pos_bub(), av );
                mp_forward_new_messages( in.sid, msg_before );
                g->mp_anchor_map_on( get_avatar() );
            }
            continue;
        }
        // DDA MP v4.0a-131: repeat the last craft (RECRAFT).
        if( in.action == "recraft" ) {
            npc *av = av_session_npc( in.sid );
            if( av != nullptr ) {
                if( av->lastrecipe.is_null() ) {
                    netplay::host_send_event_to_sid(
                        in.sid, _( "Craft something first." ), static_cast<int>( m_info ) );
                } else {
                    const int batch = std::max( 1, av->last_batch );
                    g->mp_anchor_map_on( *av, true );
                    const std::size_t msg_before = Messages::size();
                    av->make_craft( av->lastrecipe, batch );
                    av_acted_since_tick[in.sid] = true;
                    av_send_status( in.sid );
                    av_send_session_picture( in.sid, av->pos_bub(), av );
                    mp_forward_new_messages( in.sid, msg_before );
                    if( !netplay::host_avatar_keys_pending() ) {
                        g->mp_anchor_map_on( get_avatar() );
                    }
                }
            }
            continue;
        }
        // DDA MP v4.0a-121: ranged fire. The client picked the target with the
        // stock cursor; the host resolves the shot on the session avatar.
        if( in.action == "fire" && in.args.size() >= 3 ) {
            shared_ptr_fast<npc> npc_ptr;
            const auto sid_it = av_sid_to_npc.find( in.sid );
            if( sid_it != av_sid_to_npc.end() ) {
                npc_ptr = av_find_npc( sid_it->second );
            }
            if( npc_ptr && av_npc_alive( npc_ptr ) ) {
                npc *av = npc_ptr.get();
                g->mp_anchor_map_on( *av, true );
                const tripoint_bub_ms from = av->pos_bub();
                const tripoint_bub_ms tgt = from + tripoint_rel_ms( in.args[0], in.args[1], 0 );
                const int shots = std::max( 1, in.args[2] );
                const item_location gun = av->get_wielded_item();
                if( gun && gun->is_gun() ) {
                    const std::size_t msg_before = Messages::size();
                    av->fire_gun( tgt, shots );
                    av_move_pool[in.sid] = std::max( 0, av->get_moves() );
                    av->set_moves( av_move_pool[in.sid] );
                    av_acted_since_tick[in.sid] = true;
                    if( av_move_pool[in.sid] <= 0 && !netplay::long_wait_active() ) {
                        av_waiting_for_turn[in.sid] = true;
                        netplay::host_send_hover_to_sid( in.sid,
                                                         _( "Waiting for other players…" ) );
                    } else {
                        netplay::host_send_hover_to_sid( in.sid, "" );
                    }
                    av_send_session_picture( in.sid, av->pos_bub(), av );
                    mp_forward_new_messages( in.sid, msg_before );
                } else {
                    netplay::host_send_event_to_sid(
                        in.sid, _( "You are not wielding a gun." ),
                        static_cast<int>( m_info ) );
                }
                g->mp_anchor_map_on( get_avatar() );
            }
            continue;
        }
        // DDA MP v4.0a-137: player chat, broadcast to every session.
        if( in.action == "chat" && !in.data.empty() ) {
            std::string nick = host_session_name( in.sid );
            if( nick.empty() ) {
                nick = "player";
            }
            const std::string line = string_format( "%s: %s", nick, in.data );
            host_broadcast_event( line, static_cast<int>( m_info ) );
            add_msg( "%s", line );
            continue;
        }
        if( std::FILE *f = netplay::mp_diag_open( "netplay_actions.log" ) ) {
            std::fprintf( f, "intent unhandled sid=%d action=%s args=%d\n",
                          in.sid, in.action.c_str(), static_cast<int>( in.args.size() ) );
            std::fclose( f );
        }
        netplay::host_send_event_to_sid(
            in.sid, string_format( _( "The action \"%s\" is not available to netplay clients yet." ),
                                   in.action ),
            static_cast<int>( m_info ) );
    }
}

/* DDA MP v4.0a-7: called at the end of every host world turn. A fresh round
 * starts: every session may act again, so clear the per-session "acted" marks
 * and lift their waiting hints. */
void host_note_world_turn()
{
    if( !host_is_running() || host_session_count() == 0 ) {
        return;
    }
    av_acted_since_tick.clear();
    // Move points of an avatar that is outside the bubble (the usual case
    // between its own steps) are not refilled by the vanilla NPC turn pass,
    // so pay its one turn of speed here — exactly like Character::process_turn
    // does for a processed NPC, and only when it wasn't processed already.
    for( int sid : netplay::host_session_ids() ) {
        shared_ptr_fast<npc> npc_ptr;
        {
            const auto sid_it = av_sid_to_npc.find( sid );
            if( sid_it != av_sid_to_npc.end() ) {
                npc_ptr = av_find_npc( sid_it->second );
            }
        }
        if( npc_ptr && av_npc_alive( npc_ptr ) && !npc_ptr->is_dead_state() ) {
            npc *av = npc_ptr.get();
            // Per-turn state (body temperature, effects, activity, needs) is
            // normally advanced by the NPC turn pass; an out-of-bubble session
            // avatar never gets it, so do the base Character pass here with the
            // bubble anchored on the avatar.
            g->mp_anchor_map_on( *av, true );
            av->Character::process_turn();
            // Exactly one turn of move points, never an accumulated stock.
            av_move_pool[sid] = av->get_speed();
            av->set_moves( av_move_pool[sid] );
            // DDA MP v4.0a-22: activities that only touch the avatar's own
            // inventory (reading, reloading, mending) are not ticked by the
            // world, so advance them here. Map-bound activities such as
            // disassembly are deliberately not ticked: the avatar may be
            // outside the host bubble and would act on the wrong map.
            if( av->activity ) {
                static const activity_id mp_act_read( "ACT_READ" );
                static const activity_id mp_act_reload( "ACT_RELOAD" );
                static const activity_id mp_act_mend( "ACT_MEND_ITEM" );
                static const activity_id mp_act_pickup( "ACT_PICKUP" );
                const activity_id &act_id = av->activity.id();
                if( act_id == mp_act_read || act_id == mp_act_reload ||
                    act_id == mp_act_mend || act_id == mp_act_pickup ) {
                    av->process_activity();
                }
                static const activity_id mp_act_try_sleep( "ACT_TRY_SLEEP" );
                if( act_id == mp_act_try_sleep ) {
                    av->process_activity();
                }
                static const activity_id mp_act_craft( "ACT_CRAFT" );
                static const activity_id mp_act_craft_wait( "ACT_CRAFT_WAIT" );
                if( act_id == mp_act_craft || act_id == mp_act_craft_wait ) {
                    av->process_activity();
                }
            }
            // DDA MP v4.0a-8/9: refresh the client's status payloads.
            av_send_status( sid );
        } else if( npc_ptr && npc_ptr->is_dead_state() ) {
            // DDA MP v4.0a-130: tell the session once; its avatar cannot act
            // any more and the round gate already skips it.
            static std::map<int, bool> mp_death_notified;
            if( !mp_death_notified[sid] ) {
                mp_death_notified[sid] = true;
                netplay::host_send_event_to_sid(
                    sid,
                    _( "You have died. Disconnect and join again to create a new character." ),
                    netplay::EVENT_DEATH );
            }
        }
        av_waiting_for_turn[sid] = false;
        netplay::host_send_hover_to_sid( sid, "" );
    }
    // DDA MP v4.0a-80: the loop above anchored the reality bubble on every
    // session avatar in turn. Hand it back to the host player before the host
    // draws its own screen, or the host view would stay centred on the last
    // client.
    g->mp_anchor_map_on( get_avatar() );
}

/* DDA MP v4.0a-7: strict turn parity — the host may spend its turn only after
 * every bound client gave its order. No grace: waiting is exactly the point
 * (the host sees "waiting for other players..." while the world stands). */
bool host_turn_blocked()
{
    if( !host_is_running() || host_session_count() == 0 ) {
        return false;
    }
    return !av_all_acted();
}

} // namespace netplay
