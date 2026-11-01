#pragma once
#ifndef CATA_SRC_RANGED_H
#define CATA_SRC_RANGED_H

#include <string>
#include <vector>

#include "coordinates.h"

class Character;
class aim_activity_actor;
class avatar;
class gun_mode;
class item;
class item_location;
class map;
class spell;
class turret_data;
class vehicle;
enum class creature_size : int;
struct vehicle_part;

// Recoil change less or equal to this value (in MoA) stops further aiming
constexpr double MIN_RECOIL_IMPROVEMENT = 0.01;

namespace target_handler
{
// Trajectory to target. Empty if selection was aborted or player ran out of moves
using trajectory = std::vector<tripoint_bub_ms>;

/** Generic target select without fire something */
trajectory mode_select_only( avatar &you, int range );

/**
 * Firing ranged weapon. This mode allows spending moves on aiming.
 */
trajectory mode_fire( avatar &you, aim_activity_actor &activity );

/** Throwing item */
trajectory mode_throw( Character &you, item &relevant, bool blind_throwing );

/// DDA MP: plain-text lines of the host's throw aiming panel (range, target,
/// aim chances, moves) for a remote client. No UI is opened.
std::vector<std::string> netplay_throw_aim_lines( Character &you, item &weapon,
        const tripoint_bub_ms &target );

/// DDA MP: cells of the host's rendered aiming window (w_target) for a remote
/// client; colour is a base-16 palette index (same encoding as sidebar cells).
struct netplay_aim_cell {
    std::string text;
    int color = 0;
};
struct netplay_aim_panel {
    int width = 0;
    int height = 0;
    std::string title;
    std::vector<netplay_aim_cell> cells;
};
netplay_aim_panel netplay_throw_panel( Character &you, item &weapon,
                                       const tripoint_bub_ms &target );

/// DDA MP: the same chances computed client-side from a temp Character and
/// snapshot facts (no host round-trip). Returns plain lines with <color_x>.
std::vector<std::string> netplay_throw_aim_client( Character &you, item &weapon,
        const std::string &mtype, double range, int max_range, float ambient_light,
        bool sees );

/** Reach attacking */
trajectory mode_reach( avatar &you, item_location weapon );

/** Unarmed reach attacking */
trajectory mode_unarmed_reach( avatar &you );

/** Manually firing vehicle turret */
trajectory mode_turret_manual( avatar &you, turret_data &turret );

/** Selecting target for turrets (when using vehicle controls) */
trajectory mode_turrets( avatar &you, vehicle &veh, const std::vector<vehicle_part *> &turrets );

/** Casting a spell */
trajectory mode_spell( avatar &you, spell &casting, bool no_fail, bool no_mana );
} // namespace target_handler

void practice_archery_proficiency( Character &p, const item &relevant );

int range_with_even_chance_of_good_hit( int dispersion );

/**
 * Common checks for gunmode (when firing a weapon / manually firing turret)
 * @param messages Used to store messages describing failed checks
 * @return True if all conditions are true
 */
bool gunmode_checks_common( Character &you, const map &m, std::vector<std::string> &messages,
                            const gun_mode &gmode );

/**
 * Various checks for gunmode when firing a weapon
 * @param messages Used to store messages describing failed checks
 * @return True if all conditions are true
 */
bool gunmode_checks_weapon( Character &you, const map &m, std::vector<std::string> &messages,
                            const gun_mode &gmode );

int throw_cost( const Character &c, const item &to_throw );

// check for steadiness for a given pos
double calc_steadiness( const Character &you, const item &weapon, const tripoint_bub_ms &pos,
                        double predicted_recoil );

double calculate_aim_cap( const Character &you, const tripoint_bub_ms &target );

double occupied_tile_fraction( creature_size target_size );

struct Target_attributes {
    int range = 1;
    double size = 0.5;
    double size_in_moa = 10800.0;
    float light = 0.0f;
    bool visible = true;
    explicit Target_attributes() = default;
    explicit Target_attributes( tripoint_bub_ms src, tripoint_bub_ms target );
    explicit Target_attributes( int rng, double size, float l, bool can_see );
};

#endif // CATA_SRC_RANGED_H
