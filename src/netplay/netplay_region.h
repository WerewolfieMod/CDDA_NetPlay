#pragma once
#ifndef CATA_SRC_NETPLAY_NETPLAY_REGION_H
#define CATA_SRC_NETPLAY_NETPLAY_REGION_H

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "coordinates.h"
#include "memory_fast.h"

class Character;
class map;
class monster;
class npc;

namespace netplay
{

/* DDA MP v4.0a-142 (bubble split, step 3): per-player simulation regions.
 *
 * The host region (id 0) is the engine's own map (g->m) and the host avatar.
 * A split region owns a second map instance that follows one or more session
 * avatars, so two players can walk independently and each keeps a live bubble
 * around itself. Both maps are views onto the shared MAPBUFFER submaps; the
 * turn loop runs them one after another (the engine stays single-threaded),
 * and the creature tracker is switched per region so neither map ever ticks
 * or despawns the other region's creatures.
 *
 * Everything here is main-thread only. */

struct mp_saved_monster {
    int ax = 0;
    int ay = 0;
    int az = 0;
    std::string json;
};

struct bubble_region {
    int id = 0;
    /// Null for the host region (id 0), which uses g->m.
    std::unique_ptr<map> owned;
    /// Last anchor of the region (absolute); used for clustering.
    tripoint_abs_ms center = tripoint_abs_ms::zero;
    /// Session ids currently assigned to this region.
    std::vector<int> sessions;
    /// Live monsters detached from the tracker while another region is active.
    std::vector<shared_ptr_fast<monster>> monsters;
    /// Monsters that left this region's map during a shift (serialized state).
    std::map<tripoint_abs_ms, mp_saved_monster> saved;
};

/// (Re)compute session -> region clustering and create/destroy split regions.
/// Call on the main thread before processing per-session work.
void host_region_sync();

/// Run one world tick for every split region, in that region's context. The
/// callback receives the region's primary player character and runs all the
/// map-bound simulation the host turn just ran for its own bubble; the engine
/// remains single-threaded, so the regions are processed one after another in
/// the same turn. No-op when not hosting or when no region is split.
void host_tick_split_regions( const std::function<void( Character & )> &tick );

/// Drop all split regions (host shutdown). Safe to call when not hosting.
void host_region_shutdown();

/// True when the session's avatar lives in a second map.
bool host_session_region_split( int sid );

/// Region id of a session (0 = host region, -1 = no session/avatar).
int host_session_region( int sid );

/// DDA MP v4.0a-170: session whose split region owns this map, or -1 when the
/// map is the host's own bubble or not a region map at all.
int host_session_for_region_map( const map *m );

/// The session avatar npc (defined in netplay_world.cpp, where the binding
/// table lives). Null when the session is unbound or the avatar is gone.
npc *host_session_avatar_npc( int sid );

/// RAII: run engine code in the engine context of the session's region: the
/// region map (through current_map), the avatar as the player view/character
/// and the region's creatures in the tracker. Nested scopes are supported;
/// the outermost scope restores the region that was active before it.
class region_scope
{
    public:
        explicit region_scope( int sid );
        ~region_scope();
        region_scope( const region_scope & ) = delete;
        region_scope &operator=( const region_scope & ) = delete;

        /// True when the scope actually points at a split region.
        bool split() const;

    private:
        struct impl;
        std::unique_ptr<impl> p;
};

} // namespace netplay

#endif // CATA_SRC_NETPLAY_NETPLAY_REGION_H
