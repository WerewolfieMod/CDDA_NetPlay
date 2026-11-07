/* DDA MP v4.0a-142 (bubble split, step 3): region manager.
 *
 * One region is the host's own reality bubble; split regions own a second
 * map instance and follow one or more session avatars that walked away from
 * the host. Both maps are views onto the shared MAPBUFFER submaps, so world
 * edits, vehicles and fields remain consistent; the regions only separate
 * the *simulation context* (current map, viewer, creature tracker) and the
 * frame each player receives.
 *
 * The creature tracker is a single engine object. While a region is active
 * it holds exactly that region's creatures: leaving a region detaches its
 * live monsters into the region's store, entering a region puts the stored
 * monsters that lie inside its map back into the tracker. NPCs are left in
 * the overmap buffer (the world npc list), so switching regions never
 * duplicates them: the region's `load_npcs()` brings the right ones back.
 *
 * Main thread only. */

#include "netplay/netplay_region.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <set>
#include <sstream>
#include <utility>

#include "avatar.h"
#include "bubble_context.h"
#include "calendar.h"
#include "creature_tracker.h"
#include "game.h"
#include "json.h"
#include "json_loader.h"
#include "map.h"
#include "map_scale_constants.h"
#include "monster.h"
#include "npc.h"
#include "overmapbuffer.h"
#include "netplay/netplay.h"

