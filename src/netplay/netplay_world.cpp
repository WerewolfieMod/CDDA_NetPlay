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
#include "effect.h"
#include "game.h"
#include "item.h"
#include "itype.h"
#include "json.h"
#include "json_loader.h"
#include "lightmap.h"
#include "map.h"
#include "map_memory.h"
#include "memory_fast.h"
#include "monster.h"
#include "mtype.h"
#include "npc.h"
#include "output.h"
#include "messages.h"
#include "cata_utility.h"
#include "string_formatter.h"
#include "smooth_lighting.h"
#include "submap.h"
#include "translations.h"
#include "trap.h"
#include "weather.h"
#include "overmapbuffer.h"
#include "worldfactory.h"

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
/* DDA MP: local trait handle for the INATTENTIVE attitude badge check. */
static const trait_id trait_INATTENTIVE( "INATTENTIVE" );

/* DDA MP v3.9: diagnostics master switch. When false, no netplay_tiles.log /
 * netplay_light_trace.log lines are produced by this TU (mine traces,
 * snapshot size logs). Flip to true and rebuild to re-enable. */
constexpr bool mp_netplay_diag = false;

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
                    const trap &tr = here.tr_at( p );
                    if( !tr.is_null() && tr.can_see( p, self ) ) {
                        t.trap = tr.loadid.id().str();
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
        here.update_visibility_cache( center.z(), viewer );
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
        mine_tiles( out, u, viewer, here, center );
    }
#endif

    // Restore the tiles renderer before the window is destructed (RAII).
#if defined(TILES)
    use_tiles = prev_tiles;
#endif

    out.status_line = status_line_for( u );
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

static tripoint_bub_ms av_session_center( int sid )
{
    if( npc *n = av_session_npc( sid ) ) {
        return n->pos_bub();
    }
    return get_avatar().pos_bub();
}

static void av_send_session_picture( int sid, const tripoint_bub_ms &center,
                                     npc *viewer = nullptr )
{
    if( !get_map().inbounds( center ) ) {
        return;
    }
    snapshot snap;
    build_host_snapshot( snap, center, viewer );
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
        av_send_session_picture( sid, av_session_center( sid ), viewer );
    }
}

/* DDA MP v3.9: the host streams snapshots only on its own turn, so an idle
 * host would leave a freshly-loaded client staring at its menu until somebody
 * pressed a key. The client polls with SNAP_REQ instead; the reader threads
 * must not touch game state, so this consumes the flag on the host main
 * thread and answers with the current picture. No-op without a request. */
void host_pump_snapshot_requests()
{
    if( !host_pop_snapshot_request() ) {
        return;
    }
    host_game_tick();
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

static bool av_all_acted()
{
    for( const auto &entry : av_sid_to_npc ) {
        const auto it = av_acted_since_tick.find( entry.first );
        if( it == av_acted_since_tick.end() || !it->second ) {
            return false;
        }
    }
    return true;
}

void host_pump_avatar_keys()
{
    netplay::sid_key sk;
    while( netplay::host_pop_sid_key( sk ) ) {
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
        // DDA MP v4.0a-2: the avatar roams the world like the host player
        // does, so keep the reality bubble anchored around it (the same way
        // the engine anchors it around the player after a move). This also
        // re-activates an avatar the bubble had scrolled away from. Light
        // mode: only the cheap shifts run, no cache/spawn work — a burst of
        // client steps must not pay the full anchor price on every step.
        g->mp_anchor_map_on( *npc_ptr, true );
        const tripoint_bub_ms dst = npc_ptr->pos_bub() + dir_it->second;
        // DDA MP v4.0a-2: a step pays the price the engine charges an NPC's
        // own walk move, out of the avatar's move pool (refilled every world
        // turn). Speed, terrain and obstacles all cost real move points, so
        // the player's keyrate is naturally limited to what the body can do
        // per turn instead of flooding the world with steps.
        {
            const tripoint_bub_ms pos = npc_ptr->pos_bub();
            const bool diag = trigdist && pos.x() != dst.x() && pos.y() != dst.y();
            const int cost = npc_ptr->run_cost( here.combined_movecost( pos, dst ), diag );
            if( cost > npc_ptr->get_moves() ) {
                // pool exhausted: the move waits for the next world turn
                continue;
            }
        }
        if( here.inbounds( dst ) && npc_ptr->can_move_to( dst ) ) {
            npc_ptr->move_to( dst );
            // show the step to its own session immediately: without this the
            // movement would wait for the parity tick below
            av_send_session_picture( sk.sid, dst, npc_ptr.get() );
            // parity: this bind acted since the last world turn
            av_acted_since_tick[sk.sid] = true;
            if( av_all_acted() ) {
                // every bound avatar stepped since the last tick: spend the
                // shared world turn (host pause), then wait for the next
                // action batch — strictly one world turn at a time. The
                // host's turn machinery needs a vanilla host-anchored
                // bubble, so hand the bubble back before the pause runs.
                av_acted_since_tick.clear();
                g->mp_anchor_map_on( get_avatar() );
                netplay::host_push_legacy_key( 11 );
                break;
            }
        }
    }
    // DDA MP v4.0a-2: when the burst is over (no more client steps queued),
    // hand the bubble back to the host player so its own turn machinery
    // (input, rendering, do_turn) always runs vanilla. While a client keeps
    // stepping, the bubble stays on the avatar: a host restore per step is
    // what made far-roaming feel like 2 steps a second.
    if( !netplay::host_avatar_keys_pending() ) {
        g->mp_anchor_map_on( get_avatar() );
    }
}

} // namespace netplay
