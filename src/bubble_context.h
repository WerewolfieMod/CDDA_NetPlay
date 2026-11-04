#pragma once
#ifndef CATA_SRC_BUBBLE_CONTEXT_H
#define CATA_SRC_BUBBLE_CONTEXT_H

#include "current_map.h"

class Character;
class map;

/* DDA MP v4.0a-140 (bubble split, step 1): the engine's current region view.
 *
 * The defaults (nullptr) mean the ordinary single-player world: get_map() is
 * whatever current_map points to (normally g->m), reality_bubble() is g->m and
 * the player view/character is g->u.
 *
 * Inside swap_bubble_context all three accessors follow the same region: a
 * second map, its own viewer (a session avatar npc) and the reality bubble of
 * that region. This is the context switch the multi-region turn loop and the
 * per-region snapshot builder use. */
struct bubble_context {
    map *here = nullptr;
    Character *viewer = nullptr;
};

inline bubble_context &current_bubble_context()
{
    static bubble_context context;
    return context;
}

/// RAII: run engine code against another region's map and viewer. The map is
/// swapped through current_map, so nested swap_map users (mapgen, camps,
/// explosions) still save and restore it correctly.
class swap_bubble_context
{
    public:
        swap_bubble_context( map &here, Character &viewer )
            : map_swap( here ), previous( current_bubble_context() ) {
            current_bubble_context().here = &here;
            current_bubble_context().viewer = &viewer;
        }
        ~swap_bubble_context() {
            current_bubble_context() = previous;
        }
        swap_bubble_context( const swap_bubble_context & ) = delete;
        swap_bubble_context &operator=( const swap_bubble_context & ) = delete;
    private:
        swap_map map_swap;
        bubble_context previous;
};

#endif // CATA_SRC_BUBBLE_CONTEXT_H