namespace netplay
{

namespace
{

/* Keep both players inside one map until they can no longer share a frame
 * with the host. Two maps are resident after the split; they overlap, which
 * is fine because both are views onto the same MAPBUFFER submaps. */
constexpr int region_split_distance = 48;

std::map<int, bubble_region> mp_regions;
/* The host region (id 0) is not in the map above: it uses g->m and the host
 * avatar, but still needs a persistent monster store. */
bubble_region mp_host_region;
/* session id -> region id (0 = host). Recomputed by host_region_sync(). */
std::map<int, int> mp_session_region;
/* Which region's creatures the tracker currently holds. */
int mp_active_region = 0;
int mp_next_region_id = 1;

map *region_map( bubble_region &reg )
{
    return reg.id == 0 ? &g->mp_host_map() : reg.owned.get();
}

bubble_region *find_region( int id )
{
    if( id == 0 ) {
        mp_host_region.id = 0;
        return &mp_host_region;
    }
    const auto it = mp_regions.find( id );
    return it == mp_regions.end() ? nullptr : &it->second;
}

bool in_region( bubble_region &reg, const tripoint_abs_ms &p )
{
    map *here = region_map( reg );
    return here != nullptr && here->inbounds( here->get_bub( p ) );
}

int chebyshev( const tripoint_abs_ms &a, const tripoint_abs_ms &b )
{
    return std::max( std::abs( a.x() - b.x() ), std::abs( a.y() - b.y() ) );
}

void detach_monsters()
{
    bubble_region *reg = find_region( mp_active_region );
    if( reg == nullptr ) {
        return;
    }
    creature_tracker &tracker = get_creature_tracker();
    const std::vector<shared_ptr_fast<monster>> live = tracker.get_monsters_list();
    for( const shared_ptr_fast<monster> &m : live ) {
        if( !m ) {
            continue;
        }
        if( !m->is_dead_state() ) {
            reg->monsters.push_back( m );
        }
        tracker.remove( *m );
    }
    // NPCs stay registered in the overmap buffer; dropping the active list is
    // enough and avoids ever holding the same npc in two region stores.
    tracker.clear_npcs();
}

void attach_monsters( bubble_region &reg )
{
    map *here = region_map( reg );
    if( here == nullptr ) {
        return;
    }
    creature_tracker &tracker = get_creature_tracker();
    std::vector<shared_ptr_fast<monster>> keep;
    keep.reserve( reg.monsters.size() );
    for( const shared_ptr_fast<monster> &m : reg.monsters ) {
        if( !m || m->is_dead_state() ) {
            continue;
        }
        const tripoint_abs_ms ap = m->pos_abs();
        if( !here->inbounds( here->get_bub( ap ) ) ) {
            keep.push_back( m );
            continue;
        }
        if( !tracker.add( m ) ) {
            keep.push_back( m );
        }
    }
    reg.monsters = std::move( keep );
}

/* Put the engine into region `r` (map context + its creatures). Region 0 is
 * the host's own map. Restores nothing; callers wrap the switch in a scope. */
void activate_region( int r )
{
    if( r == mp_active_region ) {
        return;
    }
    bubble_region *target = find_region( r );
    if( target == nullptr ) {
        r = 0;
        target = find_region( 0 );
    }
    detach_monsters();

    map *here = region_map( *target );
    Character *viewer = nullptr;
    if( r == 0 ) {
        viewer = &get_avatar();
    } else if( !target->sessions.empty() ) {
        viewer = host_session_avatar_npc( target->sessions.front() );
    }
    if( viewer == nullptr ) {
        viewer = &get_avatar();
    }
    if( here != nullptr ) {
        swap_bubble_context ctx( *here, *viewer );
        attach_monsters( *target );
        g->load_npcs( here );
    }
    mp_active_region = r;
}

void dispatch_stored_monsters( bubble_region &reg )
{
    for( const shared_ptr_fast<monster> &m : reg.monsters ) {
        if( !m || m->is_dead_state() ) {
            continue;
        }
        m->on_unload();
        overmap_buffer.despawn_monster( *m );
    }
    reg.monsters.clear();

    for( const auto &entry : reg.saved ) {
        const mp_saved_monster &sm = entry.second;
        try {
            JsonValue jv = json_loader::from_string( sm.json );
            auto mon = std::make_shared<monster>();
            mon->deserialize( jv.get_object() );
            mon->setpos( tripoint_abs_ms( sm.ax, sm.ay, sm.az ), false );
            overmap_buffer.despawn_monster( *mon );
        } catch( ... ) {
            // Broken saved state: drop it with the region.
        }
    }
    reg.saved.clear();
}

void destroy_region( int id )
{
    if( id <= 0 ) {
        return;
    }
    if( mp_active_region == id ) {
        activate_region( 0 );
    }
    const auto it = mp_regions.find( id );
    if( it == mp_regions.end() ) {
        return;
    }
    dispatch_stored_monsters( it->second );
    if( netplay::mp_diag_enabled() ) {
        if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
            std::fprintf( f, "region destroy id=%d\n", id );
            std::fclose( f );
        }
    }
    mp_regions.erase( it );
}

/* DDA MP test hook: force the first bound avatar away from the host so an
 * automated run can exercise the split without a second interactive client.
 * DDA_MP_TEST_TELEPORT="dx,dy" (map tiles). Applied once. */
void maybe_test_teleport()
{
    static bool teleported = false;
    static bool returned = false;
    static std::chrono::steady_clock::time_point teleport_time =
        std::chrono::steady_clock::now();

    if( !teleported ) {
        const char *env = std::getenv( "DDA_MP_TEST_TELEPORT" );
        if( env == nullptr || env[0] == '\0' ) {
            return;
        }
        int dx = 0;
        int dy = 0;
        if( std::sscanf( env, "%d,%d", &dx, &dy ) != 2 ) {
            teleported = true;
            return;
        }
        for( int sid : host_session_ids() ) {
            npc *av = host_session_avatar_npc( sid );
            if( av != nullptr ) {
                const tripoint_abs_ms target =
                    get_avatar().pos_abs() + tripoint_rel_ms( dx, dy, 0 );
                av->setpos( target, false );
                if( netplay::mp_diag_enabled() ) {
                    if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
                        std::fprintf( f, "region test teleport sid=%d to=%d,%d\n",
                                      sid, target.x(), target.y() );
                        std::fclose( f );
                    }
                }
                teleported = true;
                teleport_time = std::chrono::steady_clock::now();
                break;
            }
        }
        return;
    }

    // Optional merge test: after N seconds bring the avatar back next to the
    // host; host_region_sync() must then drop region 1 and fold the session
    // into the host bubble again.
    if( returned ) {
        return;
    }
    const char *back = std::getenv( "DDA_MP_TEST_RETURN_AFTER" );
    if( back == nullptr || back[0] == '\0' ) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    if( now - teleport_time < std::chrono::seconds( std::atoi( back ) ) ) {
        return;
    }
    for( int sid : host_session_ids() ) {
        npc *av = host_session_avatar_npc( sid );
        if( av != nullptr ) {
            const tripoint_abs_ms target =
                get_avatar().pos_abs() + tripoint_rel_ms( 10, 0, 0 );
            av->setpos( target, false );
            if( netplay::mp_diag_enabled() ) {
                if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
                    std::fprintf( f, "region test return sid=%d to=%d,%d\n",
                                  sid, target.x(), target.y() );
                    std::fclose( f );
                }
            }
            returned = true;
            break;
        }
    }
}

} // namespace

void host_region_sync()
{
    if( !host_is_running() ) {
        return;
    }
    maybe_test_teleport();

    const tripoint_abs_ms host_pos = get_avatar().pos_abs();
    std::map<int, int> assignment;
    bool changed = false;

    for( int sid : host_session_ids() ) {
        npc *av = host_session_avatar_npc( sid );
        if( av == nullptr ) {
            continue;
        }
        const tripoint_abs_ms pos = av->pos_abs();
        if( chebyshev( pos, host_pos ) <= region_split_distance ) {
            assignment[sid] = 0;
            continue;
        }
        int chosen = 0;
        int best = region_split_distance + 1;
        for( auto &entry : mp_regions ) {
            if( entry.first == 0 || entry.second.owned == nullptr ) {
                continue;
            }
            const int dist = chebyshev( pos, entry.second.center );
            if( dist <= region_split_distance && dist < best ) {
                chosen = entry.first;
                best = dist;
            }
        }
        if( chosen == 0 ) {
            bubble_region reg;
            reg.id = mp_next_region_id++;
            reg.owned = std::make_unique<map>();
            reg.center = pos;
            {
                // map::load() wants the top-left submap of the bubble; putting
                // the avatar exactly there would leave its frame half outside
                // the map, so center it like the game's own load path does.
                const tripoint_abs_sm origin =
                    project_to<coords::sm>( pos ) - point( HALF_MAPSIZE, HALF_MAPSIZE );
                swap_bubble_context ctx( *reg.owned, *av );
                reg.owned->load( origin, false );
            }
            chosen = reg.id;
            if( netplay::mp_diag_enabled() ) {
                if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
                    std::fprintf( f, "region create id=%d center=%d,%d sid=%d\n",
                                  chosen, pos.x(), pos.y(), sid );
                    std::fclose( f );
                }
            }
            mp_regions[chosen] = std::move( reg );
            changed = true;
        }
        assignment[sid] = chosen;
    }

    if( assignment != mp_session_region ) {
        changed = true;
    }
    mp_session_region = std::move( assignment );

    // Refresh each region's session list; drop regions nobody uses any more.
    for( auto &entry : mp_regions ) {
        entry.second.sessions.clear();
    }
    for( const auto &entry : mp_session_region ) {
        if( entry.second > 0 ) {
            const auto it = mp_regions.find( entry.second );
            if( it != mp_regions.end() ) {
                it->second.sessions.push_back( entry.first );
            }
        }
    }
    std::vector<int> unused_regions;
    for( const auto &entry : mp_regions ) {
        if( entry.second.sessions.empty() ) {
            unused_regions.push_back( entry.first );
        }
    }
    for( int id : unused_regions ) {
        destroy_region( id );
        changed = true;
    }
    // Keep the region center on its primary avatar so it can follow the player
    // across the world instead of staying parked at its creation tile.
    for( auto &entry : mp_regions ) {
        if( entry.second.sessions.empty() ) {
            continue;
        }
        npc *av = host_session_avatar_npc( entry.second.sessions.front() );
        if( av != nullptr ) {
            entry.second.center = av->pos_abs();
        }
    }
    if( changed && netplay::mp_diag_enabled() ) {
        if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
            std::fprintf( f, "region sync done regions=%d active=%d\n",
                          static_cast<int>( mp_regions.size() ), mp_active_region );
            std::fclose( f );
        }
    }
    ( void )changed;
}

void host_region_shutdown()
{
    if( mp_active_region != 0 ) {
        activate_region( 0 );
    }
    for( auto &entry : mp_regions ) {
        dispatch_stored_monsters( entry.second );
    }
    mp_regions.clear();
    mp_session_region.clear();
    mp_next_region_id = 1;
}

bool host_session_region_split( int sid )
{
    const auto it = mp_session_region.find( sid );
    return it != mp_session_region.end() && it->second > 0;
}

int host_session_region( int sid )
{
    const auto it = mp_session_region.find( sid );
    return it == mp_session_region.end() ? -1 : it->second;
}

void host_tick_split_regions( const std::function<void( Character & )> &tick )
{
    if( !host_is_running() || mp_regions.empty() ) {
        return;
    }
    host_region_sync();
    // Snapshot the jobs: region_scope may create/destroy regions through
    // mapgen or an activity, and iterating the live map would be unsafe.
    std::vector<std::pair<int, int>> jobs;
    jobs.reserve( mp_regions.size() );
    for( const auto &entry : mp_regions ) {
        if( !entry.second.sessions.empty() ) {
            jobs.emplace_back( entry.second.sessions.front(), entry.first );
        }
    }
    for( const auto &job : jobs ) {
        npc *viewer = host_session_avatar_npc( job.first );
        if( viewer == nullptr ) {
            continue;
        }
        region_scope scope( job.first );
        if( !scope.split() ) {
            continue;
        }
        if( netplay::mp_diag_enabled() ) {
            if( std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" ) ) {
                std::fprintf( f, "region tick id=%d sid=%d at=%d,%d\n",
                              job.second, job.first, viewer->pos_abs().x(), viewer->pos_abs().y() );
                std::fclose( f );
            }
        }
        tick( *viewer );
    }
}

/* Called by game::mp_anchor_map_on (through netplay.h) before a region's map
 * shifts: park the live monsters with their absolute positions. */
void host_capture_region_monsters()
{
    if( !host_is_running() ) {
        return;
    }
    bubble_region *reg = find_region( mp_active_region );
    if( reg == nullptr ) {
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
        reg->saved[ap] = std::move( sm );
    }
}

/* Called after the shift: resurrect parked monsters whose area is now inside
 * the active region; the rest stay parked for a later shift. */
void host_restore_region_monsters()
{
    if( !host_is_running() ) {
        return;
    }
    bubble_region *reg = find_region( mp_active_region );
    if( reg == nullptr ) {
        return;
    }
    map &here = get_map();
    creature_tracker &tracker = get_creature_tracker();
    for( auto it = reg->saved.begin(); it != reg->saved.end(); ) {
        const mp_saved_monster &sm = it->second;
        const tripoint_abs_ms ap( sm.ax, sm.ay, sm.az );
        if( !here.inbounds( here.get_bub( ap ) ) ) {
            ++it;
            continue;
        }
        bool live = false;
        for( const shared_ptr_fast<monster> &m : tracker.get_monsters_list() ) {
            if( m && m->pos_abs() == ap ) {
                live = true;
                break;
            }
        }
        if( !live ) {
            try {
                JsonValue jv = json_loader::from_string( sm.json );
                auto mon = std::make_shared<monster>();
                mon->deserialize( jv.get_object() );
                mon->setpos( ap, false );
                tracker.add( mon );
            } catch( ... ) {
                // Broken saved state: drop it rather than blocking the region.
            }
        }
        it = reg->saved.erase( it );
    }
}

/* ------------------------------------------------------------------ */
/* region_scope                                                        */
/* ------------------------------------------------------------------ */

struct region_scope::impl {
    int region = -1;
    int previous = 0;
    bool switched = false;
    std::unique_ptr<swap_bubble_context> ctx;
};

region_scope::region_scope( int sid ) : p( std::make_unique<impl>() )
{
    p->previous = mp_active_region;
    p->region = host_session_region( sid );
    if( p->region < 0 ) {
        p->region = 0;
    }
    if( p->region != mp_active_region ) {
        activate_region( p->region );
        p->switched = true;
    }
    bubble_region *reg = find_region( p->region );
    if( reg == nullptr ) {
        return;
    }
    map *here = region_map( *reg );
    Character *viewer = p->region == 0
                        ? static_cast<Character *>( &get_avatar() )
                        : static_cast<Character *>( host_session_avatar_npc( sid ) );
    if( here != nullptr && viewer != nullptr && current_bubble_context().here != here ) {
        p->ctx = std::make_unique<swap_bubble_context>( *here, *viewer );
    }
}

region_scope::~region_scope()
{
    p->ctx.reset();
    if( p->switched ) {
        activate_region( p->previous );
    }
}

bool region_scope::split() const
{
    return p->region > 0;
}

} // namespace netplay
