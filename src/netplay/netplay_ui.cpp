// CataDDA Multiplayer ("DDA MP") — user-facing flows:
//  * in-game "Open world to network" host dialog,
//  * in-game/main-menu "Join by IP…" client flow,
//  * the remote-session fullscreen ASCII viewer (`run_view_loop`).

#include "netplay.h"
#include "inventory_source.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

#include "cursesdef.h"
#include "cursesport.h"
#include "avatar.h"      // get_avatar (native character creation)
#include "bionics.h"     // read-only bionic list on the client mirror
#include "mutation.h"    // read-only mutation list on the client mirror
#include "character_martial_arts.h" // style picker on the client mirror
#include "crafting_gui.h" // select_crafter_and_crafting_recipe (client craft)
#include "recipe.h"       // recipe::ident (craft intent)
#include "recipe_dictionary.h" // recipe_dict / recipe_subset (view recipe)
#include "faction.h"
#include "clzones.h"
#include "field.h"       // fields mirrored into the native map
#include "field_type.h"  // field_type_str_id
#include "creature.h"    // Creature::attitude_raw_string (attitude badge)
#include "debug.h"       // debug_suppress_prompts (fat client load)
#include "init.h"        // DynamicDataLoader (fat client finalize guard)
#include "json.h"        // JsonOut (avatar profile serialization)
#include "json_loader.h" // profile/json cfg parsing
#include "cata_utility.h" // write_to_file / read_from_file_optional (client prefs)
#include "path_info.h"   // PATH_INFO::config_dir (client prefs)
#include "sdl_wrappers.h" // GetTicks (view probe timestamps)
#include "sdltiles.h"     // display_buffer_draw_scope (single-pass client frame)
#include "game.h"        // g (fat client world data load)
#include "weather.h"      // host clock/weather for native lighting
#include "map.h"         // get_map (client-side pickup selector scratch tile)
#include "mapbuffer.h"   // synthetic client-side map bubble
#include "messages.h"    // client journal mirrored into the game's message list
#include "scores_ui.h"   // single-player death scores screen on the client
#include "construction.h"        // client-side construction list (data only)
#include "construction_group.h"  // construction group names
#include "diary.h"        // session-local diary for remote characters
#include "overmapbuffer.h" // weather/overmap context for map::load
#include "overmap_ui.h"    // client-side sky/time window

#include "submap.h"      // synthetic client-side map bubble
#include "lightmap.h"    // lit_level (client light overlay)
#include "mtype.h"
#include "input_context.h"
#include "input.h"
#include "inventory_ui.h"
#include "inventory.h"
#include "item_category.h"
#include "itype.h"
#include "mission.h"
#include "veh_type.h"
#include "game_inventory.h"
#include "flag.h"
#include "ranged.h"
#include "npc.h"
#include "monster.h"
#include "creature_tracker.h"
#include "loading_ui.h"  // loading_ui::done (releases the splash adaptors)
#include "output.h"
#include "uilist.h"
#include "ui_manager.h"
#include "action.h"
#include "auto_note.h"
#include "auto_pickup.h"
#include "bodygraph.h"
#include "distraction_manager.h"
#include "help.h"
#include "options.h"
#include "panels.h"
#include "safemode_ui.h"
#include "ui_extended_description.h"
#include "cata_imgui.h"
#include "imgui/imgui.h"
#include "worldfactory.h" // world_generator / WORLD (fat client)
#include "mod_manager.h"  // check_mods_list (fat client world load)

#include "string_input_popup.h"
#include "catacharset.h"
#include "wcwidth.h"
#include "color.h"
#include "string_formatter.h"
#include "translations.h"

#include <fstream>
#include <sstream>
#include <thread>
#include <vector>

namespace netplay
{

/// DDA MP v4.0a-186: full-screen map view built from the streamed overmap
/// minimap block. Opened from the client action menu, closed by any key.
static bool mp_map_open = false;
/// DDA MP v4.0a-210: diary is session-local; session characters have no avatar
/// diary object on the host.
static std::unique_ptr<diary> mp_session_diary;

/* DDA MP v4.0a-86: one-line long-wait notice, drawn by the host under the
 * "Waiting for other players..." line. No-op while the mode is off. */
void host_draw_long_wait_hint( int row )
{
    if( !long_wait_active() ) {
        return;
    }
    const int w = std::min( TERMX, 48 );
    static catacurses::window wins[2];
    static int win_w[2] = { -1, -1 };
    const int slot = row == 1 ? 1 : 0;
    catacurses::window &win = wins[slot];
    if( !win || win_w[slot] != w ) {
        win = catacurses::newwin( 1, w, point( std::max( 0, ( TERMX - w ) / 2 ), slot ) );
        win_w[slot] = w;
        if( !win ) {
            return;
        }
    }
    werase( win );
    mvwprintz( win, point( 0, 0 ), c_light_green, _( "Long wait mode is on" ) );
    wnoutrefresh( win );
}

/* DDA MP v4.0a-180: progress line for the host while it helps a client. Draws
 * on the requested top row; when the hint becomes empty the row is erased once
 * and further calls are no-ops. */
void host_draw_help_hint( int row )
{
    const std::string &hint = host_help_hint();
    const int w = std::min( TERMX, 64 );
    static catacurses::window wins[2];
    static int win_w[2] = { -1, -1 };
    static bool cleared[2] = { true, true };
    const int slot = row == 1 ? 1 : 0;
    if( hint.empty() && cleared[slot] ) {
        return;
    }
    catacurses::window &win = wins[slot];
    if( !win || win_w[slot] != w ) {
        win = catacurses::newwin( 1, w, point( std::max( 0, ( TERMX - w ) / 2 ), slot ) );
        win_w[slot] = w;
        if( !win ) {
            return;
        }
    }
    werase( win );
    if( hint.empty() ) {
        cleared[slot] = true;
    } else {
        mvwprintz( win, point( 0, 0 ), c_light_blue, hint );
        cleared[slot] = false;
    }
    wnoutrefresh( win );
}

/* ------------------------------------------------------------------ */
/* chunk helpers                                                       */
/* ------------------------------------------------------------------ */

/// DDA MP v4.0a-129: true once the character JSON has been applied to g->u.
static bool mp_native_avatar_ready = false;

std::string describe_endpoint()
{
    if( host_is_running() ) {
        return string_format( netplay::fork_text( "[%d] Listening on %d" ), host_session_count(), host_listen_port() );
    }
    return netplay::fork_text( "[x] Not listening" );
}

/* ------------------------------------------------------------------ */
/* in-game host dialog ("Open world to network")                       */
/* ------------------------------------------------------------------ */

/* DDA MP v4.0a-173: in-game co-op instructions. The text lives in fork_text()
 * so it follows the player's language like the rest of the fork's UI. */
void show_coop_help()
{
    std::vector<std::string> lines;
    const auto add_block = [&lines]( const char *key ) {
        const std::string text = fork_text( key );
        std::size_t start = 0;
        while( true ) {
            const std::size_t nl = text.find( '\n', start );
            if( nl == std::string::npos ) {
                lines.push_back( text.substr( start ) );
                break;
            }
            lines.push_back( text.substr( start, nl - start ) );
            start = nl + 1;
        }
    };
    lines.emplace_back();
    add_block( "1. Host: start or load a world as usual, press ESC and pick "
               "\"Network: open world for co-op\".\n"
               "Clients connect to your address on port 25565.\n"
               "While they play, turn on \"Long wait mode\" in the same menu - the host then "
               "passes its turns automatically." );
    lines.emplace_back();
    add_block( "2. Client: in the main menu pick \"Join Network World\", enter the host address "
               "and a nickname.\n"
               "Or start the game with:  cataclysm-tiles.exe --mpclient <host[:port]>" );
    lines.emplace_back();
    add_block( "3. Controls: movement and actions use the same keys as in single player.\n"
               "The action menu, inventory, look, aiming/throwing, chat, butchering, construction "
               "and sleep work.\n"
               "Your journal and the map come from the host." );
    lines.emplace_back();
    add_block( "4. Notes: world time advances only after every player has acted; sleep, crafting "
               "and other long activities are exceptions.\n"
               "Some actions are not available to clients yet.\n"
               "Host and client may use different interface languages - each player reads "
               "messages in their own language." );

    catacurses::window w = catacurses::newwin( TERMY, TERMX, point( 0, 0 ) );
    input_context ctxt( "MULTIPLAYER_HELP" );
    ctxt.register_action( "UP" );
    ctxt.register_action( "DOWN" );
    ctxt.register_action( "PAGE_UP" );
    ctxt.register_action( "PAGE_DOWN" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );

    int scroll = 0;
    const int body = std::max( 1, TERMY - 3 );
    const int max_scroll = std::max( 0, static_cast<int>( lines.size() ) - body );
    while( true ) {
        werase( w );
        draw_border( w, c_white, fork_text( "Co-op help" ) );
        for( int i = 0; i < body; ++i ) {
            const int idx = scroll + i;
            if( idx >= static_cast<int>( lines.size() ) ) {
                break;
            }
            mvwprintz( w, point( 2, 1 + i ), c_white, lines[idx] );
        }
        mvwprintz( w, point( 2, TERMY - 2 ), c_dark_gray,
                   fork_text( "Up/Down: scroll   Esc: close" ) );
        wnoutrefresh( w );
        ui_manager::redraw();
        const std::string action = ctxt.handle_input();
        if( action == "QUIT" ) {
            break;
        } else if( action == "DOWN" ) {
            scroll = std::min( scroll + 1, max_scroll );
        } else if( action == "UP" ) {
            scroll = std::max( 0, scroll - 1 );
        } else if( action == "PAGE_DOWN" ) {
            scroll = std::min( scroll + body, max_scroll );
        } else if( action == "PAGE_UP" ) {
            scroll = std::max( 0, scroll - body );
        }
    }
}

void show_host_dialog()
{
    input_context ctxt( "NETPLAY_HOST", keyboard_mode::keycode );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );

    // DDA MP v4.0a-99: keep serving connected clients while this dialog stays
    // open. It used to block on input, so a host that left the dialog up never
    // answered a join (avatar ops/snapshots queued until ESC). 100 ms keeps the
    // join latency low without repainting this menu 30 times a second.
    ctxt.set_timeout( 100 );

    while( true ) {
        if( host_is_running() ) {
            host_pump_snapshot_requests();
            host_pump_avatar_ops();
            host_pump_avatar_keys();
            host_pump_inventory_actions();
            host_pump_client_actions();
            host_pump_intents();
        }
        ui_manager::redraw();

        const int height = 12;
        const int width  = 60;
        const int x_start = std::max( 0, ( TERMX - width ) / 2 );
        const int y_start = std::max( 0, ( TERMY - height ) / 2 );

        catacurses::window win =
            catacurses::newwin( height, width, point( x_start, y_start ) );

        werase( win );
        draw_border( win, c_white, _( "Open world to network" ) );
        mvwprintw( win, point( 2, 2 ), string_format( netplay::fork_text( "Protocol: DDA MP 0.1  Port: %d" ), DEFAULT_PORT ) );
        mvwprintw( win, point( 2, 3 ), describe_endpoint() );
        if( !host_is_running() ) {
            mvwprintw( win, point( 2, 4 ),
                       netplay::fork_text( "<ENTER>: start listener on port 25565" ) );
        } else {
            mvwprintw( win, point( 2, 4 ),
                       netplay::fork_text( "<ENTER>: stop listener" ) );
            const std::vector<std::string> names = host_session_names();
            int line = 5;
            if( names.empty() ) {
                mvwprintw( win, point( 2, line ), _( "No players connected yet." ) );
                line++;
            } else {
                mvwprintw( win, point( 2, line ), string_format( netplay::fork_text( "Players (%d):" ), static_cast<int>( names.size() ) ) );
                line++;
                for( const std::string &n : names ) {
                    if( line > height - 4 ) {
                        break;
                    }
                    mvwprintw( win, point( 3, line ), "- " + n );
                    line++;
                }
            }
        }
        mvwprintw( win, point( 2, height - 3 ), _( "<ESC>: close and return" ) );

        wnoutrefresh( win );

        const std::string action = ctxt.handle_input();
        if( action == "QUIT" ) {
            break;
        } else if( action == "CONFIRM" ) {
            if( !host_is_running() ) {
                if( netplay::host_start( netplay::DEFAULT_PORT ) ) {
                } else {
                    popup( string_format( netplay::fork_text( "Failed to bind the listener. Port %d may be in use." ), DEFAULT_PORT ) );
                }
            } else {
                netplay::host_stop();
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* fat client: host world static data                                  */
/* ------------------------------------------------------------------ */

static std::string fat_client_loaded_world;

/* DDA MP v3.9: diagnostics master switch (fat-load notes, tiles frame
 * traces, view probes) - built-in (mp_diag_dir). Default ON without any
 * env; DDA_MP_DIAG=0|off disables. */
static const bool mp_netplay_diag = mp_diag_enabled();

// Appends to netplay_tiles.log (same channel as the replay renderer trace).
static void mp_fat_log( const std::string &msg )
{
    if( !mp_netplay_diag ) {
        return;
    }
    try {
        std::ofstream f( netplay::mp_diag_dir() + "\\netplay_tiles.log", std::ios::app );
        if( f ) {
            f << msg << "\n";
        }
    } catch( ... ) { }
}

/// Look the host world up without worldfactory::get_world()'s
/// "Requested non-existing world, prepare for crash" debug message. A client
/// that never opened the world menu has an empty world list, and that modal
/// used to block the whole join flow (it waits for SPACE).
static WORLD *mp_find_local_world( const std::string &name )
{
    if( world_generator == nullptr || name.empty() ) {
        return nullptr;
    }
    const auto &all = world_generator->get_all_worlds();
    const auto it = all.find( name );
    return it == all.end() ? nullptr : it->second.get();
}

/// Load the host-announced world's mod data on the client (idempotent).
/// Call right after the connection is accepted, so world/mod loading starts
/// immediately instead of waiting for the first frame / user input.
static void fat_client_load_data()
{
    std::string wname = client_host_world_name();
    // The WELCOME frame races with client_connect returning: give the
    // reader thread a moment so the "join" click leads straight into
    // world data loading.
    for( int i = 0; wname.empty() && client_is_connected() && i < 40; i++ ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 50 ) );
        wname = client_host_world_name();
    }
    if( wname.empty() || wname == fat_client_loaded_world ) {
        return;
    }
    fat_client_loaded_world = wname;
    mp_fat_log( "tiles: fat client loading world '" + wname + "'" );
    try {
        if( DynamicDataLoader::get_instance().is_data_finalized() ) {
            // This process already played a local game: static data is
            // finalized and additive loading would assert. Keep the
            // ascii + snapshot drawing path.
            mp_fat_log( "tiles: fat client skipped, static data already finalized" );
        } else {
            WORLD *w = mp_find_local_world( wname );
            // DDA MP v4.0a-122: no local world? Build a virtual one from the
            // host's WELCOME mod list. Only the mod packs matter for loading
            // the game data; map/world state arrives in snapshots.
            static std::unique_ptr<WORLD> mp_virtual_world;
            if( w == nullptr ) {
                const std::vector<std::string> host_mods = netplay::client_host_mods();
                if( !host_mods.empty() ) {
                    mp_virtual_world = std::make_unique<WORLD>( wname );
                    mp_virtual_world->active_mod_order.clear();
                    for( const std::string &id : host_mods ) {
                        mp_virtual_world->active_mod_order.emplace_back( mod_id( id ) );
                    }
                    w = mp_virtual_world.get();
                    mp_fat_log( string_format( "tiles: virtual world from host mods (%d)",
                                               static_cast<int>( host_mods.size() ) ) );
                }
            }
            if( w != nullptr && g != nullptr ) {
                // DDA MP v4.0a-123: adopt the host's world options so the
                // client's season length, spawn rates and map rules match.
                for( const auto &kv : netplay::client_host_world_options() ) {
                    if( get_options().has_option( kv.first ) &&
                        get_options().get_option( kv.first ).getPage() == "world_default" ) {
                        w->WORLD_OPTIONS[kv.first].setValue( kv.second );
                    }
                }
                world_generator->set_active_world( w );
                debug_suppress_prompts( true );
                // Mirror game::setup(): core JSON first, then the world's mod
                // packs (loading only the packs leaves the core definitions -
                // traps, terrain, items - undefined and the first mapgen with
                // an unknown id aborts the whole load).
                if( !g->is_core_data_loaded() ) {
                    g->load_core_data();
                }
                world_generator->get_mod_manager().check_mods_list( w );
                g->load_world_modfiles();
                // DDA MP v4.0a-122: the map object was constructed before the
                // world's packs were registered, so its trap table is too
                // small; writing a trap terrain (e.g. t_gutter_downspout)
                // would index past traplocs. Rebuild the map after loading,
                // exactly like game::load does.
                get_map() = map();
                panel_manager::get_manager().init();
                debug_suppress_prompts( false );
                // Same as game_io's own world load flow: release the
                // loading screen afterwards, otherwise its splash/adaptors
                // repaint over our fullscreen replay on every ui redraw.
                loading_ui::done();
                mp_fat_log( "tiles: fat client world data ok" );
            } else {
                mp_fat_log( "tiles: fat client world '" + wname +
                            "' not found locally; basic fidelity only" );
            }
        }
    } catch( const std::exception &err ) {
        debug_suppress_prompts( false );
        mp_fat_log( std::string( "tiles: fat client load EXCEPTION " ) + err.what() );
    } catch( ... ) {
        debug_suppress_prompts( false );
        mp_fat_log( "tiles: fat client load EXCEPTION unknown" );
    }
}

/* ------------------------------------------------------------------ */
/* DDA MP v4.0: avatar bind flow                                       */
/* ------------------------------------------------------------------ */

/* Wait for the host's answer to a resume/profile request. Returns the
 * final bind state: 0 = connection dropped, 2 = accepted, 3 = denied,
 * -1 = timed out. */
static int avatar_wait_answer( int timeout_seconds )
{
    for( int i = 0; i < timeout_seconds * 10; ++i ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        const int st = netplay::client_avatar_bind_state();
        if( st == 2 || st == 3 ) {
            return st;
        }
        if( !netplay::client_is_connected() ) {
            return 0;
        }
    }
    return -1;
}

/* Build a freshly created avatar on this machine through the native
 * character creation UI, then offer its profile to the host. Returns
 * true when the host accepted the avatar. */
static bool avatar_create_flow( const std::string &nick )
{
    if( !DynamicDataLoader::get_instance().is_data_finalized() ) {
        popup( _( "The host world data is not loaded on this machine, "
                  "so a new avatar cannot be created here (watch mode only)." ) );
        return false;
    }

    // DDA MP v4.0a-214 test hook: headless avatar creation. The native
    // creator needs a user, and a client without a local world copy would
    // otherwise stop at a modal popup, which blocks automated runs.
    if( const char *auto_create = std::getenv( "DDA_MP_TEST_AVATAR_AUTO" );
        auto_create != nullptr && auto_create[0] != '\0' &&
        std::string( auto_create ) != "0" ) {
        avatar &auto_pc = get_avatar();
        auto_pc.name = nick;
        auto_pc.play_name = nick;
        // The default avatar never ran the creator, so its body map is empty
        // and the host would treat it as having no vital parts.
        auto_pc.set_body();
        auto_pc.set_all_parts_hp_to_max();
        // The default avatar never ran the creator, so speed_base is still 0;
        // a remote avatar with speed 0 can never spend a move point.
        auto_pc.set_speed_base( 100 );
        auto_pc.set_stamina( auto_pc.get_stamina_max() );
        auto_pc.set_moves( 100 );
        // A bag gives the test avatar pockets; without one every i_add falls
        // back to wielding the item, which item-based goals do not see.
        item bag( itype_id( "backpack" ), calendar::turn );
        auto_pc.wear_item( bag, false );
        std::ostringstream aos;
        JsonOut ajo( aos );
        auto_pc.serialize( ajo );
        netplay::client_send_avatar_profile( nick, aos.str() );
        const int auto_st = avatar_wait_answer( 30 );
        if( auto_st == 2 ) {
            return true;
        }
        if( auto_st == 3 ) {
            popup( _( "The host refused the avatar: %s" ), netplay::client_avatar_error() );
        } else if( auto_st == 0 ) {
            popup( _( "The host disconnected." ) );
        } else {
            popup( _( "The host did not answer the avatar profile." ) );
        }
        return false;
    }

    // Character creation needs the host's world data: the fat client
    // loaded it exactly when DynamicDataLoader finalized and the host's
    // world is the active one.
    const std::string wname = netplay::client_host_world_name();
    WORLD *w = mp_find_local_world( wname );
    if( w == nullptr || world_generator->active_world != w ) {
        popup( _( "The host world '%s' is not available on this machine, "
                  "so a new avatar cannot be created here (watch mode only)." ), wname );
        return false;
    }

    avatar &pc = get_avatar();
    if( !pc.create( character_type::CUSTOM ) ) {
        // the user aborted the native creator, or the name clash prompt
        // was declined
        return false;
    }
    pc.add_profession_items();
    std::ostringstream os;
    JsonOut jo( os );
    pc.serialize( jo );
    netplay::client_send_avatar_profile( nick, os.str() );

    const int st = avatar_wait_answer( 30 );
    if( st == 2 ) {
        return true;
    }
    if( st == 3 ) {
        popup( _( "The host refused the avatar: %s" ), netplay::client_avatar_error() );
    } else if( st == 0 ) {
        popup( _( "The host disconnected." ) );
    } else {
        popup( _( "The host did not answer the avatar profile." ) );
    }
    return false;
}

/* The client's own avatar join flow, run after the fat client loaded the
 * host's world data. The nickname comes from the join dialog (and is kept
 * between restarts in the client prefs). Asks the host for the avatar
 * registry, resumes an alive avatar (auto-picked when there is exactly one)
 * or goes straight to a fresh creation through the native creator UI. */
/* DDA MP v4.0a-216: the nickname of this session, kept so the client can
 * create a fresh character after death without reconnecting. */
static std::string mp_session_nick;

static bool run_avatar_join_flow( const std::string &use_nick )
{
    mp_session_nick = use_nick;
    netplay::client_send_avatar_list_req( use_nick );
    for( int i = 0; i < 150; ++i ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        if( !netplay::client_is_connected() ) {
            popup( _( "The host disconnected." ) );
            return false;
        }
        if( netplay::client_avatar_list_ready() ||
            netplay::client_avatar_bind_state() == 3 ) {
            break;
        }
        // the wait now ends as soon as the host answers the list request,
        // even when the world has no avatars at all (treated as "create")
    }
    if( !netplay::client_is_connected() ) {
        popup( _( "The host disconnected." ) );
        return false;
    }
    if( netplay::client_avatar_bind_state() == 3 ) {
        popup( _( "The host cut the avatar exchange short: %s" ),
               netplay::client_avatar_error() );
    }

    // DDA MP v4.0a-85: client_avatar_list() returns a copy, so keep values, not
    // pointers into the temporary (they used to dangle right after the loop).
    bool has_alive = false;
    for( const netplay::avatar_entry &e : netplay::client_avatar_list() ) {
        if( e.alive ) {
            has_alive = true;
            break;
        }
    }

    if( !has_alive ) {
        // v4.0a-1: no live avatar for this nick goes straight to creation
        return avatar_create_flow( use_nick );
    }

    // v4.0a-1: the host keeps one avatar per nick, so the client resumes
    // the first (only) living entry automatically - no choice is offered.
    netplay::client_send_avatar_resume( use_nick );

    const int st = avatar_wait_answer( 10 );
    if( st != 2 ) {
        if( st == 3 ) {
            popup( _( "Bind refused: %s" ), netplay::client_avatar_error() );
        } else if( st == 0 ) {
            popup( _( "The host disconnected." ) );
        } else {
            popup( _( "The host did not answer the avatar bind." ) );
        }
        // fall through into plain watch mode
        return false;
    }
    return true;
}

/* ------------------------------------------------------------------ */
/* join dialogs                                                        */
/* ------------------------------------------------------------------ */

/* client prefs: last host address + nickname, kept between game restarts */
static void join_load_prefs( std::string &addr, std::string &nick )
{
    try {
        std::string text;
        // PATH_INFO::config_dir() already ends with a slash
        const std::string cfg_path = PATH_INFO::config_dir() + "netplay_client.cfg";
        read_from_file_optional( cfg_path, [&]( std::istream &is ) {
            std::ostringstream ss;
            ss << is.rdbuf();
            text = ss.str();
        } );
        if( text.empty() ) {
            return;
        }
        JsonValue jsin = json_loader::from_string( text );
        JsonObject jo = jsin.get_object();
        jo.read( "address", addr );
        jo.read( "nick", nick );
    } catch( ... ) {
        // missing/broken prefs: fall back to the defaults - never block joining
    }
}

static void join_save_prefs( const std::string &addr, const std::string &nick )
{
    try {
        // PATH_INFO::config_dir() already ends with a slash
        const std::string cfg_path = PATH_INFO::config_dir() + "netplay_client.cfg";
        write_to_file( cfg_path, [&]( std::ostream &os ) {
            JsonOut json( os );
            json.start_object();
            json.member( "address", addr );
            json.member( "nick", nick );
            json.end_object();
        } );
    } catch( ... ) { }
}

/* The join screen: host address, client nickname and a Connect item on one
 * screen. UP/DOWN moves the selection, typing edits the selected field and
 * ENTER on Connect starts the session; ESC cancels. The STRING_INPUT context
 * category reuses the engine's default text-editing bindings (ENTER/ESC/
 * BACKSPACE/UP/DOWN arrive from the default keybindings of that category). */
static bool join_fields_dialog( std::string &addr, std::string &nick )
{
    const int height = 11;
    const int width = 62;
    const int x_start = std::max( 0, ( TERMX - width ) / 2 );
    const int y_start = std::max( 0, ( TERMY - height ) / 2 );

    enum { SEL_ADDR = 0, SEL_NICK = 1, SEL_CONNECT = 2 };

    std::string fields[2] = { addr, nick };
    int sel = SEL_ADDR;

    input_context ctxt( "STRING_INPUT", keyboard_mode::keychar );
    ctxt.register_action( "TEXT.QUIT" );
    ctxt.register_action( "TEXT.CONFIRM" );
    ctxt.register_action( "TEXT.BACKSPACE" );
    ctxt.register_action( "TEXT.CLEAR" );
    ctxt.register_action( "UP" );
    ctxt.register_action( "DOWN" );
    ctxt.register_action( "ANY_INPUT" );

    while( true ) {
        ui_manager::redraw();

        catacurses::window win =
            catacurses::newwin( height, width, point( x_start, y_start ) );
        werase( win );
        draw_border( win, c_white, _( "Join by IP" ) );

        mvwprintw( win, point( 2, 2 ), "%s%s",
                   sel == SEL_ADDR ? "> " : "  ", _( "Host address:" ) );
        mvwprintw( win, point( 20, 2 ), "%s", fields[0].c_str() );
        mvwprintw( win, point( 2, 4 ), "%s%s",
                   sel == SEL_NICK ? "> " : "  ", _( "Your nickname:" ) );
        mvwprintw( win, point( 20, 4 ), "%s", fields[1].c_str() );
        // the connect button, as its own selectable item
        mvwprintw( win, point( 2, 7 ), "%s",
                   sel == SEL_CONNECT ? "> [ Connect ] <" : "  [ Connect ]" );
        mvwprintw( win, point( 2, height - 2 ),
                   netplay::fork_text( "<UP>/<DOWN>: select   <ENTER>: connect   <ESC>: cancel" ) );

        wnoutrefresh( win );

        const std::string action = ctxt.handle_input();
        const input_event ev = ctxt.get_raw_input();
        if( action == "TEXT.QUIT" ) {
            return false;
        }
        if( action == "UP" ) {
            // wrap upward: 0 -> 2, otherwise sel - 1
            sel = ( sel + 3 - 1 ) % 3;
            continue;
        }
        if( action == "DOWN" ) {
            sel = ( sel + 1 ) % 3;
            continue;
        }
        if( action == "TEXT.CONFIRM" ) {
            if( fields[0].empty() ) {
                popup( netplay::fork_text( "The host address can not be empty." ) );
                continue;
            }
            addr = fields[0];
            nick = fields[1].empty() ? std::string( "player" ) : fields[1];
            return true;
        }
        if( sel == SEL_CONNECT ) {
            // editing keys do nothing while the connect button is selected
            continue;
        }
        if( action == "TEXT.BACKSPACE" ) {
            std::string &f = fields[sel];
            if( !f.empty() ) {
                f.pop_back();
            }
            continue;
        }
        if( action == "TEXT.CLEAR" ) {
            fields[sel].clear();
            continue;
        }
        if( action == "ANY_INPUT" && !ev.text.empty() ) {
            std::string &f = fields[sel];
            const char *str = ev.text.c_str();
            int len = static_cast<int>( ev.text.length() );
            while( len > 0 ) {
                const uint32_t ch = UTF8_getch( &str, &len );
                // mk_wcwidth filters out control characters
                if( mk_wcwidth( ch ) >= 0 && f.size() < 40 ) {
                    f += utf32_to_utf8( ch );
                }
            }
            continue;
        }
    }
}

void show_join_dialog()
{
    std::string addr = "127.0.0.1";
    std::string nick = "player";
    join_load_prefs( addr, nick );

    if( !join_fields_dialog( addr, nick ) ) {
        return; // cancelled
    }
    join_save_prefs( addr, nick );

    if( !netplay::client_connect( addr ) ) {
        popup( netplay::fork_text( "Could not connect: the host rejected the connection." ) );
        return;
    }

    // A brief welcome is likely streaming immediately; blocking poll.
    for( int i = 0; i < 50 && !client_is_connected(); i++ ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
    if( !client_is_connected() ) {
        const std::string reason = client_deny_reason();
        if( reason.empty() ) {
            popup( netplay::fork_text( "Connection refused: host unreachable." ) );
        } else {
            popup( string_format( netplay::fork_text( "Connection refused: %s" ), reason ) );
        }
        return;
    }

    // DDA MP v3.9: ask for the current picture right after the WELCOME -
    // the host answers while the client loads its world data, so the view
    // loop starts with a frame already buffered and shows tiles instantly.
    // If the host plays during the load, its tick snapshots keep the buffer
    // fresh; the view loop's own request loop stays as a fallback.
    netplay::client_request_snapshot();

    // DDA MP fat client: the host announced its world in WELCOME. Load its
    // mod data now — before entering the fullscreen view — so the loading
    // starts immediately upon the user's join confirmation, not after the
    // first input is pumped in the view loop.
    fat_client_load_data();

    // DDA MP v4.0: the client binds to its own avatar (resume an existing
    // one or create a fresh one through the native creator UI) before the
    // fullscreen view session starts.
    run_avatar_join_flow( nick );

#if defined(TILES)
    // DDA MP v3.9: load the replay tileset now, while the loading screen
    // still owns the window. The view loop would otherwise create it on its
    // first tile frame - multi seconds with the stale main-menu frame
    // sitting on the glass. create() is idempotent (shared instance).
    if( netplay::client_is_connected() ) {
        netplay::tiles_backend_create();
    }
#endif

    run_view_loop();
    netplay::client_disconnect();
}

/* DDA MP v4.0a-94 test hook: the same post-connect steps the join dialog runs,
 * without any GUI. Enabled by DDA_MP_TEST_AVATAR=<nick> together with
 * --mpclient, so automated runs can bind an avatar and exercise the client UI. */
void client_bootstrap_session( const std::string &nick )
{
    mp_fat_log( "join: bootstrap enter" );
    // The world list is normally populated by the main menu; the CLI shortcut
    // runs before it, so scan the save dirs here or the host's world cannot be
    // found. Prompts stay suppressed: a broken local world must never put a
    // modal over the join flow (the client falls back to snapshot drawing).
    if( world_generator && world_generator->get_all_worlds().empty() ) {
        mp_fat_log( "join: scanning local worlds" );
        debug_suppress_prompts( true );
        world_generator->init();
        debug_suppress_prompts( false );
    }
    mp_fat_log( "join: local world scan done" );
    client_request_snapshot();
    fat_client_load_data();
    mp_fat_log( "join: fat client data done" );
    run_avatar_join_flow( nick );
    mp_fat_log( "join: avatar flow done" );
#if defined(TILES)
    // Same as the join dialog: create the replay tileset while the loading
    // screen still owns the window, otherwise the map area stays black until
    // (and unless) the view loop gets the tileset in place.
    if( netplay::client_is_connected() ) {
        netplay::tiles_backend_create();
        mp_fat_log( "join: replay tileset requested" );
    }
#endif
}

/* ------------------------------------------------------------------ */
/* remote viewer / remote console                                      */
/* ------------------------------------------------------------------ */

/// Escaped-session dialog: offer to reconnect to the same host address.
static bool ask_reconnect( const std::string &addr )
{
    input_context ctxt( "NETPLAY_RECONNECT", keyboard_mode::keycode );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );

    while( true ) {
        ui_manager::redraw();

        const int height = 6;
        const int width = 60;
        const int x_start = std::max( 0, ( TERMX - width ) / 2 );
        const int y_start = std::max( 0, ( TERMY - height ) / 2 );

        catacurses::window win =
            catacurses::newwin( height, width, point( x_start, y_start ) );

        werase( win );
        draw_border( win, c_red, _( "Disconnected" ) );
        mvwprintw( win, point( 2, 2 ), _( "Connection to %s lost." ), addr );
        mvwprintw( win, point( 2, 3 ), _( "<ENTER>: reconnect   <ESC>: exit" ) );

        wnoutrefresh( win );

        const std::string action = ctxt.handle_input();
        if( action == "CONFIRM" ) {
            return true;
        } else if( action == "QUIT" ) {
            return false;
        }
    }
}

/// Decode a wire color: 0..15 = base_color index (bold encoded as +8, same
/// encoding cursesport uses in its cell grid); anything else is treated as a
/// legacy ncurses pair index.
static nc_color decode_cell_color( int v )
{
    static const std::array<nc_color, 16> base16 = {{
        c_black, c_red, c_green, c_brown, c_blue, c_magenta, c_cyan,
        c_light_gray, c_dark_gray, c_light_red, c_light_green, c_yellow,
        c_light_blue, c_pink, c_light_cyan, c_white
    }};
    if( v >= 0 && v < 16 ) {
        return base16[v];
    }
    return nc_color::from_color_pair_index( v );
}

static long key_to_remote( const std::string &action )
{
    // v1 maps a fixed action namespace to neutral codes.
    if( action == "LEFT" )  { return 1; }
    if( action == "RIGHT" ) { return 2; }
    if( action == "UP" )    { return 3; }
    if( action == "DOWN" )  { return 4; }
    // DDA MP v4.0a-117: ENTER is the action-menu key (ACTION_ACTIONMENU). It
    // used to be forwarded as a raw CONFIRM key, which the host swallows, so
    // the action menu could not be opened at all. Leave it to the default
    // context mapping below.
    if( action == "RIGHTUP" )   { return 12; }
    if( action == "RIGHTDOWN" ) { return 13; }
    if( action == "LEFTUP" )    { return 14; }
    if( action == "LEFTDOWN" )  { return 15; }
    return 0; // no map
}

namespace {
/* DDA MP v4.0a-8: the client owns the sidebar layout. The host only sends
 * "key=value" lines; parsing and drawing live here. */
struct mp_sheet_data {
    std::string name;
    std::string wield;
    int hp = 0;
    int hp_max = 0;
    int stam = 0;
    int stam_max = 0;
    int pain = 0;
    int sleepy = 0;
    int hunger = 0;
    int thirst = 0;
    int str = 0;
    int dex = 0;
    int intel = 0;
    int per = 0;
    int wear = 0;
    int turn = 0; // world turn for message ageing
    int speed = 0;
    int moves = 0;
    int morale = 0;
    std::string effects;
    // body part -> "cur/max"
    std::map<std::string, std::string> parts;
    bool valid = false;
};

mp_sheet_data mp_parse_sheet( const std::string &text )
{
    mp_sheet_data d;
    std::istringstream iss( text );
    std::string line;
    while( std::getline( iss, line ) ) {
        const std::size_t eq = line.find( '=' );
        if( eq == std::string::npos ) {
            continue;
        }
        const std::string k = line.substr( 0, eq );
        const std::string v = line.substr( eq + 1 );
        auto to_int = [&v]( int &dst ) {
            try {
                dst = std::stoi( v );
            } catch( ... ) {
                // keep the previous value on malformed payloads
            }
        };
        if( k == "name" ) {
            d.name = v;
        } else if( k == "wield" ) {
            d.wield = v;
        } else if( k == "hp" ) {
            to_int( d.hp );
        } else if( k == "hp_max" ) {
            to_int( d.hp_max );
        } else if( k == "stam" ) {
            to_int( d.stam );
        } else if( k == "stam_max" ) {
            to_int( d.stam_max );
        } else if( k == "pain" ) {
            to_int( d.pain );
        } else if( k == "sleepy" ) {
            to_int( d.sleepy );
        } else if( k == "hunger" ) {
            to_int( d.hunger );
        } else if( k == "thirst" ) {
            to_int( d.thirst );
        } else if( k == "str" ) {
            to_int( d.str );
        } else if( k == "dex" ) {
            to_int( d.dex );
        } else if( k == "int" ) {
            to_int( d.intel );
        } else if( k == "per" ) {
            to_int( d.per );
        } else if( k == "wear" ) {
            to_int( d.wear );
        } else if( k == "turn" ) {
            to_int( d.turn );
        } else if( k == "speed" ) {
            to_int( d.speed );
        } else if( k == "moves" ) {
            to_int( d.moves );
        } else if( k == "morale" ) {
            to_int( d.morale );
        } else if( k == "effects" ) {
            d.effects = v;
        } else if( k.rfind( "hp_", 0 ) == 0 ) {
            d.parts[k.substr( 3 )] = v;
        }
    }
    d.valid = !d.name.empty();
    return d;
}

std::string mp_bar( int v, int max, int w = 10 )
{
    if( max <= 0 ) {
        max = 1;
    }
    int fill = v <= 0 ? 0 : ( v * w ) / max;
    if( fill > w ) {
        fill = w;
    }
    std::string s = "[";
    for( int i = 0; i < w; ++i ) {
        s += i < fill ? '#' : '-';
    }
    s += ']';
    return s;
}

/* DDA MP v4.0a-10: the sidebar as plain text lines (client-owned layout). */
std::vector<std::string> mp_sheet_lines( const mp_sheet_data &d )
{
    std::vector<std::string> out;
    out.push_back( d.name );
    static const char *const part_ids[] = { "head", "torso", "arm_l", "arm_r", "leg_l", "leg_r" };
    static const char *const part_labels[] = {
        "Head", "Torso", "Left Arm", "Right Arm", "Left Leg", "Right Leg"
    };
    for( int i = 0; i < 6; i++ ) {
        const auto it = d.parts.find( part_ids[i] );
        out.push_back( string_format( "%-11s %s", _( part_labels[i] ),
                                      it == d.parts.end() ? "-" : it->second ) );
    }
    out.emplace_back();
    out.push_back( string_format( "%s %s %d/%d", _( "HP" ), mp_bar( d.hp, d.hp_max ), d.hp, d.hp_max ) );
    out.push_back( string_format( "%s %s %d/%d", _( "Stamina" ), mp_bar( d.stam, d.stam_max ),
                                  d.stam, d.stam_max ) );
    out.emplace_back();
    out.push_back( string_format( "%s %d", _( "Pain" ), d.pain ) );
    out.push_back( string_format( "%s %d", _( "Sleepiness" ), d.sleepy ) );
    out.push_back( string_format( "%s %d", _( "Hunger" ), d.hunger ) );
    out.push_back( string_format( "%s %d", _( "Thirst" ), d.thirst ) );
    out.push_back( string_format( "%s %d", _( "Morale" ), d.morale ) );
    out.emplace_back();
    out.push_back( string_format( "%s %d", _( "Strength" ), d.str ) );
    out.push_back( string_format( "%s %d", _( "Dexterity" ), d.dex ) );
    out.push_back( string_format( "%s %d", _( "Intelligence" ), d.intel ) );
    out.push_back( string_format( "%s %d", netplay::fork_text( "Perception" ), d.per ) );
    out.push_back( string_format( "%s %d", _( "Speed" ), d.speed ) );
    out.push_back( string_format( "%s %d", _( "Moves" ), d.moves ) );
    out.emplace_back();
    out.push_back( _( "Weapon" ) );
    out.push_back( d.wield.empty() ? "-" : d.wield );
    out.push_back( string_format( "%s %d", _( "Worn" ), d.wear ) );
    if( !d.effects.empty() ) {
        out.emplace_back();
        std::istringstream eff( d.effects );
        std::string e;
        while( std::getline( eff, e, ';' ) ) {
            out.push_back( e );
        }
    }
    return out;
}
} // namespace

/* DDA MP v4.0a-16: overlay inventory, laid out like the host window. Data
 * comes from parse_inv_entries() (identical fields the host adapter produced),
 * so the client draws the same rows without owning any live items. */
static std::string mp_clip_text( const std::string &s, float max_w )
{
    if( max_w <= 0.0f ) {
        return std::string();
    }
    if( ImGui::CalcTextSize( s.c_str() ).x <= max_w ) {
        return s;
    }
    std::string out = s;
    while( !out.empty() && ImGui::CalcTextSize( ( out + "..." ).c_str() ).x > max_w ) {
        out.pop_back();
        while( !out.empty() && ( static_cast<unsigned char>( out.back() ) & 0xC0 ) == 0x80 ) {
            out.pop_back();
        }
    }
    return out + "...";
}

static std::string mp_fmt_kg( long g )
{
    char b[48];
    std::snprintf( b, sizeof( b ), "%.2f kg", static_cast<double>( g ) / 1000.0 );
    return b;
}

static std::string mp_fmt_l( long ml )
{
    char b[48];
    std::snprintf( b, sizeof( b ), "%.2f L", static_cast<double>( ml ) / 1000.0 );
    return b;
}

static std::string mp_fmt_cm( int mm )
{
    char b[48];
    std::snprintf( b, sizeof( b ), "%.1f cm", static_cast<double>( mm ) / 10.0 );
    return b;
}

/// Rows belonging to the left (worn/wielded) column, refreshed every draw so
/// the input handler can jump between columns.
static int mp_inv_split = 0;

/* DDA MP v4.0a-35: throw targeting. The client draws its own crosshair over
 * the streamed canvas and sends the tile offset; the host performs the throw
 * with Character::throw_item so range/blocking rules stay authoritative. */
static bool mp_throw_active = false;
static std::string mp_throw_uid;
static int mp_throw_dx = 1;
static int mp_throw_dy = 0;
/* DDA MP v4.0a-150: netplay-native look/fire cursor. The stock look_around()
 * and target UI block on a game window the view loop does not present, so the
 * client only saw a frozen screen. These modes draw the reticle and tile info
 * straight onto the streamed canvas and send the final offset to the host. */
enum class mp_cursor_mode : int {
    none = 0,
    look,
    fire,
    spell
};
static mp_cursor_mode mp_cursor = mp_cursor_mode::none;
static std::string mp_pending_spell;
static std::string mp_last_spell;
static int mp_last_spell_dx = 0;
static int mp_last_spell_dy = 0;
static int mp_cursor_dx = 0;
static int mp_cursor_dy = 0;
static bool mp_look_fast_scroll = false;

/* Tile under the look/fire/throw reticle. The snapshot origin is the pivot
 * cell (frame centre), not the frame's top-left. */
static bool mp_cursor_target( const snapshot &snap, tripoint_abs_ms &ap,
                              tripoint_bub_ms &lp )
{
    if( mp_cursor == mp_cursor_mode::none && !mp_throw_active ) {
        return false;
    }
    const int rdx = mp_throw_active ? mp_throw_dx : mp_cursor_dx;
    const int rdy = mp_throw_active ? mp_throw_dy : mp_cursor_dy;
    const int tx = snap.width / 2 + rdx;
    const int ty = snap.height / 2 + rdy;
    if( tx < 0 || ty < 0 || tx >= snap.width || ty >= snap.height ) {
        return false;
    }
    ap = tripoint_abs_ms( snap.origin_x + tx - snap.width / 2,
                          snap.origin_y + ty - snap.height / 2, snap.origin_z );
    map &here = get_map();
    lp = here.get_bub( ap );
    return here.inbounds( lp );
}

static void mp_inv_log( const std::string &msg );

static void mp_begin_client_fire()
{
    const item_location weapon = get_avatar().get_wielded_item();
    if( !weapon || !weapon->is_gun() ) {
        mp_inv_log( "fire: no gun wielded" );
        return;
    }
    mp_cursor = mp_cursor_mode::fire;
    mp_cursor_dx = 0;
    mp_cursor_dy = 0;
}
static float mp_canvas_cx = 0.0f;
static float mp_canvas_cy = 0.0f;
static float mp_canvas_tw = 0.0f;
static float mp_canvas_th = 0.0f;
/* DDA MP v4.0a-41: aiming view-model. The client has no game map, so the
 * snapshot supplies the only facts the stock target_ui needs for a throw:
 * terrain blocking (smooth-light barrier bit) and creature ids. */
static std::vector<uint8_t> mp_aim_flags;
static std::vector<std::string> mp_aim_critters;
static std::vector<int> mp_aim_light;
static std::vector<uint32_t> mp_aim_glyphs;
static std::vector<uint8_t> mp_aim_badges;
static int mp_aim_w = 0;
static int mp_aim_h = 0;
static std::unique_ptr<npc> mp_aim_char;
static item mp_aim_item;
static std::vector<std::string> mp_client_aim_lines;
static std::string mp_char_json_cache;
static netplay::minimap_frame mp_aim_minimap;
static std::vector<netplay::aim_target> mp_aim_targets;
static std::string mp_throw_name;
static int mp_throw_range = 1;
static std::string mp_aim_text;
/* World origin of the last snapshot (absolute coords of the session avatar). */
static int mp_origin_x = 0;
static int mp_origin_y = 0;
static int mp_origin_z = 0;
static bool mp_have_origin = false;

/// DDA MP v4.0a-179: the inventory packet carries the host's localized
/// category header plus the stable category id. Resolve the id against the
/// client's own data so a Russian client reading an English host still gets
/// Russian category headers.
static std::string mp_local_category( const netplay::inv_entry_view &e )
{
    if( e.category_id.empty() ) {
        return e.category;
    }
    for( const item_category &cat : item_category::get_all() ) {
        if( cat.get_id().str() == e.category_id ) {
            return cat.name_header();
        }
    }
    return e.category;
}

static void mp_overlay_inventory( const std::string &text, bool open, int &sel, int &scroll )
{
    if( !imclient || !open ) {
        return;
    }
    const netplay::inv_packet pkt = netplay::parse_inv_packet( text );
    const std::vector<netplay::inv_entry_view> &items = pkt.entries;
    ( void )scroll; // scrolling now follows the selection
    netplay::overlay_begin();
    const ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float fs = ImGui::GetFontSize();
    const float lh = ImGui::GetTextLineHeight();
    const float pad = fs * 0.8f;
    const float iw = std::min( io.DisplaySize.x - pad * 2.0f, 1100.0f );
    const float ih = std::min( io.DisplaySize.y - pad * 2.0f, 700.0f );
    const float ix = ( io.DisplaySize.x - iw ) / 2.0f;
    const float iy = ( io.DisplaySize.y - ih ) / 2.0f;
    const float hdr_h = lh * 2.9f;
    const float foot_h = lh * 1.9f;

    // Frame and header band.
    dl->AddRectFilled( ImVec2( ix, iy ), ImVec2( ix + iw, iy + ih ), IM_COL32( 8, 10, 10, 255 ) );
    dl->AddRectFilled( ImVec2( ix, iy ), ImVec2( ix + iw, iy + hdr_h ), IM_COL32( 24, 28, 28, 255 ) );
    dl->AddRect( ImVec2( ix, iy ), ImVec2( ix + iw, iy + ih ), IM_COL32( 170, 180, 175, 255 ), 0.0f, 0, 2.0f );
    dl->AddLine( ImVec2( ix, iy + hdr_h ), ImVec2( ix + iw, iy + hdr_h ),
                 IM_COL32( 110, 120, 115, 255 ) );

    const ImU32 col_title = IM_COL32( 240, 240, 240, 255 );
    const ImU32 col_dim = IM_COL32( 145, 150, 145, 255 );
    const ImU32 col_stat = IM_COL32( 180, 205, 185, 255 );
    const ImU32 col_section = IM_COL32( 160, 190, 205, 255 );
    const ImU32 col_qty = IM_COL32( 165, 190, 170, 255 );
    const ImU32 col_sel = IM_COL32( 42, 74, 62, 255 );
    const ImU32 col_detail = IM_COL32( 160, 175, 160, 255 );

    // Header line 1: title, hint underneath, stats right-aligned on two rows
    // (same information the host prints above its columns).
    const float h1 = iy + pad * 0.35f;
    dl->AddText( ImVec2( ix + pad, h1 ), col_title, _( "Inventory" ) );
    {
        const std::string hint = ( pkt.header.hotkeys_total > 0 ) ?
                                 string_format( netplay::fork_text( "Item hotkeys assigned: %d/%d" ),
                                                pkt.header.hotkeys_assigned, pkt.header.hotkeys_total ) :
                                 string_format( netplay::fork_text( "Items: %d" ), static_cast<int>( items.size() ) );
        dl->AddText( ImVec2( ix + pad, h1 + lh ), col_dim, hint.c_str() );
    }
    {
        const std::string s1 = string_format( "%s %s/%s", _( "Total Volume:" ),
                                              mp_fmt_l( pkt.header.volume_ml ).c_str(),
                                              mp_fmt_l( pkt.header.volume_cap_ml ).c_str() );
        const std::string s2 = string_format( "%s %s/%s   %s %s %s", _( "Total Weight:" ),
                                              mp_fmt_kg( pkt.header.weight_g ).c_str(),
                                              mp_fmt_kg( pkt.header.weight_cap_g ).c_str(),
                                              _( "Largest Space" ),
                                              mp_fmt_l( pkt.header.large_free_ml ).c_str(),
                                              mp_fmt_cm( pkt.header.large_free_len_mm ).c_str() );
        const float w1 = ImGui::CalcTextSize( s1.c_str() ).x;
        const float w2 = ImGui::CalcTextSize( s2.c_str() ).x;
        dl->AddText( ImVec2( ix + iw - pad - w1, h1 ), col_stat, s1.c_str() );
        dl->AddText( ImVec2( ix + iw - pad - w2, h1 + lh ), col_stat, s2.c_str() );
    }

    // Two host-like columns: wielded/worn gear on the left, carried on the right.
    // Top-level entries are grouped by the host's category ordering, while a
    // container keeps its nested contents directly underneath (depth > 0).
    auto build_rows = [&]( bool gear ) {
        std::vector<std::vector<const netplay::inv_entry_view *>> blocks;
        for( std::size_t i = 0; i < items.size(); i++ ) {
            const netplay::inv_entry_view &e = items[i];
            if( e.depth != 0 || ( e.wielded || e.worn ) != gear ) {
                continue;
            }
            std::vector<const netplay::inv_entry_view *> blk;
            blk.push_back( &e );
            for( std::size_t j = i + 1; j < items.size() && items[j].depth > 0; j++ ) {
                blk.push_back( &items[j] );
            }
            blocks.push_back( std::move( blk ) );
        }
        std::sort( blocks.begin(), blocks.end(),
        []( const std::vector<const netplay::inv_entry_view *> &a,
        const std::vector<const netplay::inv_entry_view *> &b ) {
            const netplay::inv_entry_view *x = a.front();
            const netplay::inv_entry_view *y = b.front();
            if( x->category_rank != y->category_rank ) {
                return x->category_rank < y->category_rank;
            }
            if( x->category != y->category ) {
                return x->category < y->category;
            }
            return x->name < y->name;
        } );
        std::vector<const netplay::inv_entry_view *> out;
        for( const auto &blk : blocks ) {
            out.insert( out.end(), blk.begin(), blk.end() );
        }
        return out;
    };
    std::vector<const netplay::inv_entry_view *> worn_rows = build_rows( true );
    std::vector<const netplay::inv_entry_view *> carried_rows = build_rows( false );
    std::vector<const netplay::inv_entry_view *> all_rows = worn_rows;
    all_rows.insert( all_rows.end(), carried_rows.begin(), carried_rows.end() );
    const int total = static_cast<int>( all_rows.size() );
    sel = total <= 0 ? 0 : std::min( std::max( sel, 0 ), total - 1 );

    const float body_top = iy + hdr_h + pad * 0.45f;
    const float body_bot = iy + ih - foot_h;
    const float gap = pad;
    const float col_w = ( iw - pad * 2.0f - gap ) / 2.0f;
    const float lx = ix + pad;
    const float rx = lx + col_w + gap;
    dl->AddLine( ImVec2( rx - gap * 0.5f, body_top ), ImVec2( rx - gap * 0.5f, body_bot ),
                 IM_COL32( 70, 80, 75, 255 ) );

    const int worn_n = static_cast<int>( worn_rows.size() );
    mp_inv_split = worn_n;
    const netplay::inv_entry_view *chosen = sel < total ? all_rows[sel] : nullptr;

    // DDA MP v4.0a-179: the host sends display names in ITS language. The
    // client's avatar mirror holds a full copy of the same items (tagged with
    // the stable mp_uid), so render the mirror's name instead. That keeps
    // damage/ammo/mod details intact but lets the client's own catalog
    // translate them. Items without a mirror (pickup rows) keep the host name.
    std::unordered_map<std::string, std::string> mp_mirror_names;
    for( item *it : get_avatar().inv_dump() ) {
        if( it == nullptr ) {
            continue;
        }
        const std::string uid = it->get_var( "mp_uid", std::string() );
        if( !uid.empty() ) {
            mp_mirror_names[uid] = netplay::inv_strip_tags(
                                       it->display_name( it->count(), false ) );
        }
    }
    const auto mp_local_item_name = [&]( const netplay::inv_entry_view & e ) -> std::string {
        const auto it = mp_mirror_names.find( e.uid );
        return it != mp_mirror_names.end() ? it->second : e.name;
    };

    auto draw_column = [&]( float x, const std::vector<const netplay::inv_entry_view *> &rows, int base ) {
        float y = body_top;
        const bool active = sel >= base && sel < base + static_cast<int>( rows.size() );
        const int idx = active ? sel - base : 0;
        // Keep the selection visible; leave room for the expanded details.
        const int cap = std::max( 1, static_cast<int>( ( body_bot - y ) / lh ) - 3 );
        const int first = active ? std::max( 0, idx - cap + 1 ) : 0;
        if( first > 0 ) {
            dl->AddText( ImVec2( x, y ), col_dim, string_format( "... %d", first ).c_str() );
            y += lh;
        }
        std::string last_cat;
        for( int i = first; i < static_cast<int>( rows.size() ); i++ ) {
            const netplay::inv_entry_view *e = rows[i];
            // Category header row, like the host's "Инструменты"/"Носимые Вещи".
            if( e->depth == 0 && e->category != last_cat && !e->category.empty() ) {
                if( y + lh * 1.35f > body_bot ) {
                    return;
                }
                if( !last_cat.empty() ) {
                    y += lh * 0.25f;
                }
                dl->AddRectFilled( ImVec2( x - fs * 0.35f, y - lh * 0.1f ),
                                   ImVec2( x + col_w, y + lh * 1.0f ), IM_COL32( 30, 40, 48, 255 ) );
                dl->AddText( ImVec2( x, y ), col_section, mp_local_category( *e ).c_str() );
                last_cat = e->category;
                y += lh * 1.15f;
            }
            if( y + lh > body_bot ) {
                dl->AddText( ImVec2( x, y ), col_dim,
                             string_format( "... +%d", static_cast<int>( rows.size() ) - i ).c_str() );
                return;
            }
            const bool is_sel = base + i == sel;
            const ImU32 col = cataimgui::ImU32_from_color( decode_cell_color( e->color_index ) );
            if( is_sel ) {
                dl->AddRectFilled( ImVec2( x - fs * 0.35f, y - lh * 0.12f ),
                                   ImVec2( x + col_w, y + lh * 1.05f ), col_sel );
            }
            // Inventory letter column (the host prints it at the row start).
            if( e->depth == 0 && e->invlet != '\0' ) {
                const char ib[2] = { e->invlet, '\0' };
                dl->AddText( ImVec2( x, y ), IM_COL32( 225, 215, 150, 255 ), ib );
            }
            std::string qty;
            if( e->count > 1 ) {
                qty = string_format( "%dx", e->count );
            }
            const std::string wstr = mp_fmt_kg( e->weight_g );
            qty += ( qty.empty() ? "" : "  " ) + wstr;
            const float qw = ImGui::CalcTextSize( qty.c_str() ).x;
            const float indent = static_cast<float>( e->depth ) * fs * 1.1f;
            const float name_x = x + fs * 1.15f + indent;
            const float name_w = col_w - ( name_x - x ) - qw - fs * 0.9f;
            const std::string nm = mp_clip_text(
                                       ( e->depth > 0 ? "> " : "" ) + mp_local_item_name( *e ), name_w );
            dl->AddText( ImVec2( name_x, y ), col, nm.c_str() );
            dl->AddText( ImVec2( x + col_w - qw, y ), col_qty, qty.c_str() );
            y += lh;
            if( is_sel ) {
                // Inline details, like the host expands the highlighted entry.
                std::string d1 = string_format( "%s %s   %s %s", _( "Weight" ),
                                                mp_fmt_kg( e->weight_g ).c_str(), netplay::fork_text( "Volume" ),
                                                mp_fmt_l( e->volume_ml ).c_str() );
                if( e->side_mm > 0 ) {
                    d1 += "   " + string_format( "%s %s", netplay::fork_text( "Longest side" ),
                                                 mp_fmt_cm( e->side_mm ).c_str() );
                }
                std::string d2 = mp_local_category( *e );
                if( e->charges != 0 ) {
                    d2 += ( d2.empty() ? "" : "   " ) + string_format( "%s %d", _( "Charges" ), e->charges );
                }
                if( e->container ) {
                    d2 += ( d2.empty() ? "" : "   " ) + std::string( _( "Container" ) );
                }
                if( y + lh * 1.2f < body_bot ) {
                    dl->AddText( ImVec2( x + fs * 0.6f, y ), col_detail,
                                 mp_clip_text( d1, col_w - fs * 0.6f ).c_str() );
                    y += lh;
                }
                if( !d2.empty() && y + lh * 1.2f < body_bot ) {
                    dl->AddText( ImVec2( x + fs * 0.6f, y ), col_detail,
                                 mp_clip_text( d2, col_w - fs * 0.6f ).c_str() );
                    y += lh;
                }
            }
        }
    };

    draw_column( lx, worn_rows, 0 );
    draw_column( rx, carried_rows, worn_n );
    if( items.empty() ) {
        dl->AddText( ImVec2( lx, body_top + lh * 1.6f ), col_dim, _( "(empty)" ) );
    }

    // Footer hints.
    const float fy = iy + ih - foot_h + lh * 0.35f;
    dl->AddLine( ImVec2( ix + pad, fy - lh * 0.3f ), ImVec2( ix + iw - pad, fy - lh * 0.3f ),
                 IM_COL32( 70, 80, 75, 255 ) );
    dl->AddText( ImVec2( ix + pad, fy ), col_dim,
                 netplay::fork_text( "[Esc] close   [Up/Down] select   [Tab] column   [PgUp/PgDn] page" ).c_str() );
    if( chosen != nullptr ) {
        const std::string right = string_format( "%s: %s", _( "Category" ),
                                                 mp_local_category( *chosen ).c_str() );
        const float rw = ImGui::CalcTextSize( right.c_str() ).x;
        dl->AddText( ImVec2( ix + iw - pad - rw, fy ), col_dim, right.c_str() );
    }

    netplay::overlay_end();
}

/* DDA MP: debug switch to compare renderers. DDA_MP_LEGACY_INV=1 keeps the
 * old ImGui approximation instead of the game's real inventory UI. */
static bool mp_legacy_inventory_enabled()
{
    static const bool enabled = []() {
        const char *env = std::getenv( "DDA_MP_LEGACY_INV" );
        return env != nullptr && env[0] != '\0' && std::string( env ) != "0";
    }();
    return enabled;
}

/* DDA MP v4.0a-78: debug switch for default-mode key parity on the client.
 * DDA_MP_KEY_ACTIONS=0 disables it, leaving only the built-in keys. */
static bool mp_key_action_parity()
{
    static const bool enabled = []() {
        const char *env = std::getenv( "DDA_MP_KEY_ACTIONS" );
        return env == nullptr || env[0] == '\0' || std::string( env ) != "0";
    }();
    return enabled;
}

/* DDA MP v4.0a-17: diagnostics for the client inventory bridge. Always
 * compiled, gated by DDA_MP_DIAG at runtime. */
static void mp_inv_log( const std::string &msg )
{
    if( !mp_netplay_diag ) {
        return;
    }
    try {
        std::ofstream f( netplay::mp_diag_dir() + "\\netplay_inventory.log", std::ios::app );
        if( f ) {
            f << msg << "\n";
        }
    } catch( ... ) {
    }
}

static void mp_run_client_throw( const item &thrown, const std::string &uid,
                                 const tripoint_abs_ms &origin );

/* DDA MP v4.0a-18: the item action menu, same entries/hotkeys/texts as the
 * host's inventory_item_menu. Ratings mirror the host's rate_action_* helpers
 * (game.cpp) so invalid choices are grey/red exactly like in the original UI. */
static hint_rating mp_rate_use( Character &you, const item &it )
{
    if( it.is_container() && it.item_has_uses_recursive() ) {
        return hint_rating::good;
    }
    if( it.is_broken() ) {
        return hint_rating::iffy;
    } else if( it.is_tool() ) {
        return it.ammo_sufficient( &you ) ? hint_rating::good : hint_rating::iffy;
    } else if( it.is_gunmod() ) {
        return static_cast<int>( you.get_skill_level( skill_id( "gun" ) ) ) == 0 ?
               hint_rating::iffy : hint_rating::good;
    } else if( it.is_food() || it.is_medication() || it.is_book() || it.is_armor() ) {
        if( it.is_medication() && !you.can_use_heal_item( it ) ) {
            return hint_rating::cant;
        }
        if( it.is_comestible() && it.is_frozen_liquid() ) {
            return hint_rating::cant;
        }
        return hint_rating::iffy;
    } else if( it.type->has_use() ) {
        return hint_rating::good;
    }
    return hint_rating::cant;
}

static hint_rating mp_rate_read( Character &you, const item &it )
{
    if( !it.is_book() ) {
        return hint_rating::cant;
    }
    std::vector<std::string> dummy;
    return you.get_book_reader( it, dummy ) ? hint_rating::good : hint_rating::iffy;
}

static hint_rating mp_rate_eat( Character &you, const item &it )
{
    if( it.is_container() ) {
        hint_rating best = hint_rating::cant;
        it.visit_items( [&you, &best]( item *node, item * ) {
            if( you.can_consume_as_is( *node ) ) {
                const ret_val<edible_rating> rate = you.will_eat( *node );
                if( rate.success() ) {
                    best = hint_rating::good;
                    return VisitResponse::ABORT;
                } else if( rate.value() != INEDIBLE && rate.value() != INEDIBLE_MUTATION ) {
                    best = hint_rating::iffy;
                }
            }
            return VisitResponse::NEXT;
        } );
        return best;
    }
    if( !you.can_consume_as_is( it ) ) {
        return hint_rating::cant;
    }
    const ret_val<edible_rating> rating = you.will_eat( it );
    if( rating.success() ) {
        return hint_rating::good;
    }
    if( rating.value() == INEDIBLE || rating.value() == INEDIBLE_MUTATION ) {
        return hint_rating::cant;
    }
    return hint_rating::iffy;
}

static hint_rating mp_rate_wear( Character &you, const item &it, bool worn )
{
    if( !it.is_armor() ) {
        return hint_rating::cant;
    }
    if( worn ) {
        return hint_rating::iffy;
    }
    return you.can_wear( it ).success() ? hint_rating::good : hint_rating::iffy;
}

static hint_rating mp_rate_take_off( const item &it, bool worn )
{
    if( !it.is_armor() || it.has_flag( flag_NO_TAKEOFF ) ||
        it.has_flag( flag_INTEGRATED ) ) {
        return hint_rating::cant;
    }
    return worn ? hint_rating::good : hint_rating::iffy;
}

static void mp_menu_entry( uilist &menu, char key, const std::string &text,
                           hint_rating rating )
{
    menu.addentry( key, true, key, text );
    menu.entries.back().text_color = rating == hint_rating::good ? c_light_green :
                                     ( rating == hint_rating::iffy ? c_light_red : c_light_gray );
}

static int mp_client_item_menu( Character &you, const item &it )
{
    uilist menu;
    menu.text = it.display_name();
    const bool worn = you.is_worn( it );
    const bool can_drop = you.can_drop( it ).success();
    mp_menu_entry( menu, 'a', pgettext( "action", "activate" ), mp_rate_use( you, it ) );
    mp_menu_entry( menu, 'R', pgettext( "action", "read" ), mp_rate_read( you, it ) );
    mp_menu_entry( menu, 'E', pgettext( "action", "eat" ), mp_rate_eat( you, it ) );
    mp_menu_entry( menu, 'W', pgettext( "action", "wear" ), mp_rate_wear( you, it, worn ) );
    mp_menu_entry( menu, 'w', pgettext( "action", "wield" ),
                   you.can_wield( it ).success() ? hint_rating::good : hint_rating::iffy );
    mp_menu_entry( menu, 't', pgettext( "action", "throw" ),
                   you.can_wield( it ).success() ? hint_rating::good : hint_rating::iffy );
    mp_menu_entry( menu, 'c', pgettext( "action", "change side" ),
                   it.is_armor() ? hint_rating::good : hint_rating::cant );
    mp_menu_entry( menu, 'T', pgettext( "action", "take off" ), mp_rate_take_off( it, worn ) );
    mp_menu_entry( menu, 'd', pgettext( "action", "drop" ),
                   can_drop ? hint_rating::good : hint_rating::cant );
    // DDA MP v4.0a-148: partner interaction - hand the item to a player
    // standing next to the avatar (the host resolves the nearest one).
    mp_menu_entry( menu, 'g', pgettext( "action", "give" ), hint_rating::good );
    mp_menu_entry( menu, 'U', pgettext( "action", "unload" ),
                   you.rate_action_unload( it ) == hint_rating::cant ?
                   hint_rating::cant : hint_rating::good );
    mp_menu_entry( menu, 'r', pgettext( "action", "reload" ),
                   you.rate_action_reload( it ) == hint_rating::cant ?
                   hint_rating::cant : hint_rating::good );
    mp_menu_entry( menu, 'p', pgettext( "action", "part reload" ),
                   you.rate_action_reload( it ) == hint_rating::cant ?
                   hint_rating::cant : hint_rating::good );
    mp_menu_entry( menu, 'm', pgettext( "action", "mend" ),
                   !it.faults.empty() ? hint_rating::good :
                   ( it.faults_potential().empty() ? hint_rating::cant : hint_rating::iffy ) );
    mp_menu_entry( menu, 'D', pgettext( "action", "disassemble" ), hint_rating::cant );
    if( it.is_container() && !it.is_corpse() ) {
        mp_menu_entry( menu, 'i', pgettext( "action", "insert" ), hint_rating::cant );
        if( it.num_item_stacks() > 0 ) {
            mp_menu_entry( menu, 'o', pgettext( "action", "open" ), hint_rating::good );
        }
        mp_menu_entry( menu, 'v', pgettext( "action", "pocket settings" ), hint_rating::good );
    }
    mp_menu_entry( menu, 'f', it.is_favorite ? pgettext( "action", "unfavorite" ) :
                   pgettext( "action", "favorite" ), hint_rating::good );
    mp_menu_entry( menu, 'V', pgettext( "action", "view recipe" ), hint_rating::good );
    {
        bool can_collapse = false;
        bool can_expand = false;
        for( const item_pocket *pocket : it.get_standard_pockets() ) {
            if( pocket->settings.is_collapsed() ) {
                can_expand = true;
            } else {
                can_collapse = true;
            }
        }
        mp_menu_entry( menu, '>', pgettext( "action", "hide contents" ),
                       can_collapse ? hint_rating::good : hint_rating::cant );
        mp_menu_entry( menu, '<', pgettext( "action", "show contents" ),
                       can_expand ? hint_rating::good : hint_rating::cant );
    }
    mp_menu_entry( menu, '=', pgettext( "action", "reassign" ), hint_rating::good );
    // Host's inventory_item_menu shows the full item info window (x=0, width
    // 50, full height) and places the action list right of it.
    std::vector<iteminfo> vThisItem;
    std::vector<iteminfo> vDummy;
    it.info( true, vThisItem );
    item_info_data data( it.tname(), it.type_name(), vThisItem, vDummy );
    data.without_getch = true;
    catacurses::window w_info = catacurses::newwin( TERMY, 50, point( 0, 0 ) );
    ui_adaptor info_ui;
    if( w_info ) {
        info_ui.position_from_window( w_info );
        info_ui.on_redraw( [&]( const ui_adaptor & ) {
            draw_item_info( w_info, data );
        } );
        info_ui.mark_resize();
    }
    const float mp_cw = ImGui::CalcTextSize( "X" ).x;
    menu.desired_bounds = { 50.0f * mp_cw, 0.0f, 30.0f * mp_cw, -1.0f };
    menu.reposition();
    menu.query();
    return menu.ret;
}

/* DDA MP v4.1: the client's character mirror. The host serializes the session
 * avatar (raw state: stats, skills, effects, every item), the client
 * deserializes it once per revision and runs the game's own menus on it. The
 * streamed item packets stay only as a fallback and as the row-id map. */
static npc *mp_mirror()
{
    static std::unique_ptr<npc> npc_mirror;
    static std::string src;
    std::string json;
    if( !netplay::client_character_json( json ) || json.empty() ) {
        return npc_mirror.get();
    }
    if( npc_mirror && json == src ) {
        return npc_mirror.get();
    }
    try {
        JsonValue jv = json_loader::from_string( json );
        auto next = std::make_unique<npc>();
        next->deserialize( jv.get_object() );
        npc_mirror = std::move( next );
        src = json;
        int tagged = 0;
        const std::vector<item *> dump = npc_mirror->inv_dump();
        for( const item *it : dump ) {
            if( it != nullptr && !it->get_var( "mp_uid", std::string() ).empty() ) {
                ++tagged;
            }
        }
        mp_inv_log( string_format( "mirror: rebuilt %d bytes, items=%d tagged=%d",
                                   static_cast<int>( json.size() ),
                                   static_cast<int>( dump.size() ), tagged ) );
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "mirror error: " ) + e.what() );
    } catch( ... ) {
        mp_inv_log( "mirror error: unknown" );
    }
    return npc_mirror.get();
}

/* DDA MP v4.0a-27: the client's own @ menu. Running it on the mirror gives
 * Character::disp_info() the real stats/traits/skills and live item list. */
static void mp_run_character_info( const std::string &text )
{
    static const bool mp_info_disabled = []() {
        const char *env = std::getenv( "DDA_MP_NO_INFO_JSON" );
        return env != nullptr && env[0] != '\0' && std::string( env ) != "0";
    }();
    if( mp_info_disabled ) {
        return;
    }
    ( void )text;
    try {
        static const bool mp_look_on = []() {
        const char *v = std::getenv( "DDA_MP_NATIVE_LOOK" );
        return !( v != nullptr && v[0] != '\0' && std::string( v ) == "0" );
    }();
    if( !mp_look_on ) {
        return;
    }
        if( !mp_native_avatar_ready ) {
            return;
        }
        mp_inv_log( "info: opening @ menu on the avatar mirror" );
        // Same layering rule as the inventory: hide the pre-session main menu,
        // keep the @ menu and its own submenus above the blocker.
        ui_adaptor mp_info_blocker( ui_adaptor::disable_uis_below{} );
        get_avatar().disp_info( false );
        mp_inv_log( "info: closed" );
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "info error: " ) + e.what() );
    } catch( ... ) {
        mp_inv_log( "info error: unknown" );
    }
}

/* DDA MP v4.0a-90: the client's own pickup selector. The host streams the ground
 * items around the session avatar (with their tile offsets and whether that
 * avatar can carry them). The client places the real items on a scratch tile of
 * its local map and runs the game's own inventory_multiselector, so the list,
 * categories, counts and denial reasons are the original UI. Only the chosen
 * row indices travel back; the host applies them with the (now character-based)
 * pickup activity. */
namespace
{
class mp_pickup_preset : public inventory_selector_preset
{
    public:
        std::string get_denial( const item_location &loc ) const override {
            if( loc && loc->get_var( "mp_can_take", std::string( "1" ) ) == "0" ) {
                return _( "You can't carry that." );
            }
            return std::string();
        }
};

/* DDA MP v4.0a-105: the pickup rows act on the client's own keybindings, the
 * same way the host's pickup does: ENTER takes the row, the "Wield" action key
 * puts it in hand and the "Wear" action key puts it on. The keys come from the
 * game's keybinding table, so a rebind on the client is honoured. Only the row
 * indices travel to the host, which applies them with its own pickup code. */
class mp_pickup_selector : public inventory_multiselector
{
    public:
        explicit mp_pickup_selector( Character &u, const inventory_selector_preset &preset ) :
            inventory_multiselector( u, preset, _( "ITEMS TO PICK UP" ), {},
                                     /*allow_select_contained=*/true ) {}

        /// 0 = take, 1 = wield, 2 = wear; meaningful when nodes is non-empty.
        int equip = 0;
        /// Row indices the host should act on; empty when the menu was closed.
        std::vector<int> nodes;

        /// Rows marked with SPACE, falling back to the highlighted row.
        std::vector<int> chosen_nodes() const {
            std::vector<int> out;
            for( const std::pair<item_location, int> &drop : to_use ) {
                const std::string nv = drop.first->get_var( "mp_node", std::string() );
                if( !nv.empty() ) {
                    out.push_back( std::atoi( nv.c_str() ) );
                }
            }
            if( out.empty() ) {
                const inventory_entry &hl = get_highlighted();
                if( hl.is_item() ) {
                    const std::string nv = hl.any_item()->get_var( "mp_node", std::string() );
                    if( !nv.empty() ) {
                        out.push_back( std::atoi( nv.c_str() ) );
                    }
                }
            }
            return out;
        }
};

/// First key bound to a default-mode action, so the client's rebinds apply.
int mp_action_hotkey( action_id act, int fallback )
{
    const std::optional<input_event> ev = hotkey_for_action( act, 1, false );
    return ev ? ev->get_first_input() : fallback;
}
} // namespace

static void mp_run_pickup( const std::string &text )
{
    try {
        const netplay::inv_packet pkt = netplay::parse_inv_packet( text );
        if( pkt.entries.empty() ) {
            return;
        }
        // DDA MP v4.0a-93: the client's local map is a replay scratch space and
        // is not safe to spawn items on (anchoring it logs unloaded-submap
        // errors and can crash). Keep the display copies in a synthetic wielded
        // container on a temporary NPC instead: force insertion bypasses
        // capacity, so every streamed row is listed regardless of its volume.
        npc viewer;
        viewer.name = "client";
        if( !item::type_is_defined( itype_id( "backpack" ) ) ) {
            mp_inv_log( "pickup: backpack itype missing" );
            return;
        }
        item bag( itype_id( "backpack" ), calendar::turn );
        bag.set_var( "mp_can_take", "0" ); // the wrapper itself is not a row
        viewer.set_wielded_item( bag );
        item *bag_item = nullptr;
        if( item_location w = viewer.get_wielded_item() ) {
            bag_item = w.get_item();
        }
        if( bag_item == nullptr ) {
            return;
        }
        // Contents are hidden by default for collapsed pockets; show them.
        for( item_pocket *pocket : bag_item->get_standard_pockets() ) {
            pocket->settings.set_collapse( false );
        }
        int added = 0;
        for( size_t i = 0; i < pkt.entries.size(); i++ ) {
            const netplay::inv_entry_view &e = pkt.entries[i];
            if( !item::type_is_defined( itype_id( e.id ) ) ) {
                continue;
            }
            item it( itype_id( e.id ), calendar::turn );
            it.set_var( "mp_node", std::to_string( i ) );
            it.set_var( "mp_can_take", e.can_take ? "1" : "0" );
            if( e.charges > 0 &&
                ( it.count_by_charges() || it.is_magazine() || it.is_ammo() || it.is_tool() ) ) {
                it.charges = e.charges;
            }
            it.is_favorite = e.favorite;
            if( e.damage > 0 ) {
                it.set_damage( e.damage );
            }
            if( e.filthy ) {
                it.set_flag( flag_FILTHY );
            }
            bag_item->force_insert_item( it, pocket_type::CONTAINER );
            ++added;
        }
        if( added == 0 ) {
            return;
        }
        int equip = 0;
        std::vector<int> nodes;
        {
            ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
            mp_pickup_preset preset;
            mp_pickup_selector sel( viewer, preset );
            sel.set_title( _( "Pickup" ) );
            sel.set_hint( _( "Select the items to pick up" ) );
            sel.clear_items();
            sel.add_character_items( viewer );
            const int wield_key = mp_action_hotkey( ACTION_WIELD, 'w' );
            const int wear_key = mp_action_hotkey( ACTION_WEAR, 'W' );
            sel.row_action_handler = [&sel, wield_key, wear_key]( const std::string & action,
            const input_event & raw ) {
                int kind = -1;
                if( action == "CONFIRM" ) {
                    kind = 0;
                } else if( raw.get_first_input() > 0 && raw.get_first_input() == wield_key ) {
                    kind = 1;
                } else if( raw.get_first_input() > 0 && raw.get_first_input() == wear_key ) {
                    kind = 2;
                } else {
                    return false;
                }
                std::vector<int> chosen = sel.chosen_nodes();
                if( chosen.empty() ) {
                    return false;
                }
                sel.nodes = std::move( chosen );
                sel.equip = kind;
                return true;
            };
            sel.execute();
            nodes = sel.nodes;
            equip = sel.equip;
        }
        mp_inv_log( string_format( "pickup: rows=%d chosen=%d equip=%d",
                                   static_cast<int>( pkt.entries.size() ),
                                   static_cast<int>( nodes.size() ), equip ) );
        if( !nodes.empty() ) {
            netplay::client_send_pickup_action( pkt.header.generation, nodes, equip );
        }
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "pickup error: " ) + e.what() );
    } catch( ... ) {
        mp_inv_log( "pickup error: unknown" );
    }
}

/* DDA MP v4.0a-17: exact host inventory on the client.
 *
 * The client keeps a real Character mirror of its avatar (raw JSON from the
 * host). The game's own inventory_pick_selector runs on that mirror, so the
 * list, categories, counts, invlets and item info are the original UI over the
 * original item objects. Each mirror item is tagged with the host row index
 * through its stable mp_uid, so an action names the very same host item even
 * if the mirror rebuilt the inventory in a different order. The streamed rows
 * remain only as a fallback when no mirror snapshot is available. */
/* DDA MP v4.0a-159: start client-side throw targeting for the chosen item.
 * Shared by the inventory item menu and the 't' throw-item selection, so both
 * paths pick a throwable and enter the same reticle. */
static void mp_start_client_throw( const item &it, const std::string &uid )
{
    static const bool mp_legacy_throw = []() {
        const char *env = std::getenv( "DDA_MP_LEGACY_THROW" );
        return env != nullptr && env[0] != '\0' && std::string( env ) != "0";
    }();
    // Running the stock target_ui against the client's local map needs a fully
    // initialised client game/map; it is opt-in until the client has a real map
    // view (otherwise it can hard-crash).
    static const bool mp_try_host_aim = []() {
        const char *env = std::getenv( "DDA_MP_TRY_HOST_AIM" );
        return env != nullptr && env[0] != '\0' && std::string( env ) != "0";
    }();
    if( !mp_legacy_throw && mp_try_host_aim && mp_have_origin ) {
        mp_run_client_throw( it, uid,
                             tripoint_abs_ms( mp_origin_x, mp_origin_y, mp_origin_z ) );
        return;
    }
    avatar &viewer = get_avatar();
    mp_throw_uid = uid;
    mp_throw_dx = 1;
    mp_throw_dy = 0;
    mp_throw_name = it.tname();
    mp_throw_range = std::max( 1, viewer.throw_range( it ) );
    const std::string tr_var = it.get_var( "mp_throw_range", std::string() );
    if( !tr_var.empty() ) {
        mp_throw_range = std::max( 1, std::atoi( tr_var.c_str() ) );
    }
    mp_throw_active = true;
    // Same idea as target_ui::choose_initial_target: start on the nearest
    // visible target in range instead of an arbitrary tile.
    {
        auto glyph_at = []( int dx, int dy ) -> uint32_t {
            if( mp_aim_w <= 0 || mp_aim_h <= 0 ) {
                return 0;
            }
            const int x = mp_aim_w / 2 + dx;
            const int y = mp_aim_h / 2 + dy;
            if( x < 0 || y < 0 || x >= mp_aim_w || y >= mp_aim_h ) {
                return 0;
            }
            const size_t i = static_cast<size_t>( y ) * mp_aim_w + x;
            return i < mp_aim_glyphs.size() ? mp_aim_glyphs[i] : 0;
        };
        int best = INT_MAX;
        for( const netplay::aim_target &t : mp_aim_targets ) {
            // AIM_TARGETS offsets can be one tile off from the glyph cells;
            // snap to the character glyph when present.
            int tx = t.dx;
            int ty = t.dy;
            if( t.is_character && glyph_at( tx, ty ) != '@' ) {
                static const int offs[8][2] = {
                    { -1, 0 }, { 1, 0 }, { 0, -1 }, { 0, 1 },
                    { -1, -1 }, { -1, 1 }, { 1, -1 }, { 1, 1 }
                };
                for( const auto &o : offs ) {
                    if( glyph_at( t.dx + o[0], t.dy + o[1] ) == '@' ) {
                        tx = t.dx + o[0];
                        ty = t.dy + o[1];
                        break;
                    }
                }
            }
            const int d = std::abs( tx ) + std::abs( ty );
            if( d > 0 && d <= mp_throw_range && d < best ) {
                best = d;
                mp_throw_dx = tx;
                mp_throw_dy = ty;
            }
        }
    }
    mp_aim_text.clear();
    mp_client_aim_lines.clear();
    mp_aim_char.reset();
    if( !mp_char_json_cache.empty() ) {
        try {
            JsonValue jv = json_loader::from_string( mp_char_json_cache );
            std::unique_ptr<npc> tmp = std::make_unique<npc>();
            tmp->deserialize( jv.get_object() );
            mp_aim_char = std::move( tmp );
            mp_aim_item = it;
        } catch( ... ) {
            mp_aim_char.reset();
        }
    }
}

/// DDA MP v4.0a-185: recipe view from the client item menu. Defined after the
/// crafting helper it calls.
static void mp_run_view_recipe( const item &it );

static void mp_run_host_inventory( const std::string &text )
{
    try {
    // Block only the UI that existed before the session (the main menu we
    // joined from). Created before the inventory windows, so every session
    // window and popup draws above it while pre-session UI stays hidden.
    ui_adaptor mp_session_blocker( ui_adaptor::disable_uis_below{} );
    // DDA MP v4.1: the inventory exists only on the mirror. No rows are
    // rebuilt here any more; if the snapshot has not arrived yet the menu
    // waits for it instead of showing a synthetic copy.
    (void)text;
    static const bool mp_look_on = []() {
        const char *v = std::getenv( "DDA_MP_NATIVE_LOOK" );
        return !( v != nullptr && v[0] != '\0' && std::string( v ) == "0" );
    }();
    if( !mp_look_on ) {
        return;
    }
    if( !mp_native_avatar_ready ) {
        mp_inv_log( "inventory: no character mirror yet" );
        return;
    }
    avatar &viewer = get_avatar();
    // Same preset/state as game_menus::inv::common, so the client's category
    // view/sort mode matches the host's inventory instead of a fresh default.
    inventory_selector_preset inv_preset = default_preset;
    inv_preset.save_state = &inventory_ui_default_state;
    inventory_pick_selector sel( viewer, inv_preset );
    sel.set_title( _( "Inventory" ) );
    int assigned = 0;
    for( const item *it : viewer.inv_dump() ) {
        if( it != nullptr && it->invlet != '\0' ) {
            ++assigned;
        }
    }
    // Exact host string (with colour tags) so the client's own translation
    // is used instead of falling back to English.
    sel.set_hint( string_format(
                      _( "Item hotkeys assigned: <color_light_gray>%d</color>/<color_light_gray>%d</color>" ),
                      assigned, static_cast<int>( inv_chars.size() ) ) );
    sel.show_invlet( true );
    sel.drag_enabled = true;
    // The selector constructor does not populate itself; the host's menu does
    // this explicitly before every pass (see game_menus::inv::common).
    sel.clear_items();
    sel.add_character_items( viewer );
    mp_inv_log( string_format( "built: top=%d worn=%d armed=%d",
                               static_cast<int>( viewer.inv_dump().size() ),
                               static_cast<int>( viewer.worn.size() ),
                               viewer.is_armed() ? 1 : 0 ) );
    // Same flow as game_menus::inv::common: pick an item, show its action
    // menu, and when "open" is chosen dive into the container and repeat.
    std::vector<item_location> open_stack;
    while( true ) {
        item_location chosen;
        if( open_stack.empty() ) {
            sel.clear_items();
            sel.add_character_items( viewer );
            chosen = sel.execute();
        } else {
            item_location &container = open_stack.back();
            container_inventory_selector cs( viewer, container );
            cs.set_title( string_format( _( "Inventory of %s" ), container->tname() ) );
            cs.set_hint( string_format(
                             _( "Item hotkeys assigned: <color_light_gray>%d</color>/<color_light_gray>%d</color>" ),
                             assigned, static_cast<int>( inv_chars.size() ) ) );
            cs.clear_items();
            cs.add_contained_items( container );
            chosen = cs.execute();
            if( !chosen ) {
                open_stack.pop_back();
                continue;
            }
        }
        if( !chosen ) {
            break;
        }
        const item &it = *chosen;
        const std::string uid = it.get_var( "mp_uid", std::string() );
        if( uid.empty() ) {
            mp_inv_log( "action: item has no mp_uid, ignored" );
            break;
        }
        const int act = mp_client_item_menu( viewer, it );
        if( act == 'o' && it.is_container() ) {
            open_stack.push_back( chosen );
            continue;
        }
        if( act == 'i' && it.is_container() ) {
            // Same selector chain as game_menus::inv::insert_items: the stock
            // insert menu over the temp inventory, then one command per item.
            inventory_holster_preset holster_preset( chosen, &viewer );
            inventory_insert_selector insert_menu( viewer, holster_preset,
                                                   _( "ITEMS TO INSERT" ), false );
            insert_menu.add_character_items( viewer );
            insert_menu.set_display_stats( true );
            insert_menu.set_title( string_format( _( "Put item into %s" ), it.tname() ) );
            insert_menu.set_hint( string_format( _( "Choose an item to put into your %s" ),
                                                 it.tname( 1, false ) ) );
            const drop_locations picked = insert_menu.execute();
            std::vector<std::string> insert_uids;
            for( const drop_location &dl : picked ) {
                const std::string src_uid = dl.first->get_var( "mp_uid", std::string() );
                if( !src_uid.empty() ) {
                    insert_uids.push_back( src_uid );
                }
            }
            if( !insert_uids.empty() ) {
                netplay::client_send_inventory_uid( uid, 'i', insert_uids );
            }
            mp_inv_log( string_format( "action: uid key=i targets=%d",
                                       static_cast<int>( insert_uids.size() ) ) );
            break;
        }
        if( act == 'v' && it.is_container() ) {
            // Stock pocket menu on the temp item, then stream the resulting
            // per-pocket settings back so the host item stays authoritative.
            chosen->get_contents().favorite_settings_menu( chosen );
            std::ostringstream os;
            JsonOut js( os );
            js.start_array();
            for( item_pocket *pocket : chosen->get_pockets(
            []( const item_pocket & ) {
            return true;
            } ) ) {
                pocket->settings.serialize( js );
            }
            js.end_array();
            netplay::client_send_inventory_settings_uid( uid, os.str() );
            mp_inv_log( "action: uid key=v settings" );
            break;
        }
        if( act == '=' ) {
            const int invlet = popup_getkey(
                                   _( "Enter new letter.  Press SPACE to clear a manually-assigned letter, ESCAPE to cancel." ) );
            if( invlet != KEY_ESCAPE && ( invlet == ' ' || inv_chars.valid( invlet ) ) ) {
                netplay::client_send_inventory_uid( uid, '=', {}, { invlet } );
            }
            break;
        }
        if( act == 'g' ) {
            // Partner interaction: the host hands this item to the nearest
            // adjacent player (another session avatar or the host player).
            netplay::client_send_inventory_uid( uid, 'g', {}, {} );
            mp_inv_log( "action: uid key=g give" );
            break;
        }
        if( act == 't' ) {
            mp_start_client_throw( it, uid );
            break;
        }
        if( act == 'V' ) {
            mp_run_view_recipe( it );
            break;
        }
        if( act > 0 ) {
            netplay::client_send_inventory_uid( uid, act );
            mp_inv_log( string_format( "action: uid key=%c", static_cast<char>( act ) ) );
        }
        break;
    }
    mp_inv_log( "closed" );
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "error: " ) + e.what() );
    } catch( ... ) {
        mp_inv_log( "error: unknown" );
    }
}

/* DDA MP v4.0a-159: 't' opens the throw-item selection (like the host's
 * avatar_action::plthrow -> game_menus::inv::titled_menu( "Throw item" )),
 * not the general inventory. Picking an item starts the throw reticle. */
static void mp_run_throw_select( const std::string &text )
{
    try {
        ui_adaptor mp_session_blocker( ui_adaptor::disable_uis_below{} );
        (void)text;
        if( !mp_native_avatar_ready ) {
            mp_inv_log( "throw: no character mirror yet" );
            return;
        }
        avatar &viewer = get_avatar();
        inventory_selector_preset inv_preset;
        inventory_pick_selector sel( viewer, inv_preset );
        sel.set_title( _( "Throw item" ) );
        sel.set_display_stats( false );
        sel.clear_items();
        sel.add_character_items( viewer );
        if( sel.empty() ) {
            popup( _( "You don't have any items to throw." ) );
            return;
        }
        item_location chosen = sel.execute();
        if( !chosen ) {
            return;
        }
        const item &it = *chosen;
        const std::string uid = it.get_var( "mp_uid", std::string() );
        if( uid.empty() ) {
            mp_inv_log( "throw: item has no mp_uid, ignored" );
            return;
        }
        mp_start_client_throw( it, uid );
        mp_inv_log( "throw: selected" );
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "throw error: " ) + e.what() );
    } catch( ... ) {
        mp_inv_log( "throw error: unknown" );
    }
}

/* DDA MP v4.0a-39: the host's real aiming UI on the client.
 *
 * target_ui is now Character-based. The client's local avatar is temporarily
 * moved to the streamed world origin (the session avatar's position) and the
 * bubble is re-anchored, so g->draw/mode_throw show the host terrain and the
 * stock reticle. Only the chosen offset is sent to the host. */
/* DDA MP v4.0a-45: host-style aiming panel, drawn after the canvas. The host
 * text carries <color_x> tags; they are parsed here instead of showing tags. */
/* DDA MP v4.0a-46: the host renders its real w_target aiming window and
 * streams the cells; the client simply paints that grid, so layout, colours,
 * target info, steadiness bar and chances match cell for cell. */
static netplay::sidebar_frame mp_aim_panel;
static bool mp_have_aim_panel = false;
static std::vector<netplay::sidebar_cell> mp_aim_title_cells;

static void mp_build_client_aim_panel();
static void mp_client_aim_recalc()
{
    mp_client_aim_lines.clear();
    if( !mp_aim_char || mp_aim_w <= 0 || mp_aim_h <= 0 ) {
        return;
    }
    const int tx = mp_aim_w / 2 + mp_throw_dx;
    const int ty = mp_aim_h / 2 + mp_throw_dy;
    if( tx < 0 || ty < 0 || tx >= mp_aim_w || ty >= mp_aim_h ) {
        return;
    }
    const size_t idx = static_cast<size_t>( ty ) * mp_aim_w + tx;
    const std::string critter = idx < mp_aim_critters.size() ? mp_aim_critters[idx] : "";
    const bool on_char_glyph = idx < mp_aim_glyphs.size() && mp_aim_glyphs[idx] == '@';
    const bool is_character = on_char_glyph ||
                              critter.rfind( "npc_", 0 ) == 0 ||
                              critter.rfind( "player_", 0 ) == 0;
    const int light = idx < mp_aim_light.size() ? mp_aim_light[idx] : 0;
    const bool sees = ( idx < mp_aim_flags.size() && ( mp_aim_flags[idx] & 32 ) != 0 ) ||
                      light > 0;
    const double range = std::sqrt( static_cast<double>( mp_throw_dx * mp_throw_dx +
                                   mp_throw_dy * mp_throw_dy ) );
    // DDA MP v4.0a-75: the exact dodge of a character target, so the client's
    // dispersion matches the host's own aiming panel.
    double target_dodge = 0.0;
    if( is_character ) {
        for( const netplay::aim_target &t : mp_aim_targets ) {
            if( t.dx == mp_throw_dx && t.dy == mp_throw_dy && t.is_character ) {
                target_dodge = t.dodge;
                break;
            }
        }
    }
    mp_client_aim_lines = target_handler::netplay_throw_aim_client(
                              *mp_aim_char, mp_aim_item, critter, is_character, range,
                              mp_throw_range, light * 50.0f, sees, target_dodge );
    mp_build_client_aim_panel();
}

static void mp_build_client_aim_panel()
{
    // DDA MP v4.0a-80: clamp to the real terminal; a window narrower than the
    // host's 55 columns used to place the offscreen window at a negative x,
    // and a short terminal printed the hotkey hints above row 0.
    const int wwid = std::min( 55, TERMX );
    const int whei = std::min( 33, TERMY );
    if( wwid < 20 || whei < 10 ) {
        return;
    }
    catacurses::window w = catacurses::newwin( whei, wwid, point( TERMX - wwid, 0 ) );
    if( !w ) {
        return;
    }
    werase( w );
    draw_border( w );
    mvwprintz( w, point( 2, 0 ), c_white, "< " );
    trim_and_print( w, point( 4, 0 ), wwid - 7, c_red,
                    string_format( _( "Throwing %s" ), mp_throw_name ) );
    wprintz( w, c_white, " >" );

    // Target name: minimap beacons cover the host and creatures whose cell id
    // is not in the snapshot; otherwise fall back to the critter type.
    std::string target_name;
    std::string target_id;
    bool on_character_glyph = false;
    if( mp_aim_w > 0 && mp_aim_h > 0 ) {
        const int gx = mp_aim_w / 2 + mp_throw_dx;
        const int gy = mp_aim_h / 2 + mp_throw_dy;
        if( gx >= 0 && gy >= 0 && gx < mp_aim_w && gy < mp_aim_h ) {
            const size_t gidx = static_cast<size_t>( gy ) * mp_aim_w + gx;
            on_character_glyph = gidx < mp_aim_glyphs.size() && mp_aim_glyphs[gidx] == '@';
        }
    }
    const netplay::aim_target *target_char = nullptr;
    for( const netplay::aim_target &t : mp_aim_targets ) {
        if( t.dx == mp_throw_dx && t.dy == mp_throw_dy ) {
            // Character rows are only valid when the cursor cell really holds
            // the character glyph; otherwise a neighbouring tile would show
            // the target as if it stood there.
            if( !t.is_character || on_character_glyph ) {
                target_name = t.name;
                if( t.is_character ) {
                    target_char = &t;
                }
            }
            break;
        }
    }
    if( mp_aim_w > 0 && mp_aim_h > 0 ) {
        const int tx = mp_aim_w / 2 + mp_throw_dx;
        const int ty = mp_aim_h / 2 + mp_throw_dy;
        if( tx >= 0 && ty >= 0 && tx < mp_aim_w && ty < mp_aim_h ) {
            const size_t idx = static_cast<size_t>( ty ) * mp_aim_w + tx;
            if( idx < mp_aim_critters.size() && !mp_aim_critters[idx].empty() ) {
                target_id = mp_aim_critters[idx];
                if( target_name.empty() &&
                    mtype_id( target_id ).is_valid() ) {
                    target_name = mtype_id( target_id ).obj().nname();
                }
            }
        }
    }

    nc_color col = c_light_gray;
    int ty = 1;
    const double dist = std::sqrt( static_cast<double>( mp_throw_dx * mp_throw_dx +
                                   mp_throw_dy * mp_throw_dy ) );
    // If the cursor is on a character glyph but the target list offset is one
    // tile off, resolve the nearest character target.
    if( target_char == nullptr && mp_aim_w > 0 && mp_aim_h > 0 ) {
        const int cx = mp_aim_w / 2 + mp_throw_dx;
        const int cy = mp_aim_h / 2 + mp_throw_dy;
        if( cx >= 0 && cy >= 0 && cx < mp_aim_w && cy < mp_aim_h ) {
            const size_t cidx = static_cast<size_t>( cy ) * mp_aim_w + cx;
            if( cidx < mp_aim_glyphs.size() && mp_aim_glyphs[cidx] == '@' ) {
                int best = 2;
                for( const netplay::aim_target &t : mp_aim_targets ) {
                    if( !t.is_character ) {
                        continue;
                    }
                    const int d = std::abs( t.dx - mp_throw_dx ) +
                                  std::abs( t.dy - mp_throw_dy );
                    if( d < best ) {
                        best = d;
                        target_char = &t;
                        target_name = t.name;
                    }
                }
            }
        }
    }
    int targets_in_range = 0;
    for( const netplay::aim_target &t : mp_aim_targets ) {
        if( t.dx * t.dx + t.dy * t.dy <= mp_throw_range * mp_throw_range ) {
            ++targets_in_range;
        }
    }
    print_colored_text( w, point( 1, ty++ ), col, col,
                        string_format( "<color_light_gray>%s  %s  %s</color>",
                                       string_format( _( "Range: %d/%d" ),
                                               static_cast<int>( dist ), mp_throw_range ),
                                       string_format( _( "Elevation: %d" ), 0 ),
                                       string_format( _( "Targets: %d" ), targets_in_range ) ) );
    ++ty; // blank line after the cursor stats, as in target_ui
    if( target_char != nullptr ) {
        // Raw NPC facts from the AIM_TARGETS packet, laid out like npc::print_info.
        mvwprintz( w, point( 1, ty++ ), c_light_blue, _( "-----CHARACTER-----" ) );
        const std::pair<std::string, nc_color> bar = get_hp_bar( target_char->hp_pct, 100 );
        mvwprintz( w, point( 1, ty ), bar.second, bar.first );
        mvwhline( w, point( 4, ty ), c_white, '.',
                  std::max( 0, 5 - utf8_width( bar.first ) ) );
        trim_and_print( w, point( 7, ty ), wwid - 9,
                        decode_cell_color( target_char->name_color ),
                        target_char->name );
        ++ty;
        const auto att = Creature::get_attitude_ui_data(
                             static_cast<Creature::Attitude>( target_char->attitude ) );
        mvwprintz( w, point( 1, ty ), att.second, "%s", att.first.translated() );
        wprintz( w, c_light_gray, ";" );
        wprintz( w, decode_cell_color( target_char->name_color ), " %s",
                 npc_attitude_name( static_cast<npc_attitude>( target_char->npc_attitude ) ) );
        ++ty;
        const bool sees_you = target_char->sees_you != 0;
        print_colored_text( w, point( 1, ty++ ), col, col,
                            string_format( "<color_%s>%s</color>",
                                           sees_you ? "yellow" : "green",
                                           sees_you ? _( "Aware of your presence" ) :
                                           _( "Unaware of you" ) ) );
        if( !target_char->wielded.empty() ) {
            print_colored_text( w, point( 1, ty++ ), col, col,
                                std::string( "<color_light_gray>" ) + _( "Wielding: " ) +
                                std::string( "</color><color_red>" ) + target_char->wielded +
                                std::string( "</color>" ) );
        }
    } else if( !target_id.empty() && mp_aim_char && mtype_id( target_id ).is_valid() ) {
        monster tmp( mtype_id( target_id ), mp_aim_char->pos_bub() );
        ty = tmp.print_info( w, ty, 6, 1 );
    } else if( !target_name.empty() && ty < whei - 4 ) {
        print_colored_text( w, point( 1, ty++ ), col, col,
                            string_format( "<color_white>%s %s</color>",
                                           _( "Target:" ), target_name ) );
    }
    ty += 2; // two blank lines between the character block and steadiness
    for( size_t li = 1; li < mp_client_aim_lines.size(); li++ ) {
        if( ty >= whei - 6 ) {
            break;
        }
        ty += fold_and_print( w, point( 1, ty ), wwid - 2, col, mp_client_aim_lines[li] );
    }
    mvwprintz( w, point( 1, whei - 5 ), c_white,
               _( "Move cursor with directional keys" ) );
    mvwprintz( w, point( 1, whei - 4 ), c_white,
               "%s %s", _( "Mouse: LMB: Target, Wheel: Cycle," ), _( "RMB: Fire" ) );
    mvwprintz( w, point( 1, whei - 3 ), c_white,
               "%s %s", string_format( _( "[%s] Cycle targets;" ), "TAB" ),
               string_format( _( "[%s] %s." ), "f",
                              to_translation( "[Hotkey] to throw", "to throw" ).translated() ) );
    mvwprintz( w, point( 1, whei - 2 ), c_white,
               string_format( _( "[%s] target self; [%s] toggle snap-to-target" ),
                              "0", "*" ) );

    // Send only the interior; the client paints its own border (ACS glyphs
    // are not in the client font).
    mp_aim_panel.width = wwid - 2;
    mp_aim_panel.height = whei - 2; // interior only; title goes on the border
    mp_aim_panel.cells.assign(
        static_cast<size_t>( mp_aim_panel.width ) * mp_aim_panel.height, {} );
    cata_cursesport::WINDOW *wraw = w.get<cata_cursesport::WINDOW>();
    mp_aim_title_cells.assign( static_cast<size_t>( mp_aim_panel.width ), {} );
    for( int x = 0; x < wwid - 2; x++ ) {
        const cata_cursesport::cursecell &raw = wraw->line[0].chars[x + 1];
        mp_aim_title_cells[static_cast<size_t>( x )].text = raw.ch;
        mp_aim_title_cells[static_cast<size_t>( x )].color = static_cast<int>( raw.FG );
        mp_aim_title_cells[static_cast<size_t>( x )].bg = static_cast<int>( raw.BG );
    }
    for( int y = 0; y < whei - 2; y++ ) {
        for( int x = 0; x < wwid - 2; x++ ) {
            const cata_cursesport::cursecell &raw = wraw->line[y + 1].chars[x + 1];
            netplay::sidebar_cell &c = mp_aim_panel.cells[
                                           static_cast<size_t>( y ) * mp_aim_panel.width + x];
            c.text = raw.ch;
            c.color = static_cast<int>( raw.FG );
            c.bg = static_cast<int>( raw.BG );
        }
    }
    mp_have_aim_panel = true;
}


static void mp_draw_aim_panel()
{
    if( !mp_have_aim_panel || mp_aim_panel.width <= 0 || mp_aim_panel.height <= 0 ) {
        return;
    }
    const ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float cw = ImGui::CalcTextSize( "M" ).x;
    const float ch = ImGui::GetTextLineHeight();
    const float pw = ( mp_aim_panel.width + 2 ) * cw;
    const float ph = ( mp_aim_panel.height + 2 ) * ch;
    const float px = std::max( 4.0f, io.DisplaySize.x - pw - 4.0f );
    const float py = 0.0f;
    dl->AddRectFilled( ImVec2( px - cw * 0.5f, py - ch * 0.5f ),
                       ImVec2( px + pw + cw * 0.5f, py + ph + ch * 0.5f ),
                       IM_COL32( 0, 0, 0, 255 ) );
    auto is_box_glyph = []( const std::string & s ) {
        if( s.empty() ) {
            return true;
        }
        const unsigned char c = static_cast<unsigned char>( s[0] );
        if( c >= 0x20 && c < 0x7F ) {
            return false; // ASCII text/punctuation
        }
        if( c == 0xD0 || c == 0xD1 ) {
            return false; // Cyrillic
        }
        if( c == 0xE2 && s.size() >= 3 &&
            static_cast<unsigned char>( s[1] ) == 0x80 ) {
            return false; // dashes/quotes
        }
        return true; // box drawing / unknown glyph -> skip (tofu)
    };
    // Title sits on the top border; the border line is interrupted under it
    // (like draw_window_title overwriting the border cell).
    int title_first = -1;
    int title_last = -1;
    for( size_t x = 0; x < mp_aim_title_cells.size(); x++ ) {
        const netplay::sidebar_cell &c = mp_aim_title_cells[x];
        if( !c.text.empty() && c.text != " " && !is_box_glyph( c.text ) ) {
            if( title_first < 0 ) {
                title_first = static_cast<int>( x );
            }
            title_last = static_cast<int>( x );
        }
    }
    const float gap_start = title_first < 0 ? px : px + ( title_first + 1.5f ) * cw;
    const float gap_end = title_last < 0 ? px : px + ( title_last + 1.5f ) * cw;
    const std::string help = "< " + string_format( _( "[%s] show all controls" ), "?" ) + " >";
    const float help_w = ImGui::CalcTextSize( help.c_str() ).x;
    const float help_x = px + pw - help_w - cw;
    const ImU32 border = IM_COL32( 190, 190, 190, 255 );
    const float top_y = py + ch * 0.5f;
    dl->AddLine( ImVec2( px, top_y ), ImVec2( gap_start, top_y ), border );
    dl->AddLine( ImVec2( gap_end, top_y ), ImVec2( px + pw, top_y ), border );
    dl->AddLine( ImVec2( px, py + ph ), ImVec2( help_x + cw * 0.5f, py + ph ), border );
    dl->AddLine( ImVec2( help_x + help_w - cw * 0.5f, py + ph ),
                 ImVec2( px + pw, py + ph ), border );
    dl->AddLine( ImVec2( px, top_y ), ImVec2( px, py + ph ), border );
    dl->AddLine( ImVec2( px + pw, top_y ), ImVec2( px + pw, py + ph ), border );
    for( size_t x = 0; x < mp_aim_title_cells.size(); x++ ) {
        const netplay::sidebar_cell &c = mp_aim_title_cells[x];
        if( c.bg != 0 ) {
            dl->AddRectFilled( ImVec2( px + ( static_cast<float>( x ) + 1 ) * cw, py ),
                               ImVec2( px + ( static_cast<float>( x ) + 2 ) * cw, py + ch ),
                               cataimgui::ImU32_from_color( decode_cell_color( c.bg ) ) );
        }
        if( c.text.empty() || c.text == " " || is_box_glyph( c.text ) ) {
            continue;
        }
        dl->AddText( ImVec2( px + ( static_cast<float>( x ) + 1 ) * cw, py ),
                     cataimgui::ImU32_from_color( decode_cell_color( c.color ) ),
                     c.text.c_str() );
    }
    for( int row = 0; row < mp_aim_panel.height; row++ ) {
        for( int col = 0; col < mp_aim_panel.width; col++ ) {
            const netplay::sidebar_cell &c = mp_aim_panel.cells[
                                                 static_cast<size_t>( row ) * mp_aim_panel.width + col];
            if( c.bg != 0 ) {
                dl->AddRectFilled( ImVec2( px + ( col + 1 ) * cw, py + ( row + 1 ) * ch ),
                                   ImVec2( px + ( col + 2 ) * cw, py + ( row + 2 ) * ch ),
                                   cataimgui::ImU32_from_color( decode_cell_color( c.bg ) ) );
            }
            if( c.text.empty() || c.text == " " ) {
                continue;
            }
            dl->AddText( ImVec2( px + ( col + 1 ) * cw, py + ( row + 1 ) * ch ),
                         cataimgui::ImU32_from_color( decode_cell_color( c.color ) ),
                         c.text.c_str() );
        }
    }
    // Bottom border label, like draw_help_notice (brackets included).
    const float hy = py + ph - ch * 0.5f;
    dl->AddRectFilled( ImVec2( help_x - 2, hy - 1 ),
                       ImVec2( help_x + help_w + 2, hy + ch ),
                       IM_COL32( 0, 0, 0, 255 ) );
    dl->AddText( ImVec2( help_x, hy ), IM_COL32( 230, 230, 230, 255 ), help.c_str() );
}
/* DDA MP v4.0a-121: ranged fire. The client picks a target with the stock
 * select-only cursor on its own map mirror; the host fires the avatar's
 * wielded gun at that offset with Character::fire_gun, so ballistics,
 * ammo, recoil and messages stay on the authoritative side. */
/* DDA MP v4.0a-150/159: tile and creature info for the look cursor. With the
 * native renderer this is a real curses window the game paints itself (same
 * colors, backgrounds and height as the host's look window); the ImGui blit
 * remains only for the non-native replay fallback. The caller owns
 * overlay_begin/end so this and the sidebar share one frame. */
static int mp_native_sidebar_width();
static bool mp_native_render_enabled();

static void mp_draw_cursor_info( const snapshot &snap )
{
    tripoint_bub_ms lp;
    tripoint_abs_ms ap;
    if( !mp_cursor_target( snap, ap, lp ) ) {
        return;
    }
    map &here = get_map();
    const visibility_variables cache = here.get_visibility_variables_cache();
    // DDA MP v4.0a-159: the look window is the whole sidebar, exactly like
    // game::look_around: full width, full height, bottom control hints and
    // pre_print_all_tile_info (which adds the overmap place name above the
    // terrain header).
    const int info_w = std::min( TERMX, std::max( 30, mp_native_sidebar_width() ) );
    const int info_h = TERMY;
    const bool right = get_option<std::string>( "SIDEBAR_POSITION" ) == "right";
    const int info_x = right ? std::max( 0, TERMX - info_w ) : 0;
    static catacurses::window mp_info_win;
    static int mp_info_w = -1;
    static int mp_info_h = -1;
    static int mp_info_x = -1;
    if( !mp_info_win || mp_info_w != info_w || mp_info_h != info_h ||
        mp_info_x != info_x ) {
        mp_info_win = catacurses::newwin( info_h, info_w, point( info_x, 0 ) );
        mp_info_w = info_w;
        mp_info_h = info_h;
        mp_info_x = info_x;
    }
    if( !mp_info_win ) {
        return;
    }
    catacurses::window win = mp_info_win;
    werase( win );
    draw_border( win );
    const std::string title = mp_throw_active ? netplay::fork_text( "< <color_green>Throwing</color> >" ) :
                        mp_cursor == mp_cursor_mode::fire ? netplay::fork_text( "< <color_green>Aiming</color> >" ) :
                        mp_cursor == mp_cursor_mode::spell ? netplay::fork_text( "< <color_green>Casting</color> >" ) :
                        _( "< <color_green>Look around</color> >" );
    center_print( win, 0, c_white, title );
    {
        input_context lctxt( "LOOK" );
        lctxt.register_action( "EXTENDED_DESCRIPTION" );
        lctxt.register_action( "TOGGLE_FAST_SCROLL" );
        mvwprintz( win, point( 1, info_h - 3 ), c_light_green, "%s - %s",
                   lctxt.get_desc( "EXTENDED_DESCRIPTION" ),
                   lctxt.get_action_name( "EXTENDED_DESCRIPTION" ) );
        mvwprintz( win, point( 1, info_h - 2 ),
                   mp_look_fast_scroll ? c_light_green : c_green, "%s - %s",
                   lctxt.get_desc( "TOGGLE_FAST_SCROLL" ),
                   lctxt.get_action_name( "TOGGLE_FAST_SCROLL" ) );
    }
    int line = 1;
    try {
        g->pre_print_all_tile_info( lp, win, line, info_h - 4, cache );
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "lookinfo: error " ) + e.what() );
        return;
    } catch( ... ) {
        mp_inv_log( "lookinfo: error unknown" );
        return;
    }
    if( mp_native_render_enabled() ) {
        // The regular curses pass paints this window with the exact colors and
        // backgrounds the host uses.
        wnoutrefresh( win );
        return;
    }
    cata_cursesport::WINDOW *raw = win.get<cata_cursesport::WINDOW>();
    if( raw == nullptr ) {
        return;
    }
    const ImGuiIO &io = ImGui::GetIO();
    ImDrawList *dl = ImGui::GetForegroundDrawList();
    const float cw = ImGui::CalcTextSize( "M" ).x;
    const float ch = ImGui::GetTextLineHeight();
    const float x0 = std::max( 0.0f, io.DisplaySize.x - info_w * cw - cw );
    const float x1 = std::min( io.DisplaySize.x, x0 + info_w * cw );
    // Cover the native/pixel sidebar: the host shows this info in exactly that
    // place.
    dl->AddRectFilled( ImVec2( std::max( 0.0f, x0 - cw ), 0.0f ), io.DisplaySize,
                       IM_COL32( 0, 0, 0, 255 ) );
    dl->AddRect( ImVec2( x0 - cw, 0.0f ), ImVec2( x1, info_h * ch ),
                 IM_COL32( 140, 140, 140, 255 ) );
    auto box_glyph = []( const std::string &s ) {
        if( s.empty() ) {
            return true;
        }
        const unsigned char c = static_cast<unsigned char>( s[0] );
        if( c >= 0x20 && c < 0x7F ) {
            return false;
        }
        if( c == 0xD0 || c == 0xD1 ) {
            return false;
        }
        if( c == 0xE2 && s.size() >= 3 && static_cast<unsigned char>( s[1] ) == 0x80 ) {
            return false;
        }
        return true;
    };
    for( int row = 0; row < info_h; row++ ) {
        const int col_begin = 1;
        const int col_end = info_w - 1;
        int col = col_begin;
        while( col < col_end ) {
            const cata_cursesport::cursecell &c = raw->line[row].chars[col];
            if( c.ch.empty() || c.ch == " " ) {
                ++col;
                continue;
            }
            const cata_cursesport::base_color run_col = c.FG;
            std::string run;
            int end = col;
            while( end < col_end ) {
                const cata_cursesport::cursecell &cc = raw->line[row].chars[end];
                if( cc.ch.empty() || cc.ch == " " || cc.FG != run_col ) {
                    break;
                }
                run += cc.ch;
                ++end;
            }
            if( !box_glyph( run ) ) {
                dl->AddText( ImVec2( x0 + col * cw, row * ch ),
                             cataimgui::ImU32_from_color(
                                 decode_cell_color( static_cast<int>( run_col ) ) ),
                             run.c_str() );
            }
            col = end;
        }
    }
}
static void mp_run_client_fire()
{
    avatar &cli = get_avatar();
    const item_location weapon = cli.get_wielded_item();
    if( !weapon || !weapon->is_gun() ) {
        mp_inv_log( "fire: no gun wielded" );
        return;
    }
    const int range = weapon->gun_range( &cli );
    target_handler::trajectory traj;
    try {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        traj = target_handler::mode_select_only( cli, range );
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "fire error: " ) + e.what() );
        return;
    } catch( ... ) {
        mp_inv_log( "fire error: unknown" );
        return;
    }
    if( traj.empty() ) {
        mp_inv_log( "fire: cancelled" );
        return;
    }
    const tripoint_bub_ms tgt = traj.back();
    const tripoint_bub_ms from = cli.pos_bub();
    netplay::client_send_intent( "fire", 0,
                                 { tgt.x() - from.x(), tgt.y() - from.y(), 1 } );
    mp_inv_log( string_format( "fire: target %d,%d", tgt.x() - from.x(),
                               tgt.y() - from.y() ) );
}

/* DDA MP v4.0a-131: crafting. The client runs the stock crafting UI on its
 * mirrored avatar (recipe list, availability, batch size), then sends the
 * chosen recipe to the host, which starts the real craft activity on the
 * session avatar. Workbench variants use the host's map at the avatar. */
static void mp_run_client_craft( bool all, const std::string &filter = "",
                                 const recipe_id &goto_recipe = recipe_id() )
{
    if( !mp_native_avatar_ready ) {
        return;
    }
    avatar &you = get_avatar();
    int batch_size = 0;
    std::pair<Character *, const recipe *> chosen;
    try {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        chosen = select_crafter_and_crafting_recipe( batch_size, goto_recipe, &you, filter );
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "craft error: " ) + e.what() );
        return;
    } catch( ... ) {
        mp_inv_log( "craft error: unknown" );
        return;
    }
    if( chosen.second == nullptr ) {
        mp_inv_log( "craft: cancelled" );
        return;
    }
    if( batch_size < 1 ) {
        batch_size = 1;
    }
    netplay::client_send_intent( all ? "craft.all" : "craft", batch_size, {},
                                 chosen.second->ident().str() );
    mp_inv_log( string_format( "craft: %s batch=%d all=%d",
                               chosen.second->ident().str().c_str(), batch_size,
                               all ? 1 : 0 ) );
}

/* DDA MP v4.0a-185: the item menu's "view recipe" used to be disabled on the
 * client. Open the normal craft UI filtered to the item on the mirror; the
 * chosen recipe still travels to the host as the usual craft intent. */
static void mp_run_view_recipe( const item &it )
{
    if( !mp_native_avatar_ready ) {
        mp_inv_log( "recipe: mirror not ready" );
        return;
    }
    avatar &you = get_avatar();
    if( it.is_craft() ) {
        const recipe_id id = it.get_making().ident();
        if( !you.get_group_available_recipes().contains( &id.obj() ) ) {
            add_msg( m_info, _( "You don't know how to craft the %s!" ), id->result_name() );
            return;
        }
        mp_run_client_craft( false, "", id );
        return;
    }
    const itype_id item = it.typeId();
    const std::string itname = item->nname( 1U );
    bool is_byproduct = false;
    bool can_craft = false;
    for( const auto &[_, r] : recipe_dict ) {
        if( !r.obsolete && ( item == r.result() || r.in_byproducts( item ) ) ) {
            is_byproduct = true;
            if( you.get_group_available_recipes().contains( &r ) ) {
                can_craft = true;
                break;
            }
        }
    }
    if( !is_byproduct ) {
        add_msg( m_info, _( "You wonder if it's even possible to craft the %s..." ), itname );
        return;
    }
    if( !can_craft ) {
        add_msg( m_info, _( "You don't know how to craft the %s!" ), itname );
        return;
    }
    mp_run_client_craft( false, string_format( "r:%s", itname ) );
}

static void mp_run_client_throw( const item &thrown, const std::string &uid,
                                 const tripoint_abs_ms &origin )
{
    avatar &cli = get_avatar();
    const tripoint_abs_ms old_pos = cli.pos_abs();
    item copy = thrown;
    try {
        // Block UI that existed before the session while the stock aiming
        // window is open (same rule as inventory/@).
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        cli.setpos( origin, false );
        g->mp_anchor_map_on( cli, false );
        const target_handler::trajectory traj = target_handler::mode_throw( cli, copy, false );
        if( !traj.empty() ) {
            const tripoint_bub_ms tgt = traj.back();
            const tripoint_bub_ms from = cli.pos_bub();
            netplay::client_send_inventory_uid(
                uid, 't', {}, { tgt.x() - from.x(), tgt.y() - from.y() } );
            mp_inv_log( string_format( "throw: aim %d,%d", tgt.x() - from.x(),
                                       tgt.y() - from.y() ) );
        } else {
            mp_inv_log( "throw: cancelled" );
        }
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "throw error: " ) + e.what() );
    } catch( ... ) {
        mp_inv_log( "throw error: unknown" );
    }
    // Put the client back where it was; the session stream owns the bubble.
    cli.setpos( old_pos, false );
    g->mp_anchor_map_on( cli, true );
}

/* DDA MP v4.0a-171: build something from the client. The construction list is
 * pure data, so the client can show it without touching the host's map; the
 * direction is chosen here too, and the host validates the tile and consumes
 * the components for the session avatar (construction::place_construction_at). */
static void mp_run_client_construct()
{
    std::vector<construction_group_str_id> groups;
    for( const construction &con : get_constructions() ) {
        if( !con.on_display || con.is_blacklisted() ) {
            continue;
        }
        if( std::find( groups.begin(), groups.end(), con.group ) == groups.end() ) {
            groups.push_back( con.group );
        }
    }
    if( groups.empty() ) {
        return;
    }
    std::sort( groups.begin(), groups.end(),
    []( const construction_group_str_id & a, const construction_group_str_id & b ) {
        return a->name() < b->name();
    } );
    uilist menu;
    menu.text = netplay::fork_text( "Construct what?" );
    menu.title = netplay::fork_text( "Construction" );
    for( const construction_group_str_id &g : groups ) {
        menu.addentry( g->name() );
    }
    menu.query();
    if( menu.ret < 0 || menu.ret >= static_cast<int>( groups.size() ) ) {
        return;
    }
    const construction_group_str_id chosen = groups[menu.ret];
    static const int dxs[8] = { 0, 1, 1, 1, 0, -1, -1, -1 };
    static const int dys[8] = { -1, -1, 0, 1, 1, 1, 0, -1 };
    static const char *arrows[8] = { "\u2191", "\u2197", "\u2192", "\u2198",
                                     "\u2193", "\u2199", "\u2190", "\u2196" };
    uilist dir;
    dir.text = netplay::fork_text( "Construct where?" );
    dir.title = chosen->name();
    for( int i = 0; i < 8; i++ ) {
        dir.addentry( arrows[i] );
    }
    dir.query();
    if( dir.ret < 0 || dir.ret >= 8 ) {
        return;
    }
    netplay::client_send_intent( "construct", 0, { dxs[dir.ret], dys[dir.ret] },
                                 chosen.str() );
}

/* DDA MP v4.0a-174: custom action-menu id for "help the busy player". It is
 * higher than NUM_ACTIONS but handled before the category checks. */
static constexpr int MP_ACTION_HELP_NEARBY = 91000;
/// DDA MP v4.0a-198: read-only vehicle status entry.
static constexpr int MP_ACTION_VEHICLE_INFO = 91001;
/// DDA MP v4.0a-201: refuel vehicle from carried fuel.
static constexpr int MP_ACTION_VEHICLE_REFUEL = 91002;
/// DDA MP v4.0a-204: damaged vehicle parts / repair.
static constexpr int MP_ACTION_VEHICLE_PARTS = 91003;
/// Whether this client believes it is currently helping (drives the stop
/// confirmation). The host is authoritative: EVENT_HELP resets it when the
/// assistance ends or a request is refused.
static bool mp_client_helping = false;
/// DDA MP v4.0a-191: NPC used by the last mission-offer request.
static int mp_mission_dx = 0;
static int mp_mission_dy = 0;
/// DDA MP v4.0a-217: when the headless test accepted a mission (0 = never).
static std::chrono::steady_clock::time_point mp_test_mission_accepted{};
/// DDA MP v4.0a-193: NPC used by the last barter request.
static int mp_trade_dx = 0;
static int mp_trade_dy = 0;

/* DDA MP v4.0a-176: walking into another player shows the interaction menu
 * instead of attacking, like walking into an NPC in single player. */
static void mp_run_player_interact( const std::string &name, const std::string &activity )
{
    // DDA MP test hook: a scripted walk must not stop in a modal menu.
    if( const char *dismiss = std::getenv( "DDA_MP_TEST_DISMISS_MENUS" );
        dismiss != nullptr && dismiss[0] != '\0' ) {
        mp_inv_log( string_format( "interact dismissed: %s", name ) );
        return;
    }
    uilist menu;
    menu.text = string_format( netplay::fork_text( "Player: %s" ), name );
    int help_idx = -1;
    if( !activity.empty() ) {
        help_idx = 0;
        menu.addentry( string_format( netplay::fork_text( "Help: %s" ),
                                      netplay::fork_text( activity.c_str() ) ) );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    mp_inv_log( string_format( "interact open: %s activity=%s", name, activity ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    mp_inv_log( string_format( "interact: %s activity=%s ret=%d", name, activity, menu.ret ) );
    if( help_idx >= 0 && menu.ret == help_idx ) {
        // Already helping (this or another player): the host's help command
        // stops the assistance, so ask first, exactly like the action menu.
        if( mp_client_helping &&
            !query_yn( netplay::fork_text( "Stop helping?" ) ) ) {
            return;
        }
        netplay::client_send_intent( "help", 0, {} );
        mp_client_helping = !mp_client_helping;
    }
}

/* DDA MP v4.0a-181: text chat to every player. Shared by the direct 'T' key
 * and the action-menu entry, which used to fall through to the host and
 * report "not available". */
static void mp_run_client_chat()
{
    string_input_popup pop;
    pop.title( netplay::fork_text( "Chat message" ) );
    pop.max_length( 200 );
    pop.width( 60 );
    std::string msg;
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        msg = pop.query_string();
    }
    if( !msg.empty() ) {
        netplay::client_send_intent( "chat", 0, {}, msg );
    }
}

/* DDA MP v4.0a-188: common-NPC encounter menu. "Look" moves the client's look
 * cursor onto the NPC tile. Dialogue/trade/missions still live on the host and
 * need a dedicated network protocol. */
static void mp_run_npc_interact( const std::string &name, int dx, int dy )
{
    if( const char *dismiss = std::getenv( "DDA_MP_TEST_DISMISS_MENUS" );
        dismiss != nullptr && dismiss[0] != '\0' ) {
        mp_inv_log( string_format( "npc interact dismissed: %s %d,%d", name, dx, dy ) );
        return;
    }
    uilist menu;
    menu.text = string_format( netplay::fork_text( "NPC: %s" ), name );
    menu.addentry( netplay::fork_text( "Talk" ) );
    menu.addentry( netplay::fork_text( "Missions" ) );
    menu.addentry( netplay::fork_text( "Turn in" ) );
    menu.addentry( netplay::fork_text( "Look" ) );
    menu.addentry( netplay::fork_text( "Trade" ) );
    menu.addentry( netplay::fork_text( "Close" ) );
    mp_inv_log( string_format( "npc interact open: %s %d,%d", name, dx, dy ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    if( menu.ret == 0 ) {
        netplay::client_send_intent( "talk_npc", 0, { dx, dy } );
        mp_inv_log( string_format( "npc talk requested: %s %d,%d", name, dx, dy ) );
    } else if( menu.ret == 1 ) {
        mp_mission_dx = dx;
        mp_mission_dy = dy;
        netplay::client_send_intent( "mission_offers", 0, { dx, dy } );
        mp_inv_log( string_format( "npc missions requested: %s %d,%d", name, dx, dy ) );
    } else if( menu.ret == 2 ) {
        mp_mission_dx = dx;
        mp_mission_dy = dy;
        netplay::client_send_intent( "mission_turnin_list", 0, { dx, dy } );
        mp_inv_log( string_format( "npc turnin requested: %s %d,%d", name, dx, dy ) );
    } else if( menu.ret == 3 ) {
        mp_cursor = mp_cursor_mode::look;
        mp_cursor_dx = dx;
        mp_cursor_dy = dy;
    } else if( menu.ret == 4 ) {
        mp_trade_dx = dx;
        mp_trade_dy = dy;
        netplay::client_send_intent( "trade_list", 0, { dx, dy, 0 } );
        mp_inv_log( string_format( "npc trade requested: %s %d,%d", name, dx, dy ) );
    }
}

/* DDA MP v4.0a-189: one streamed dialogue step. The host sends the line and
 * numbered options; the chosen host response index goes back as an intent. */
static std::string mp_localize_field( const std::string &field );

static void mp_run_dialogue( const std::string &payload )
{
    const std::string::size_type first = payload.find( '\x1f' );
    std::string line = first == std::string::npos ? payload : payload.substr( 0, first );
    if( !line.empty() && line[0] == '\x0a' ) {
        const std::string::size_type sep = line.find( '\x0b', 1 );
        if( sep != std::string::npos ) {
            line = netplay::format_event_text(
                       std::string( "\x03" ) + line.substr( 1, sep - 1 ) + '\x1f' +
                       line.substr( sep + 1 ) );
        }
    }
    std::vector<std::pair<int, std::string>> options;
    if( first != std::string::npos ) {
        const std::string rest = payload.substr( first + 1 );
        std::string::size_type pos = 0;
        while( pos <= rest.size() ) {
            const std::string::size_type sep = rest.find( '\x1f', pos );
            const std::string item = rest.substr( pos, sep == std::string::npos
                                                  ? std::string::npos : sep - pos );
            const std::string::size_type us = item.find( '\x1e' );
            if( us != std::string::npos ) {
                options.emplace_back( std::atoi( item.substr( 0, us ).c_str() ),
                                      item.substr( us + 1 ) );
            }
            if( sep == std::string::npos ) {
                break;
            }
            pos = sep + 1;
        }
    }
    uilist menu;
    menu.text = line.empty() ? netplay::fork_text( "Conversation" ) : line;
    for( const auto &opt : options ) {
        const std::string label = mp_localize_field( opt.second );
        menu.addentry( label );
        mp_inv_log( "dialogue option: " + label );
    }
    mp_inv_log( string_format( "dialogue open: line=%d options=%d",
                               static_cast<int>( line.size() ),
                               static_cast<int>( options.size() ) ) );
    mp_inv_log( "dialogue line: " + line.substr( 0, 160 ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    int chosen = -1;
    if( menu.ret >= 0 && menu.ret < static_cast<int>( options.size() ) ) {
        chosen = options[menu.ret].first;
    }
    netplay::client_send_intent( "dialogue_choice", 0, { chosen } );
    mp_inv_log( string_format( "dialogue: line=%d options=%d chosen=%d",
                               static_cast<int>( line.size() ),
                               static_cast<int>( options.size() ), chosen ) );
}

/* DDA MP v4.0a-191: mission offer list from an NPC. Payload entries are
 * "<index>\x1e<name>"; accepting sends the host index back. */
static std::string mp_localize_field( const std::string &field );

static void mp_run_mission_offers( const std::string &payload )
{
    std::vector<std::pair<int, std::string>> offers;
    std::string::size_type pos = 0;
    while( pos <= payload.size() ) {
        const std::string::size_type sep = payload.find( '\x1f', pos );
        const std::string item = payload.substr( pos, sep == std::string::npos
                                                  ? std::string::npos : sep - pos );
        const std::string::size_type us = item.find( '\x1e' );
        if( us != std::string::npos ) {
            offers.emplace_back( std::atoi( item.substr( 0, us ).c_str() ),
                                 item.substr( us + 1 ) );
        }
        if( sep == std::string::npos ) {
            break;
        }
        pos = sep + 1;
    }
    if( offers.empty() ) {
        return;
    }
    for( const auto &offer : offers ) {
        mp_inv_log( "missions: offer name=" + mp_localize_field( offer.second ) );
    }
    // DDA MP test hook: accept the first offer without opening the menu.
    if( const char *auto_mission = std::getenv( "DDA_MP_TEST_MISSION_AUTO" );
        auto_mission != nullptr && auto_mission[0] != '\0' ) {
        static bool mp_test_mission_done = false;
        if( mp_test_mission_done ) {
            return;
        }
        mp_test_mission_done = true;
        netplay::client_send_intent(
            "mission_accept", 0,
            { mp_mission_dx, mp_mission_dy, offers.front().first } );
        // DDA MP v4.0a-217: remember when the test accepted, so the view loop
        // can request the turn-in list for the same NPC a few seconds later.
        mp_test_mission_accepted = std::chrono::steady_clock::now();
        mp_inv_log( string_format( "missions auto-accept: index=%d offers=%d",
                                   offers.front().first, static_cast<int>( offers.size() ) ) );
        return;
    }
    uilist menu;
    menu.text = netplay::fork_text( "Available missions" );
    for( const auto &offer : offers ) {
        menu.addentry( mp_localize_field( offer.second ) );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    mp_inv_log( string_format( "missions: offers=%d ret=%d",
                               static_cast<int>( offers.size() ), menu.ret ) );
    if( menu.ret >= 0 && menu.ret < static_cast<int>( offers.size() ) ) {
        netplay::client_send_intent(
            "mission_accept", 0,
            { mp_mission_dx, mp_mission_dy, offers[menu.ret].first } );
    }
}

/* DDA MP v4.0a-192: missions ready to turn in. Payload entries are
 * "<uid>\x1e<name>"; the uid identifies the host mission. */
/* DDA MP v4.0a-202: fields prefixed with \x05 carry an untranslated msgid
 * (game catalog), fields prefixed with \x06 carry an item type id. Resolve both
 * with the client's own language/data. */
static std::string mp_localize_field( const std::string &field )
{
    if( field.empty() ) {
        return field;
    }
    if( field[0] == '\x05' ) {
        const std::string msgid = field.substr( 1 );
        std::string out = to_translation( msgid ).translated();
        if( out == msgid ) {
            out = netplay::fork_text( msgid.c_str() );
        }
        return out;
    }
    if( field[0] == '\x06' ) {
        const itype_id id( field.substr( 1 ) );
        if( item::type_is_defined( id ) ) {
            return item::nname( id );
        }
        return field.substr( 1 );
    }
    if( field[0] == '\x07' || field[0] == '\x08' ) {
        const mission_type_id id( field.substr( 1 ) );
        const mission_type *type = id.is_valid() ? mission_type::get( id ) : nullptr;
        if( type != nullptr ) {
            return field[0] == '\x07' ? type->tname() : type->description.translated();
        }
        return field.substr( 1 );
    }
    return field;
}

static void mp_run_mission_turnins( const std::string &payload )
{
    std::vector<std::pair<int, std::string>> entries;
    std::string::size_type pos = 0;
    while( pos <= payload.size() ) {
        const std::string::size_type sep = payload.find( '\x1f', pos );
        const std::string item = payload.substr( pos, sep == std::string::npos
                                                  ? std::string::npos : sep - pos );
        const std::string::size_type us = item.find( '\x1e' );
        if( us != std::string::npos ) {
            entries.emplace_back( std::atoi( item.substr( 0, us ).c_str() ),
                                  item.substr( us + 1 ) );
        }
        if( sep == std::string::npos ) {
            break;
        }
        pos = sep + 1;
    }
    if( entries.empty() ) {
        return;
    }
    uilist menu;
    menu.text = netplay::fork_text( "Turn in" );
    for( const auto &e : entries ) {
        menu.addentry( mp_localize_field( e.second ) );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    // DDA MP test hook: pick the first entry without a menu (headless runs).
    int pick = -1;
    if( const char *auto_turnin = std::getenv( "DDA_MP_TEST_TURNIN_AUTO" );
        auto_turnin != nullptr && auto_turnin[0] != '\0' ) {
        pick = 0;
        mp_inv_log( "missions auto-turnin: index=0" );
    } else {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
        pick = menu.ret;
    }
    if( pick >= 0 && pick < static_cast<int>( entries.size() ) ) {
        netplay::client_send_intent(
            "mission_turnin", 0,
            { mp_mission_dx, mp_mission_dy, entries[pick].first } );
    }
    mp_inv_log( string_format( "missions: turnin=%d ret=%d",
                               static_cast<int>( entries.size() ), pick ) );
}

/* DDA MP v4.0a-191: read-only journal of missions assigned to this session. */
static void mp_run_mission_journal( const std::string &payload )
{
    struct journal_entry {
        std::string name;
        std::string status;
        std::string desc;
    };
    std::vector<journal_entry> entries;
    std::string::size_type pos = 0;
    while( pos <= payload.size() ) {
        const std::string::size_type sep = payload.find( '\x1f', pos );
        const std::string item = payload.substr( pos, sep == std::string::npos
                                                  ? std::string::npos : sep - pos );
        const std::string::size_type s1 = item.find( '\x1e' );
        const std::string::size_type s2 = s1 == std::string::npos
                                          ? std::string::npos : item.find( '\x1e', s1 + 1 );
        if( s2 != std::string::npos ) {
            entries.push_back( { mp_localize_field( item.substr( 0, s1 ) ),
                                 mp_localize_field( item.substr( s1 + 1, s2 - s1 - 1 ) ),
                                 mp_localize_field( item.substr( s2 + 1 ) ) } );
        }
        if( sep == std::string::npos ) {
            break;
        }
        pos = sep + 1;
    }
    if( entries.empty() ) {
        return;
    }
    uilist menu;
    menu.text = netplay::fork_text( "Mission journal" );
    for( const journal_entry &e : entries ) {
        menu.addentry( e.name + " (" + e.status + ")" );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    mp_inv_log( string_format( "missions: journal open entries=%d",
                               static_cast<int>( entries.size() ) ) );
    menu.query();
    if( menu.ret >= 0 && menu.ret < static_cast<int>( entries.size() ) ) {
        const journal_entry &e = entries[menu.ret];
        popup( e.status + "\n\n" + e.desc );
    }
    mp_inv_log( string_format( "missions: journal=%d ret=%d",
                               static_cast<int>( entries.size() ), menu.ret ) );
}

/* DDA MP v4.0a-193: custom barter window. Payload:
 * "<mode>\x1f<index>\x1e<name>\x1e<price>\x1f..."; mode 0 buys from the NPC,
 * mode 1 sells to it. */
static void mp_run_trade( const std::string &payload )
{
    const std::string::size_type first = payload.find( '\x1f' );
    const int mode = std::atoi( payload.substr( 0, first ).c_str() );
    int reward_credit = 0;
    int own_cash = 0;
    int trader_cash = 0;
    std::string::size_type pos = payload.size();
    if( first != std::string::npos ) {
        const std::string::size_type second = payload.find( '\x1f', first + 1 );
        reward_credit = std::atoi( payload.substr( first + 1,
                                 second == std::string::npos
                                 ? std::string::npos : second - first - 1 ).c_str() );
        pos = second == std::string::npos ? payload.size() : second + 1;
        if( second != std::string::npos ) {
            const std::string::size_type third = payload.find( '\x1f', second + 1 );
            own_cash = std::atoi( payload.substr( second + 1,
                                                  third == std::string::npos
                                                  ? std::string::npos : third - second - 1 ).c_str() );
            pos = third == std::string::npos ? payload.size() : third + 1;
            if( third != std::string::npos ) {
                const std::string::size_type fourth = payload.find( '\x1f', third + 1 );
                trader_cash = std::atoi( payload.substr( third + 1,
                                        fourth == std::string::npos
                                        ? std::string::npos : fourth - third - 1 ).c_str() );
                pos = fourth == std::string::npos ? payload.size() : fourth + 1;
            }
        }
    }
    struct trade_entry {
        int index = 0;
        std::string name;
        int price = 0;
        int count = 1;
        bool charge = false;
    };
    std::vector<trade_entry> entries;
    if( first != std::string::npos ) {
        while( pos <= payload.size() ) {
            const std::string::size_type sep = payload.find( '\x1f', pos );
            const std::string item = payload.substr( pos, sep == std::string::npos
                                                      ? std::string::npos : sep - pos );
            const std::string::size_type s1 = item.find( '\x1e' );
            const std::string::size_type s2 = s1 == std::string::npos
                                              ? std::string::npos : item.find( '\x1e', s1 + 1 );
            const std::string::size_type s3 = s2 == std::string::npos
                                              ? std::string::npos : item.find( '\x1e', s2 + 1 );
            const std::string::size_type s4 = s3 == std::string::npos
                                              ? std::string::npos : item.find( '\x1e', s3 + 1 );
            if( s4 != std::string::npos ) {
                entries.push_back( { std::atoi( item.substr( 0, s1 ).c_str() ),
                                     mp_localize_field( item.substr( s1 + 1, s2 - s1 - 1 ) ),
                                     std::atoi( item.substr( s2 + 1, s3 - s2 - 1 ).c_str() ),
                                     std::atoi( item.substr( s3 + 1, s4 - s3 - 1 ).c_str() ),
                                     item.substr( s4 + 1 ) == "1" } );
            }
            if( sep == std::string::npos ) {
                break;
            }
            pos = sep + 1;
        }
    }
    if( entries.empty() ) {
        return;
    }
    for( const trade_entry &e : entries ) {
        mp_inv_log( string_format( "trade entry: %s price=%d count=%d charge=%d",
                                   e.name, e.price, e.count, e.charge ? 1 : 0 ) );
    }
    // DDA MP test hook: perform one transaction without opening the menu so
    // the host-side transfer path can be checked automatically.
    if( const char *auto_trade = std::getenv( "DDA_MP_TEST_TRADE_AUTO" );
        auto_trade != nullptr && auto_trade[0] != '\0' ) {
        static bool mp_test_trade_done = false;
        if( mp_test_trade_done ) {
            return;
        }
        mp_test_trade_done = true;
        if( mode == 1 ) {
            const trade_entry *pick = &entries.front();
            for( const trade_entry &e : entries ) {
                if( e.price == 0 ) {
                    pick = &e;
                    break;
                }
            }
            netplay::client_send_intent( "trade_sell", 0,
                                         { mp_trade_dx, mp_trade_dy, pick->index, 1 } );
            mp_inv_log( string_format( "trade auto-sell: index=%d price=%d",
                                       pick->index, pick->price ) );
            return;
        }
        for( const trade_entry &e : entries ) {
            if( e.price == 0 ) {
                netplay::client_send_intent( "trade_buy", 0,
                                             { mp_trade_dx, mp_trade_dy, e.index, 1 } );
                mp_inv_log( string_format( "trade auto-buy: index=%d price=%d",
                                           e.index, e.price ) );
                return;
            }
        }
        mp_inv_log( "trade auto-buy: no zero-price entry" );
        return;
    }
    uilist menu;
    menu.text = mode == 1 ? netplay::fork_text( "Trade: sell" )
                : netplay::fork_text( "Trade: buy" );
    menu.text += "\n" + string_format( netplay::fork_text( "Money: %s   Trader: %s" ),
                                       format_money( own_cash ),
                                       format_money( trader_cash ) );
    if( reward_credit > 0 ) {
        menu.text += "  |  " + string_format(
                         netplay::fork_text( "Reward credit: %s" ),
                         format_money( reward_credit ) );
    }
    const int funds = own_cash + reward_credit;
    for( const trade_entry &e : entries ) {
        std::string label = e.name;
        if( e.charge && e.count > 1 ) {
            label += string_format( " x%d", e.count );
        }
        label += "  -  " + format_money( e.price );
        menu.addentry( label );
        // Charge stacks can be bought in smaller amounts, so only whole-item
        // prices are checked here; the host still validates the final deal.
        const bool too_expensive = !e.charge && e.price > 0 &&
                                   ( mode == 1 ? e.price > trader_cash : e.price > funds );
        if( too_expensive ) {
            menu.entries.back().enabled = false;
            menu.entries.back().text_color = c_dark_gray;
        }
    }
    menu.addentry( mode == 1 ? netplay::fork_text( "Buy" ) : netplay::fork_text( "Sell" ) );
    menu.addentry( netplay::fork_text( "Close" ) );
    mp_inv_log( string_format( "trade open: mode=%d entries=%d own=%d trader=%d credit=%d",
                               mode, static_cast<int>( entries.size() ),
                               own_cash, trader_cash, reward_credit ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    const int n = static_cast<int>( entries.size() );
    if( menu.ret >= 0 && menu.ret < n ) {
        const trade_entry &e = entries[menu.ret];
        int amount = 1;
        if( e.charge && e.count > 1 ) {
            std::vector<int> qty;
            for( const int q : { 1, 5, 10, e.count } ) {
                if( q >= 1 && q <= e.count &&
                    std::find( qty.begin(), qty.end(), q ) == qty.end() ) {
                    qty.push_back( q );
                }
            }
            uilist qmenu;
            qmenu.text = netplay::fork_text( "Quantity" );
            for( const int q : qty ) {
                qmenu.addentry( string_format( "%d", q ) );
            }
            qmenu.addentry( netplay::fork_text( "Close" ) );
            qmenu.query();
            if( qmenu.ret < 0 || qmenu.ret >= static_cast<int>( qty.size() ) ) {
                mp_inv_log( "trade: quantity cancelled" );
                return;
            }
            amount = qty[qmenu.ret];
        }
        netplay::client_send_intent( mode == 1 ? "trade_sell" : "trade_buy", 0,
                                     { mp_trade_dx, mp_trade_dy, e.index, amount } );
    } else if( menu.ret == n ) {
        netplay::client_send_intent( "trade_list", 0,
                                     { mp_trade_dx, mp_trade_dy, mode == 1 ? 0 : 1 } );
    }
    mp_inv_log( string_format( "trade: mode=%d entries=%d ret=%d", mode, n, menu.ret ) );
}

/* DDA MP v4.0a-196: read-only bionic list built from the avatar mirror. The
 * engine's own bionics screen activates CBMs through the host avatar in a few
 * paths (ranged bionics, water extractor queries), so it is not mirrored: the
 * client only shows what is installed and its state. */
static void mp_run_bionics()
{
    if( !mp_native_avatar_ready ) {
        mp_inv_log( "ui: status mirror not ready" );
        return;
    }
    avatar &you = get_avatar();
    uilist menu;
    menu.desc_enabled = true;
    menu.title = netplay::fork_text( "Bionics" );
    menu.text = string_format( netplay::fork_text( "Bionic power: %s" ),
                               units::display( you.get_power_level() ) );
    int rows = 0;
    for( const bionic &bio : *you.my_bionics ) {
        std::string label = bio.info().name.translated();
        if( !bio.info().activated ) {
            label += "  (" + netplay::fork_text( "passive" ) + ")";
        } else if( bio.powered ) {
            label += "  (" + netplay::fork_text( "active" ) + ")";
        } else {
            label += "  (" + netplay::fork_text( "ready" ) + ")";
        }
        menu.addentry_desc( static_cast<int>( menu.entries.size() ), false, MENU_AUTOASSIGN,
                            label, bio.info().description.translated() );
        menu.entries.back().text_color = bio.powered ? c_light_green
                                        : bio.info().activated ? c_light_gray : c_dark_gray;
        ++rows;
    }
    if( rows == 0 ) {
        menu.text += "\n" + netplay::fork_text( "No bionics installed." );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    mp_inv_log( string_format( "ui: bionics open entries=%d", rows ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    mp_inv_log( "ui: bionics closed" );
}

/* DDA MP v4.0a-196: read-only mutation list from the mirror. Activated
 * mutations are marked, but toggling them stays on the host: some activations
 * run effect-on-conditions against the host player. */
static void mp_run_mutations()
{
    if( !mp_native_avatar_ready ) {
        mp_inv_log( "ui: status mirror not ready" );
        return;
    }
    avatar &you = get_avatar();
    uilist menu;
    menu.desc_enabled = true;
    menu.title = netplay::fork_text( "Mutations" );
    int rows = 0;
    for( const auto &mut : you.my_mutations ) {
        if( mut.second.corrupted > 0 || !mut.first->player_display ) {
            continue;
        }
        std::string label = you.mutation_name( mut.first );
        if( mut.first->activated ) {
            label += mut.second.powered ? "  (" + netplay::fork_text( "active" ) + ")"
                     : "  (" + netplay::fork_text( "ready" ) + ")";
        } else {
            label += "  (" + netplay::fork_text( "passive" ) + ")";
        }
        menu.addentry_desc( static_cast<int>( menu.entries.size() ), false, MENU_AUTOASSIGN,
                            label, you.mutation_desc( mut.first ) );
        menu.entries.back().text_color = mut.second.powered ? c_light_green
                                        : mut.first->activated ? c_light_gray : c_dark_gray;
        ++rows;
    }
    if( rows == 0 ) {
        menu.text = netplay::fork_text( "No mutations." );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    mp_inv_log( string_format( "ui: mutations open entries=%d", rows ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    mp_inv_log( "ui: mutations closed" );
}

/* DDA MP v4.0a-210: spellbook on the mirror; the chosen spell enters the
 * normal netplay cursor so the client can pick a target tile. */
static void mp_run_spellbook( bool recast )
{
    if( !mp_native_avatar_ready ) {
        mp_inv_log( "ui: status mirror not ready" );
        return;
    }
    if( recast ) {
        if( mp_last_spell.empty() ) {
            mp_inv_log( "spell: no previous spell" );
            return;
        }
        netplay::client_send_intent( "cast_spell", 0,
                                     { mp_last_spell_dx, mp_last_spell_dy },
                                     mp_last_spell );
        mp_inv_log( "action: cast_spell recast " + mp_last_spell );
        return;
    }
    avatar &you = get_avatar();
    std::vector<spell_id> ids;
    uilist menu;
    menu.title = netplay::fork_text( "Spells" );
    for( const auto &entry : you.magic->get_spellbook() ) {
        if( !entry.second.is_valid() ) {
            continue;
        }
        ids.push_back( entry.first );
        menu.addentry( entry.second.name() + "  -  " +
                       entry.second.energy_cost_string( you ) );
    }
    if( ids.empty() ) {
        menu.addentry( netplay::fork_text( "No spells." ) );
        menu.entries.back().enabled = false;
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    if( menu.ret < 0 || menu.ret >= static_cast<int>( ids.size() ) ) {
        return;
    }
    mp_pending_spell = ids[menu.ret].str();
    mp_cursor = mp_cursor_mode::spell;
    mp_cursor_dx = 0;
    mp_cursor_dy = 0;
    mp_inv_log( "spell: select target for " + mp_pending_spell );
}

/* DDA MP v4.0a-210: confirm and ask the host to kill the session character. */
static void mp_run_suicide()
{
    if( !query_yn( netplay::fork_text( "Abandon this character?" ) ) ) {
        return;
    }
    if( !query_yn( netplay::fork_text( "This will kill your character. Continue?" ) ) ) {
        return;
    }
    netplay::client_send_intent( "suicide", 0, {} );
    mp_inv_log( "action: suicide" );
}

/* DDA MP v4.0a-209: movement-mode picker; the choice is applied on the host. */
static void mp_run_movement_mode()
{
    struct mode_row {
        const char *id;
        const char *label;
    };
    static const mode_row modes[] = {
        { "walk", "Walk" }, { "run", "Run" }, { "crouch", "Crouch" }, { "prone", "Prone" }
    };
    avatar &you = get_avatar();
    uilist menu;
    menu.title = netplay::fork_text( "Movement mode" );
    for( const mode_row &mode : modes ) {
        menu.addentry( netplay::fork_text( mode.label ) );
        if( you.current_movement_mode() == move_mode_id( mode.id ) ) {
            menu.entries.back().text_color = c_light_green;
        }
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    if( menu.ret >= 0 && menu.ret < 4 ) {
        netplay::client_send_intent( "movement_mode", 0, {}, modes[menu.ret].id );
        mp_inv_log( string_format( "action: movement_mode %s", modes[menu.ret].id ) );
    }
}

/* DDA MP v4.0a-212: run an inventory command on the wielded item. The client
 * only knows the mirror's uid, so the host resolves the real item. */
static bool mp_wielded_uid_command( int key, const char *what )
{
    if( !mp_native_avatar_ready ) {
        mp_inv_log( "ui: status mirror not ready" );
        return false;
    }
    const item_location wielded = get_avatar().get_wielded_item();
    if( !wielded ) {
        mp_inv_log( std::string( "action: " ) + what + " without a wielded item" );
        return false;
    }
    const std::string uid = wielded->get_var( "mp_uid", std::string() );
    if( uid.empty() ) {
        mp_inv_log( std::string( "action: " ) + what + " item has no uid" );
        return false;
    }
    netplay::client_send_inventory_uid( uid, key );
    mp_inv_log( std::string( "action: " ) + what + " uid key=" + std::to_string( key ) );
    return true;
}

/* DDA MP v4.0a-210: armor sorting runs on the mirror; the resulting worn order
 * is sent to the host as stable item uids. */
static void mp_run_sort_armor()
{
    if( !mp_native_avatar_ready ) {
        mp_inv_log( "ui: status mirror not ready" );
        return;
    }
    avatar &you = get_avatar();
    you.worn.sort_armor( you );
    std::vector<item *> items;
    you.worn.inv_dump( items );
    std::string uids;
    for( item *it : items ) {
        if( it == nullptr ) {
            continue;
        }
        const std::string uid = it->get_var( "mp_uid", std::string() );
        if( uid.empty() ) {
            mp_inv_log( "action: sort_armor aborted, missing uid" );
            return;
        }
        if( !uids.empty() ) {
            uids += ',';
        }
        uids += uid;
    }
    if( !uids.empty() ) {
        netplay::client_send_intent( "sort_armor", 0, {}, uids );
    }
    mp_inv_log( string_format( "action: sort_armor items=%d",
                               static_cast<int>( items.size() ) ) );
}

/* DDA MP v4.0a-210: directional drop. Pick a direction, then a multidrop set
 * on the mirror; each item is dropped at the target by the host. */
static void mp_run_dir_drop()
{
    static const int dx[] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    static const int dy[] = { -1, 1, 0, 0, -1, -1, 1, 1 };
    static const char *const names[] = {
        "North", "South", "East", "West", "Northeast", "Northwest",
        "Southeast", "Southwest"
    };
    uilist menu;
    menu.title = netplay::fork_text( "Drop where?" );
    for( const char *name : names ) {
        menu.addentry( netplay::fork_text( name ) );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    if( menu.ret < 0 || menu.ret >= 8 ) {
        return;
    }
    const drop_locations picked = game_menus::inv::multidrop( get_avatar() );
    int sent = 0;
    for( const drop_location &dl : picked ) {
        if( !dl.first ) {
            continue;
        }
        const std::string uid = dl.first->get_var( "mp_uid", std::string() );
        if( !uid.empty() ) {
            netplay::client_send_inventory_uid( uid, 'F', {}, { dx[menu.ret], dy[menu.ret] } );
            ++sent;
        }
    }
    mp_inv_log( string_format( "action: dir_drop %d,%d items=%d",
                               dx[menu.ret], dy[menu.ret], sent ) );
}

static void mp_run_grab()
{
    static const int dx[] = { 0, 0, 1, -1, 1, -1, 1, -1 };
    static const int dy[] = { -1, 1, 0, 0, -1, -1, 1, 1 };
    static const char *const names[] = {
        "North", "South", "East", "West", "Northeast", "Northwest",
        "Southeast", "Southwest"
    };
    uilist menu;
    menu.title = netplay::fork_text( "Grab" );
    for( const char *name : names ) {
        menu.addentry( netplay::fork_text( name ) );
    }
    menu.addentry( netplay::fork_text( "Release" ) );
    menu.addentry( netplay::fork_text( "Close" ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    if( menu.ret >= 0 && menu.ret < 8 ) {
        netplay::client_send_intent( "grab", 0, { dx[menu.ret], dy[menu.ret] } );
        mp_inv_log( string_format( "action: grab %d,%d", dx[menu.ret], dy[menu.ret] ) );
    } else if( menu.ret == 8 ) {
        netplay::client_send_intent( "grab_release", 0, {} );
        mp_inv_log( "action: grab_release" );
    }
}


static void mp_run_haul()
{
    uilist menu;
    menu.text = netplay::fork_text( "Hauling" );
    menu.addentry( netplay::fork_text( "Haul everything here" ) );
    menu.addentry( netplay::fork_text( "Stop hauling" ) );
    menu.addentry( netplay::fork_text( "Enable autohaul" ) );
    menu.addentry( netplay::fork_text( "Disable autohaul" ) );
    menu.addentry( netplay::fork_text( "Close" ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    static const char *const orders[] = { "haul_here", "haul_stop",
                                          "autohaul_on", "autohaul_off"
                                        };
    if( menu.ret >= 0 &&
        menu.ret < static_cast<int>( sizeof( orders ) / sizeof( orders[0] ) ) ) {
        netplay::client_send_intent( orders[menu.ret], 0, {} );
        mp_inv_log( string_format( "action: %s", orders[menu.ret] ) );
    }
}

/* DDA MP v4.0a-198: read-only vehicle status window. The host sends the
 * numbers and fuel ids; the client resolves names from its own data. */
static void mp_run_vehicle_info( const std::string &payload )
{
    const std::string::size_type split = payload.find( '\x1f' );
    std::string header = payload.substr( 0, split );
    std::vector<std::string> fields;
    std::string::size_type pos = 0;
    while( pos <= header.size() ) {
        const std::string::size_type sep = header.find( '\x1e', pos );
        fields.push_back( header.substr( pos, sep == std::string::npos
                                        ? std::string::npos : sep - pos ) );
        if( sep == std::string::npos ) {
            break;
        }
        pos = sep + 1;
    }
    if( fields.size() < 8 ) {
        return;
    }
    uilist menu;
    menu.title = netplay::fork_text( "Vehicle" );
    menu.text = fields[0];
    menu.text += "\n" + string_format( netplay::fork_text( "Speed: %d mph   Cruise: %d mph" ),
                                       std::atoi( fields[1].c_str() ),
                                       std::atoi( fields[2].c_str() ) );
    menu.text += "\n" + ( std::atoi( fields[3].c_str() ) != 0
                          ? netplay::fork_text( "Engine: on" )
                          : netplay::fork_text( "Engine: off" ) );
    menu.text += "\n" + string_format( netplay::fork_text( "Battery: %d/%d kJ" ),
                                       std::atoi( fields[4].c_str() ),
                                       std::atoi( fields[5].c_str() ) );
    menu.text += "\n" + string_format( netplay::fork_text( "Parts: %d   Damaged: %d" ),
                                       std::atoi( fields[6].c_str() ),
                                       std::atoi( fields[7].c_str() ) );
    if( split != std::string::npos ) {
        std::string::size_type fuel_pos = split + 1;
        while( fuel_pos <= payload.size() ) {
            const std::string::size_type sep = payload.find( '\x1f', fuel_pos );
            const std::string fuel = payload.substr( fuel_pos, sep == std::string::npos
                                                    ? std::string::npos : sep - fuel_pos );
            const std::string::size_type us = fuel.find( '\x1e' );
            if( us != std::string::npos && us > 0 ) {
                const itype_id fid( fuel.substr( 0, us ) );
                menu.text += "\n" + string_format( netplay::fork_text( "Fuel: %s %d" ),
                                                   item::nname( fid ),
                                                   std::atoi( fuel.substr( us + 1 ).c_str() ) );
            }
            if( sep == std::string::npos ) {
                break;
            }
            fuel_pos = sep + 1;
        }
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    mp_inv_log( string_format( "ui: vehicle info open parts=%s", fields[6].c_str() ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    mp_inv_log( "ui: vehicle info closed" );
}

/* DDA MP v4.0a-207: read-only zone list streamed from the host. */
static void mp_run_zones( const std::string &payload )
{
    std::vector<std::string> rows;
    std::string::size_type pos = 0;
    while( pos <= payload.size() ) {
        const std::string::size_type sep = payload.find( '\x1f', pos );
        const std::string item = payload.substr( pos, sep == std::string::npos
                                                  ? std::string::npos : sep - pos );
        std::vector<std::string> fields;
        std::string::size_type fpos = 0;
        while( fpos <= item.size() ) {
            const std::string::size_type fsep = item.find( '\x1e', fpos );
            fields.push_back( item.substr( fpos, fsep == std::string::npos
                                           ? std::string::npos : fsep - fpos ) );
            if( fsep == std::string::npos ) {
                break;
            }
            fpos = fsep + 1;
        }
        if( fields.size() >= 5 ) {
            const zone_type_id type( fields[1] );
            const std::string type_name = type.is_valid() ? type.obj().name() : fields[1];
            rows.push_back( fields[0] + "  -  " + type_name +
                            string_format( " (%s,%s,%s)", fields[2], fields[3], fields[4] ) );
        }
        if( sep == std::string::npos ) {
            break;
        }
        pos = sep + 1;
    }
    if( rows.empty() ) {
        return;
    }
    uilist menu;
    menu.title = netplay::fork_text( "Zones" );
    for( const std::string &row : rows ) {
        menu.addentry( row );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    mp_inv_log( string_format( "ui: zones open entries=%d",
                               static_cast<int>( rows.size() ) ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    mp_inv_log( "ui: zones closed" );
}


/* DDA MP v4.0a-204: damaged vehicle parts; selecting one starts the engine's
 * repair activity on the host. */
static void mp_run_vehicle_parts( const std::string &payload )
{
    struct part_row {
        int index = 0;
        std::string name;
        int damage = 0;
        bool repairable = true;
    };
    std::vector<part_row> rows;
    std::string::size_type pos = 0;
    while( pos <= payload.size() ) {
        const std::string::size_type sep = payload.find( '\x1f', pos );
        const std::string item = payload.substr( pos, sep == std::string::npos
                                                  ? std::string::npos : sep - pos );
        std::vector<std::string> fields;
        std::string::size_type fpos = 0;
        while( fpos <= item.size() ) {
            const std::string::size_type fsep = item.find( '\x1e', fpos );
            fields.push_back( item.substr( fpos, fsep == std::string::npos
                                           ? std::string::npos : fsep - fpos ) );
            if( fsep == std::string::npos ) {
                break;
            }
            fpos = fsep + 1;
        }
        if( fields.size() >= 4 ) {
            const vpart_id id( fields[1] );
            rows.push_back( { std::atoi( fields[0].c_str() ),
                              id.is_valid() ? id.obj().name() : fields[1],
                              std::atoi( fields[2].c_str() ),
                              fields[3] != "0" } );
        }
        if( sep == std::string::npos ) {
            break;
        }
        pos = sep + 1;
    }
    if( rows.empty() ) {
        return;
    }
    uilist menu;
    menu.title = netplay::fork_text( "Vehicle parts" );
    menu.text = netplay::fork_text( "Select a damaged part to repair." );
    for( const part_row &row : rows ) {
        const std::string label = string_format(
                                      netplay::fork_text( "%s (damage %d)" ),
                                      row.name, row.damage );
        menu.addentry( label );
        if( !row.repairable ) {
            menu.entries.back().enabled = false;
            menu.entries.back().text_color = c_dark_gray;
        }
        mp_inv_log( "vehicle part: " + label );
    }
    menu.addentry( netplay::fork_text( "Close" ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    if( menu.ret >= 0 && menu.ret < static_cast<int>( rows.size() ) ) {
        netplay::client_send_intent( "vehicle_repair", 0, { rows[menu.ret].index } );
        mp_inv_log( string_format( "action: vehicle_repair index=%d",
                                   rows[menu.ret].index ) );
    }
}


/* DDA MP v4.0a-199: training intensity/duration chosen on the client; the host
 * runs the engine workout actor without its own menus. */
static void mp_run_workout()
{
    uilist menu;
    menu.title = netplay::fork_text( "Workout" );
    menu.text = netplay::fork_text( "Choose training intensity:" );
    menu.addentry( netplay::fork_text( "Light" ) );
    menu.addentry( netplay::fork_text( "Moderate" ) );
    menu.addentry( netplay::fork_text( "Active" ) );
    menu.addentry( netplay::fork_text( "High" ) );
    menu.addentry( netplay::fork_text( "Close" ) );
    {
        ui_adaptor blocker( ui_adaptor::disable_uis_below{} );
        menu.query();
    }
    if( menu.ret < 0 || menu.ret > 3 ) {
        return;
    }
    int minutes = 0;
    if( !query_int( minutes, false,
                    netplay::fork_text( "Train for how long (minutes): " ) ) ||
        minutes <= 0 ) {
        return;
    }
    netplay::client_send_intent( "workout", 0, { menu.ret + 1, minutes } );
    mp_inv_log( string_format( "action: workout intensity=%d minutes=%d",
                               menu.ret + 1, minutes ) );
}

/* DDA MP v4.0a-80: runs one action id chosen in the client's action menu.
 * Local UIs run on the client; every other order travels to the host. */
static void mp_client_dispatch_action( int act, const std::string &inv_text,
                                       const std::string &char_json )
{
    if( act <= 0 ) {
        return;
    }
    if( act == MP_ACTION_HELP_NEARBY ) {
        // DDA MP v4.0a-174/175: the host finds the busy player next to us; a
        // second press asks for confirmation first, like cancelling an activity.
        if( mp_client_helping &&
            !query_yn( netplay::fork_text( "Stop helping?" ) ) ) {
            return;
        }
        netplay::client_send_intent( "help", 0, {} );
        mp_client_helping = !mp_client_helping;
        return;
    }
    if( act == MP_ACTION_VEHICLE_INFO ) {
        netplay::client_send_intent( "vehicle_info", 0, {} );
        mp_inv_log( "action: vehicle_info" );
        return;
    }
    if( act == MP_ACTION_VEHICLE_REFUEL ) {
        netplay::client_send_intent( "vehicle_refuel", 0, {} );
        mp_inv_log( "action: vehicle_refuel" );
        return;
    }
    if( act == MP_ACTION_VEHICLE_PARTS ) {
        netplay::client_send_intent( "vehicle_parts", 0, {} );
        mp_inv_log( "action: vehicle_parts" );
        return;
    }
    switch( static_cast<action_id>( act ) ) {
        case ACTION_INVENTORY:
        case ACTION_ADVANCEDINV:
            // DDA MP v4.1: the inventory comes from the mirror, not from a
            // streamed listing, so no packet has to be present to open it.
            mp_run_host_inventory( std::string() );
            break;
        case ACTION_PL_INFO:
            mp_run_character_info( char_json );
            break;
        case ACTION_MORALE:
            // DDA MP v4.0a-182: read-only morale breakdown from the avatar
            // mirror; no host state is touched.
            if( !mp_native_avatar_ready ) {
                mp_inv_log( "ui: status mirror not ready" );
                break;
            }
            mp_inv_log( "ui: morale open" );
            get_avatar().disp_morale();
            mp_inv_log( "ui: morale closed" );
            break;
        case ACTION_BODYSTATUS:
            // Read-only bodypart graph from the mirror (health, effects).
            if( !mp_native_avatar_ready ) {
                mp_inv_log( "ui: status mirror not ready" );
                break;
            }
            mp_inv_log( "ui: bodygraph open" );
            display_bodygraph( get_avatar() );
            mp_inv_log( "ui: bodygraph closed" );
            break;
        case ACTION_MEDICAL:
            // DDA MP v4.0a-183: the medical window is shown read-only; the
            // treatment actions belong to the host and would only change the
            // local mirror.
            if( !mp_native_avatar_ready ) {
                mp_inv_log( "ui: status mirror not ready" );
                break;
            }
            mp_inv_log( "ui: medical open (read-only)" );
            get_avatar().disp_medical( true );
            mp_inv_log( "ui: medical closed" );
            break;
        case ACTION_FACTIONS:
            // DDA MP v4.0a-186: the faction list is global game data and the
            // mirror carries the reputation; the screen is read-only.
            if( mp_native_avatar_ready ) {
                mp_inv_log( "ui: factions open" );
                g->faction_manager_ptr->display();
                mp_inv_log( "ui: factions closed" );
            }
            break;
        case ACTION_SKY:
            // DDA MP v4.0a-186: time/weather are streamed with the snapshots,
            // so the sky window can run locally.
            if( mp_native_avatar_ready ) {
                mp_inv_log( "ui: sky open" );
                ui::omap::display_visible_weather();
                mp_inv_log( "ui: sky closed" );
            }
            break;
        case ACTION_MAP:
            // DDA MP v4.0a-186: full-screen view of the streamed overmap
            // minimap block (the host does not send its whole explored map).
            if( mp_native_avatar_ready ) {
                mp_map_open = true;
                mp_inv_log( "ui: map open" );
            }
            break;
        case ACTION_ZONES:
            // DDA MP v4.0a-207: read-only zone list.
            netplay::client_send_intent( "zones", 0, {} );
            mp_inv_log( "action: zones" );
            break;
        case ACTION_LOOK:
        case ACTION_PEEK:
        case ACTION_LIST_ITEMS:
            // DDA MP v4.0a-150/181: the stock look_around() waits on a game
            // window the netplay view loop does not present, so it only froze
            // input. Use the netplay cursor; peek/list use it too.
            mp_cursor = mp_cursor_mode::look;
            mp_cursor_dx = 0;
            mp_cursor_dy = 0;
            break;
        case ACTION_EXAMINE:
            // DDA MP v4.0a-181: plain examine triggers the terrain/furniture/
            // trap action on the host. It used to open the look cursor, which
            // made the host's examine implementation unreachable from the
            // action menu.
            netplay::client_send_client_action( act );
            break;

        case ACTION_CRAFT:
            mp_run_client_craft( false );
            break;
        case ACTION_LONGCRAFT:
            mp_run_client_craft( true );
            break;
        case ACTION_RECRAFT:
            netplay::client_send_intent( "recraft", 0, {}, std::string() );
            break;
        case ACTION_FIRE:
        case ACTION_FIRE_BURST:
            mp_begin_client_fire();
            break;
        case ACTION_THROW:
            mp_run_throw_select( inv_text );
            break;
        case ACTION_THROW_WIELDED: {
            const item_location wielded = get_avatar().get_wielded_item();
            if( wielded ) {
                const std::string uid = wielded->get_var( "mp_uid", std::string() );
                if( !uid.empty() ) {
                    mp_start_client_throw( *wielded, uid );
                }
            }
            break;
        }
        case ACTION_CHAT:
            mp_run_client_chat();
            break;
        case ACTION_COMPARE:
            // DDA MP v4.0a-185: item comparison is read-only; run it on the
            // mirror instead of opening the generic inventory.
            if( mp_native_avatar_ready ) {
                game_menus::inv::compare( std::nullopt );
            }
            break;
        case ACTION_MISSIONS:
            // DDA MP v4.0a-191: the host streams this session's mission list.
            netplay::client_send_intent( "mission_journal", 0, {} );
            mp_inv_log( "missions: journal requested" );
            break;
        case ACTION_DIARY:
            // DDA MP v4.0a-210: session-local diary on the client mirror.
            if( mp_native_avatar_ready ) {
                if( !mp_session_diary ) {
                    mp_session_diary = std::make_unique<diary>();
                }
                mp_inv_log( "ui: diary open" );
                diary::show_diary_ui( mp_session_diary.get() );
                mp_inv_log( "ui: diary closed" );
            }
            break;
        case ACTION_BIONICS:
            // DDA MP v4.0a-196: read-only bionic list on the mirror.
            mp_run_bionics();
            break;
        case ACTION_MUTATIONS:
            // DDA MP v4.0a-196: read-only mutation list on the mirror.
            mp_run_mutations();
            break;
        case ACTION_WORKOUT:
            // DDA MP v4.0a-199: client picks intensity/duration, host trains.
            mp_run_workout();
            break;
        case ACTION_SUICIDE:
            mp_run_suicide();
            break;
        case ACTION_HAUL:
            // DDA MP v4.0a-196: haul orders run on the host for this avatar.
            mp_run_haul();
            break;
        case ACTION_GRAB:
            // DDA MP v4.0a-205: pick a direction, host grabs/releases.
            mp_run_grab();
            break;
        case ACTION_HAUL_TOGGLE:
            netplay::client_send_intent( "haul_toggle", 0, {} );
            mp_inv_log( "action: haul_toggle" );
            break;
        case ACTION_PICK_STYLE:
            // DDA MP v4.0a-187: pick the style on the mirror, then tell the
            // host to apply it to the session avatar.
            if( mp_native_avatar_ready ) {
                avatar &you = get_avatar();
                mp_inv_log( "ui: style open" );
                if( you.martial_arts_data->pick_style( you ) ) {
                    const matype_id style = you.martial_arts_data->selected_style();
                    if( !style.is_null() ) {
                        netplay::client_send_intent( "pick_style", 0, {}, style.str() );
                        mp_inv_log( "action: pick_style " + style.str() );
                    }
                }
            }
            break;
        case ACTION_SELECT_DEFAULT_AMMO:
            // DDA MP v4.0a-187: choose the ammo from the mirror inventory and
            // send its stable uid; the host stores the preference on its item.
            if( mp_native_avatar_ready ) {
                avatar &you = get_avatar();
                item_location weapon = you.get_wielded_item();
                if( weapon && weapon->is_gun() ) {
                    item::reload_option opt = you.select_ammo( weapon, false );
                    if( opt ) {
                        const std::string uid = opt.ammo->get_var( "mp_uid", std::string() );
                        if( !uid.empty() ) {
                            netplay::client_send_intent( "default_ammo", 0, {}, uid );
                            mp_inv_log( "action: default_ammo " + uid );
                        }
                    }
                }
            }
            break;
        // DDA MP v4.0a-133/181: the item-action hotkeys open the inventory; the
        // row menu exposes drop/eat/wear/read/wield/use/reload/disassemble for
        // the chosen item. The same route serves the action-menu entries that
        // have no separate UI on the host.
        case ACTION_OPEN_MOVEMENT:
            mp_run_movement_mode();
            break;
        case ACTION_DIR_DROP:
            mp_run_dir_drop();
            break;
        case ACTION_SORT_ARMOR:
            mp_run_sort_armor();
            break;
        case ACTION_DROP:
        case ACTION_EAT:
        case ACTION_WEAR:
        case ACTION_TAKE_OFF:
        case ACTION_READ:
        case ACTION_WIELD:
        case ACTION_USE:
        case ACTION_RELOAD_ITEM:
        case ACTION_RELOAD_WEAPON:
        case ACTION_RELOAD_WIELDED:
        case ACTION_DISASSEMBLE:
        case ACTION_INSERT_ITEM:
        case ACTION_UNLOAD_CONTAINER:
        case ACTION_OPEN_CONSUME:
        case ACTION_UNLOAD:
        case ACTION_ORGANIZE:
        case ACTION_ITEMACTION:
            mp_run_host_inventory( inv_text );
            break;
        case ACTION_CAST_SPELL:
            mp_run_spellbook( false );
            break;
        case ACTION_RECAST_SPELL:
            mp_run_spellbook( true );
            break;
        case ACTION_USE_WIELDED:
            // DDA MP v4.0a-212: same host path as the inventory "apply" key.
            mp_wielded_uid_command( 'a', "use_wielded" );
            break;
        case ACTION_MEND:
            // DDA MP v4.0a-212: mend the wielded item through the host.
            mp_wielded_uid_command( 'm', "mend" );
            break;
        case ACTION_AUTOATTACK:
            // DDA MP v4.0a-212: the host picks and attacks a target.
            netplay::client_send_intent( "autoattack", 0, {} );
            mp_inv_log( "action: autoattack" );
            break;

        case ACTION_ZOOM_IN:
            g->zoom_in();
            g->mark_main_ui_adaptor_resize();
            break;
        case ACTION_ZOOM_OUT:
            g->zoom_out();
            g->mark_main_ui_adaptor_resize();
            break;

        case ACTION_HELP: {
            ui_adaptor mp_ui_blocker( ui_adaptor::disable_uis_below{} );
            help_window hw;
            hw.show();
            break;
        }
        case ACTION_MESSAGES:
            // DDA MP v4.0a-170: the journal is mirrored into the game's message
            // list on the client, so show it here instead of messaging the host
            // (which used to open the HOST's log).
            Messages::display_messages();
            break;
        case ACTION_BUTCHER:
            // DDA MP v4.0a-171: the host butchers the corpse on the avatar's
            // tile (dx/dy are reserved for cursor targeting later).
            netplay::client_send_intent( "butcher", 0, { 0, 0 } );
            break;
        case ACTION_CONSTRUCT:
            // DDA MP v4.0a-171: pick the construction and the direction on the
            // client; the host validates and starts it.
            mp_run_client_construct();
            break;
        default:
            netplay::client_send_client_action( act );
            break;
    }
}

/* DDA MP v4.0a-72: the client's own action menu. Same categories and entries
 * as the host's handle_action_menu, minus the save/load/debug entries that
 * cannot apply to a remote session. The host owns the contextual weighting
 * (nearby doors, wielded gun, ...), so the client lists the vanilla order.
 * The chosen action id travels to the host, which applies it to the session
 * avatar exactly like a host keypress would apply it to the host player. */
static int mp_client_action_menu()
{
    const input_context ctxt = get_default_mode_input_context();
    std::string category = "back";

#define MP_REGISTER_ACTION( name ) entries.emplace_back( name, true, \
        hotkey_for_action( name, /*maximum_modifier_count=*/1 ), \
        ctxt.get_action_name( action_ident( name ) ) );
#define MP_REGISTER_CATEGORY( text ) categories_by_int[last_category] = text; \
    catgname = text; \
    catgname += "\xE2\x80\xA6"; \
    entries.emplace_back( last_category, true, -1, catgname ); \
    last_category++;

    while( true ) {
        std::vector<uilist_entry> entries;
        std::map<int, std::string> categories_by_int;
        int last_category = NUM_ACTIONS + 1;
        std::string catgname;

        if( category == "back" ) {
            MP_REGISTER_CATEGORY( _( "Look" ) );
            MP_REGISTER_CATEGORY( _( "Interact" ) );
            MP_REGISTER_CATEGORY( _( "Inventory" ) );
            MP_REGISTER_CATEGORY( _( "Combat" ) );
            MP_REGISTER_CATEGORY( _( "Craft" ) );
            MP_REGISTER_CATEGORY( _( "Info" ) );
            MP_REGISTER_CATEGORY( _( "Misc" ) );
            MP_REGISTER_ACTION( ACTION_HELP );
            if( !entries.empty() ) {
                entries.back().txt += "\xE2\x80\xA6"; // help _is_a menu
            }
        } else if( category == _( "Look" ) ) {
            MP_REGISTER_ACTION( ACTION_LOOK );
            MP_REGISTER_ACTION( ACTION_PEEK );
            MP_REGISTER_ACTION( ACTION_LIST_ITEMS );
            // DDA MP v4.0a-186: sky/time and the local overmap view run from
            // streamed data. Zones still need host-side zone data; hide.
            MP_REGISTER_ACTION( ACTION_SKY );
            MP_REGISTER_ACTION( ACTION_MAP );
            // DDA MP v4.0a-207: read-only zone list from the host.
            MP_REGISTER_ACTION( ACTION_ZONES );
        } else if( category == _( "Interact" ) ) {
            MP_REGISTER_ACTION( ACTION_EXAMINE );
            MP_REGISTER_ACTION( ACTION_EXAMINE_AND_PICKUP );
            MP_REGISTER_ACTION( ACTION_SMASH );
            MP_REGISTER_ACTION( ACTION_MOVE_DOWN );
            MP_REGISTER_ACTION( ACTION_MOVE_UP );
            MP_REGISTER_ACTION( ACTION_OPEN );
            MP_REGISTER_ACTION( ACTION_CLOSE );
            MP_REGISTER_ACTION( ACTION_CHAT );
            MP_REGISTER_ACTION( ACTION_PICKUP );
            MP_REGISTER_ACTION( ACTION_PICKUP_ALL );
            // DDA MP v4.0a-205: grab state is per-character now; furniture
            // dragging runs through the engine activity for session avatars.
            MP_REGISTER_ACTION( ACTION_GRAB );
            MP_REGISTER_ACTION( ACTION_HAUL );
            MP_REGISTER_ACTION( ACTION_HAUL_TOGGLE );
            MP_REGISTER_ACTION( ACTION_BUTCHER );
            MP_REGISTER_ACTION( ACTION_LOOT );
        } else if( category == _( "Inventory" ) ) {
            MP_REGISTER_ACTION( ACTION_INVENTORY );
            MP_REGISTER_ACTION( ACTION_ADVANCEDINV );
            // DDA MP v4.0a-181/209: comparison is read-only on the mirror;
            // armor sorting runs on the mirror and sends the new order;
            // organize opens the mirror inventory (letters via '='), and
            // directional drop has its own direction + multidrop flow.
            MP_REGISTER_ACTION( ACTION_COMPARE );
            MP_REGISTER_ACTION( ACTION_SORT_ARMOR );
            MP_REGISTER_ACTION( ACTION_DIR_DROP );
            MP_REGISTER_ACTION( ACTION_ORGANIZE );
            MP_REGISTER_ACTION( ACTION_INSERT_ITEM );
            MP_REGISTER_ACTION( ACTION_UNLOAD_CONTAINER );
            MP_REGISTER_ACTION( ACTION_DROP );
            MP_REGISTER_ACTION( ACTION_USE );
            MP_REGISTER_ACTION( ACTION_WEAR );
            MP_REGISTER_ACTION( ACTION_TAKE_OFF );
            MP_REGISTER_ACTION( ACTION_EAT );
            MP_REGISTER_ACTION( ACTION_OPEN_CONSUME );
            MP_REGISTER_ACTION( ACTION_READ );
            MP_REGISTER_ACTION( ACTION_WIELD );
            MP_REGISTER_ACTION( ACTION_UNLOAD );
        } else if( category == _( "Combat" ) ) {
            MP_REGISTER_ACTION( ACTION_CYCLE_MOVE );
            MP_REGISTER_ACTION( ACTION_RESET_MOVE );
            MP_REGISTER_ACTION( ACTION_TOGGLE_RUN );
            MP_REGISTER_ACTION( ACTION_TOGGLE_CROUCH );
            MP_REGISTER_ACTION( ACTION_TOGGLE_PRONE );
            // DDA MP v4.0a-181/209: the movement-mode picker now sends the
            // chosen mode to the host; spells still need a targeting protocol.
            MP_REGISTER_ACTION( ACTION_OPEN_MOVEMENT );
            MP_REGISTER_ACTION( ACTION_FIRE );
            MP_REGISTER_ACTION( ACTION_RELOAD_ITEM );
            MP_REGISTER_ACTION( ACTION_RELOAD_WEAPON );
            MP_REGISTER_ACTION( ACTION_RELOAD_WIELDED );
            // DDA MP v4.0a-210: spellbook + netplay targeting cursor.
            MP_REGISTER_ACTION( ACTION_CAST_SPELL );
            MP_REGISTER_ACTION( ACTION_RECAST_SPELL );
            // DDA MP v4.0a-212: `a`-as-wielded, mend and autoattack.
            MP_REGISTER_ACTION( ACTION_USE_WIELDED );
            MP_REGISTER_ACTION( ACTION_MEND );
            MP_REGISTER_ACTION( ACTION_AUTOATTACK );
            MP_REGISTER_ACTION( ACTION_SELECT_FIRE_MODE );
            // DDA MP v4.0a-187: style and default-ammo pickers run on the
            // mirror and send the choice to the host.
            MP_REGISTER_ACTION( ACTION_SELECT_DEFAULT_AMMO );
            MP_REGISTER_ACTION( ACTION_PICK_STYLE );
            MP_REGISTER_ACTION( ACTION_THROW );
            MP_REGISTER_ACTION( ACTION_THROW_WIELDED );
            MP_REGISTER_ACTION( ACTION_FIRE_BURST );
            // DDA MP v4.0a-210: these are host-global settings; the host
            // applies them and announces the new value.
            MP_REGISTER_ACTION( ACTION_TOGGLE_AUTO_TRAVEL_MODE );
            MP_REGISTER_ACTION( ACTION_TOGGLE_SAFEMODE );
            MP_REGISTER_ACTION( ACTION_TOGGLE_AUTOSAFE );
            MP_REGISTER_ACTION( ACTION_IGNORE_ENEMY );
            MP_REGISTER_ACTION( ACTION_TOGGLE_AUTO_FEATURES );
            MP_REGISTER_ACTION( ACTION_TOGGLE_AUTO_PULP_BUTCHER );
            MP_REGISTER_ACTION( ACTION_TOGGLE_AUTO_MINING );
            MP_REGISTER_ACTION( ACTION_TOGGLE_AUTO_FORAGING );
        } else if( category == _( "Craft" ) ) {
            MP_REGISTER_ACTION( ACTION_CRAFT );
            MP_REGISTER_ACTION( ACTION_RECRAFT );
            MP_REGISTER_ACTION( ACTION_LONGCRAFT );
            MP_REGISTER_ACTION( ACTION_CONSTRUCT );
            MP_REGISTER_ACTION( ACTION_DISASSEMBLE );
        } else if( category == _( "Info" ) ) {
            MP_REGISTER_ACTION( ACTION_PL_INFO );
            // DDA MP v4.0a-182/183/186: morale, the bodypart graph, the medical
            // window and the faction list are read-only on the mirror. Missions
            // and the diary still read or write host-side data that is not
            // mirrored in full; hide them.
            MP_REGISTER_ACTION( ACTION_MORALE );
            MP_REGISTER_ACTION( ACTION_BODYSTATUS );
            MP_REGISTER_ACTION( ACTION_MEDICAL );
            MP_REGISTER_ACTION( ACTION_FACTIONS );
            // DDA MP v4.0a-191: the mission journal is streamed from the host.
            MP_REGISTER_ACTION( ACTION_MISSIONS );
            MP_REGISTER_ACTION( ACTION_MESSAGES );
            // DDA MP v4.0a-210: session-local diary on the mirror.
            MP_REGISTER_ACTION( ACTION_DIARY );
        } else if( category == _( "Misc" ) ) {
            MP_REGISTER_ACTION( ACTION_WAIT );
            MP_REGISTER_ACTION( ACTION_SLEEP );
            // DDA MP v4.0a-210: character lifecycle action, confirmed twice.
            MP_REGISTER_ACTION( ACTION_SUICIDE );
            // DDA MP v4.0a-196/199: bionics and mutations are read-only lists on
            // the mirror; workout picks intensity/duration on the client and
            // runs the engine actor on the host.
            MP_REGISTER_ACTION( ACTION_BIONICS );
            MP_REGISTER_ACTION( ACTION_MUTATIONS );
            MP_REGISTER_ACTION( ACTION_WORKOUT );
            MP_REGISTER_ACTION( ACTION_CONTROL_VEHICLE );
            // DDA MP v4.0a-198: read-only vehicle status from host data.
            entries.emplace_back( MP_ACTION_VEHICLE_INFO, true, -1,
                                  netplay::fork_text( "Vehicle status" ) );
            // DDA MP v4.0a-201: refuel from carried fuel.
            entries.emplace_back( MP_ACTION_VEHICLE_REFUEL, true, -1,
                                  netplay::fork_text( "Refuel vehicle" ) );
            // DDA MP v4.0a-204: damaged parts / repair.
            entries.emplace_back( MP_ACTION_VEHICLE_PARTS, true, -1,
                                  netplay::fork_text( "Vehicle parts" ) );
            MP_REGISTER_ACTION( ACTION_ITEMACTION );
            // DDA MP v4.0a-200: per-character theft mode cycles on the host.
            MP_REGISTER_ACTION( ACTION_TOGGLE_THIEF_MODE );
            // DDA MP v4.0a-174: assistance for a nearby busy player.
            entries.emplace_back( MP_ACTION_HELP_NEARBY, true, 'H',
                                  netplay::fork_text( "Help the busy player" ) );
#if defined(TILES)
            MP_REGISTER_ACTION( ACTION_ZOOM_OUT );
            MP_REGISTER_ACTION( ACTION_ZOOM_IN );
#endif
        }

        if( category != "back" ) {
            std::string msg = _( "Back" );
            msg += "\xE2\x80\xA6";
            entries.emplace_back( 2 * NUM_ACTIONS, true,
                                  hotkey_for_action( ACTION_ACTIONMENU, 1 ), msg );
        }

        std::string title = _( "Actions" );
        if( category != "back" ) {
            title += ": " + uppercase_first_letter( category );
        }

        uilist smenu;
        smenu.settext( title );
        smenu.entries = entries;
        smenu.query();
        const int selection = smenu.ret;

        // DDA MP v4.0a-175: repaint after a menu closes. The client view loop
        // otherwise leaves the parent list visible under the next submenu.
        ui_manager::redraw();

        // DDA MP v4.0a-174: custom entries are resolved before the category
        // handling below (their ids also live above NUM_ACTIONS).
        if( selection == MP_ACTION_HELP_NEARBY ) {
            return MP_ACTION_HELP_NEARBY;
        }
        if( selection == MP_ACTION_VEHICLE_INFO ) {
            return MP_ACTION_VEHICLE_INFO;
        }
        if( selection == MP_ACTION_VEHICLE_REFUEL ) {
            return MP_ACTION_VEHICLE_REFUEL;
        }
        if( selection == MP_ACTION_VEHICLE_PARTS ) {
            return MP_ACTION_VEHICLE_PARTS;
        }

        if( selection < 0 || selection == NUM_ACTIONS ) {
            return ACTION_NULL;
        } else if( selection == 2 * NUM_ACTIONS ) {
            if( category != "back" ) {
                category = "back";
            } else {
                return ACTION_NULL;
            }
        } else if( selection > NUM_ACTIONS ) {
            category = categories_by_int[selection];
        } else {
            return static_cast<action_id>( selection );
        }
    }
#undef MP_REGISTER_ACTION
#undef MP_REGISTER_CATEGORY
}

/* DDA MP v4.0a-72: the client's in-game main menu. Mirrors the host's
 * handle_main_menu for everything a remote player can do locally; saving,
 * debugging, bug-report export and the host entry are omitted, and
 * "Disconnect" replaces quit. Returns a uilist result: an action_id for the
 * action entries, MP_ENTRY_KEYBINDINGS or MP_ENTRY_DISCONNECT otherwise. */
static constexpr int MP_ENTRY_KEYBINDINGS = 90001;
static constexpr int MP_ENTRY_DISCONNECT = 90002;
static constexpr int MP_ENTRY_LONG_WAIT = 90003;
static constexpr int MP_ENTRY_COOP_HELP = 90004;

static int mp_client_main_menu()
{
    const input_context ctxt = get_default_mode_input_context();
    std::vector<uilist_entry> entries;

    const auto REGISTER_ACTION = [&]( action_id name ) {
        entries.emplace_back( name, true,
                              hotkey_for_action( name, /*maximum_modifier_count=*/1 ),
                              ctxt.get_action_name( action_ident( name ) ) );
    };

    REGISTER_ACTION( ACTION_HELP );
    // The hotkey is reserved for the uilist keybindings menu.
    entries.emplace_back( MP_ENTRY_KEYBINDINGS, true, std::nullopt,
                          ctxt.get_action_name( action_ident( ACTION_KEYBINDINGS ) ) );
    REGISTER_ACTION( ACTION_OPTIONS );
    REGISTER_ACTION( ACTION_TOGGLE_PANEL_ADM );
    REGISTER_ACTION( ACTION_AUTOPICKUP );
    REGISTER_ACTION( ACTION_AUTONOTES );
    REGISTER_ACTION( ACTION_SAFEMODE );
    REGISTER_ACTION( ACTION_DISTRACTION_MANAGER );
    REGISTER_ACTION( ACTION_COLOR );
    REGISTER_ACTION( ACTION_WORLD_MODS );
    REGISTER_ACTION( ACTION_ACTIONMENU );

    // DDA MP v4.0a-86: long wait, only for a bound session avatar (a watcher
    // must not make the host player pass turns).
    if( netplay::client_avatar_bind_state() == 2 ) {
        entries.emplace_back( MP_ENTRY_LONG_WAIT, true, 'L',
                              netplay::long_wait_active() ?
                              netplay::fork_text( "Long wait mode: ON (pass turns for other players)" ) :
                              netplay::fork_text( "Long wait mode: OFF (pass turns for other players)" ) );
    }

    entries.emplace_back( MP_ENTRY_DISCONNECT, true, 'Q',
                          netplay::fork_text( "Disconnect from server" ) );
    // DDA MP v4.0a-179: like the host's menu, the separate "Co-op help" tab is
    // not needed for the mod. The window stays reachable from the main menu;
    // the entry and its handler are kept for now but not displayed.
#if 0
    entries.emplace_back( MP_ENTRY_COOP_HELP, true, 'H',
                          netplay::fork_text( "Co-op help" ) );
#endif

    uilist smenu;
    smenu.settext( _( "MAIN MENU" ) );
    smenu.entries = entries;
    // DDA MP v4.0a-86: hide the pre-session main menu behind the client menu.
    {
        ui_adaptor mp_menu_blocker( ui_adaptor::disable_uis_below{} );
        smenu.query();
    }
    return smenu.ret;
}

/* Runs one local entry of the client main menu. Returns true when the session
 * must disconnect. `inv_text`/`char_json` are the latest host payloads, used by
 * the inventory / @ entries. */
static bool mp_client_run_menu_entry( int sel, const std::string &inv_text,
                                      const std::string &char_json )
{
    if( sel == MP_ENTRY_DISCONNECT ) {
        return true;
    }
    if( sel == MP_ENTRY_LONG_WAIT ) {
        const bool mp_lw = !netplay::long_wait_active();
        netplay::set_long_wait( mp_lw );
        // DDA MP v4.0a-88: the host skips AFK sessions in its turn parity.
        netplay::client_send_afk( mp_lw );
        return false;
    }
    if( sel == MP_ENTRY_KEYBINDINGS ) {
        ui_adaptor mp_menu_blocker( ui_adaptor::disable_uis_below{} );
        input_context kb = get_default_mode_input_context();
        kb.display_menu( false );
        return false;
    }
    if( sel == MP_ENTRY_COOP_HELP ) {
        netplay::show_coop_help();
        return false;
    }
    if( sel < 0 || sel >= NUM_ACTIONS ) {
        return false;
    }
    // DDA MP v4.0a-86: any submenu opened from the client menu must hide the
    // pre-session main menu (the same rule the inventory and @ menus follow).
    ui_adaptor mp_menu_blocker( ui_adaptor::disable_uis_below{} );
    switch( static_cast<action_id>( sel ) ) {
        case ACTION_HELP: {
            help_window hw;
            hw.show();
            break;
        }
        case ACTION_OPTIONS:
            get_options().show( true );
            break;
        case ACTION_TOGGLE_PANEL_ADM:
            panel_manager::get_manager().show_adm();
            break;
        case ACTION_AUTOPICKUP:
            get_auto_pickup().show();
            break;
        case ACTION_AUTONOTES:
            get_auto_notes_settings().show_gui();
            break;
        case ACTION_SAFEMODE:
            get_safemode().show();
            break;
        case ACTION_DISTRACTION_MANAGER:
            get_distraction_manager().show();
            break;
        case ACTION_COLOR:
            all_colors.show_gui();
            break;
        case ACTION_WORLD_MODS:
            if( world_generator && world_generator->active_world ) {
                world_generator->show_active_world_mods(
                    world_generator->active_world->active_mod_order );
            }
            break;
        case ACTION_ACTIONMENU: {
            mp_client_dispatch_action( mp_client_action_menu(), inv_text, char_json );
            break;
        }
        default:
            // Anything else is not a local menu item.
            break;
    }
    return false;
}

/* DDA MP v4.2: native renderer adapter.
 *
 * Instead of replaying pre-resolved sprite ids through a private backend, the
 * client writes the host's raw cell state into its own map and lets the game's
 * single-player renderer (cata_tiles::draw) draw the frame. Terrain, furniture,
 * traps, items, the avatar, lighting and animations then come from the same
 * code path the host uses. The synthesized map lives at a fixed local origin
 * around the client's avatar, so no world save and no mapgen travel is needed;
 * the streamed frame is the renderer's viewport.
 */
static bool mp_native_render_enabled()
{
    static const bool enabled = []() {
        const char *env = std::getenv( "DDA_MP_NATIVE_RENDER" );
        return env == nullptr || env[0] == '\0' || std::string( env ) != "0";
    }();
    return enabled;
}

#if defined(TILES)
static void mp_native_ensure_bubble( const tripoint_abs_ms &center_abs )
{
    static bool ready = false;
    static tripoint_abs_ms bubble_center;
    // The mirror loads an 11-submap map (132 tiles). The visible 49x32 frame
    // needs ~24 tiles of camera margin, so the camera can stay centred while
    // the avatar is at most ~42 tiles from the anchor; 40 keeps that safe.
    constexpr int mp_mirror_reanchor_distance = 40;
    const bool inside = ready &&
                        std::abs( center_abs.x() - bubble_center.x() ) <=
                        mp_mirror_reanchor_distance &&
                        std::abs( center_abs.y() - bubble_center.y() ) <=
                        mp_mirror_reanchor_distance &&
                        center_abs.z() == bubble_center.z();
    if( inside ) {
        return;
    }
    mp_inv_log( "native: ensure bubble begin" );
    const auto mp_bubble_t0 = std::chrono::steady_clock::now();
    map &here = get_map();
    static const ter_id t_open_air( "t_open_air" );
    // The client keeps no save of its own. Fill the mapbuffer with empty,
    // transparent submaps for a 7x7 overmap-tile block (all z levels) so
    // map::load() never runs mapgen, then load that bubble. The streamed
    // frame overwrites the visible cells; everything else stays open air.
    const tripoint_abs_omt omt0 = project_to<coords::omt>( center_abs );
    for( int ox = -3; ox <= 3; ox++ ) {
        for( int oy = -3; oy <= 3; oy++ ) {
            const tripoint_abs_sm base = project_to<coords::sm>(
                                             tripoint_abs_omt( omt0.x() + ox, omt0.y() + oy, omt0.z() ) );
            for( int sx = 0; sx < 2; sx++ ) {
                for( int sy = 0; sy < 2; sy++ ) {
                    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
                        const tripoint_abs_sm addr( base.x() + sx, base.y() + sy, z );
                        if( MAPBUFFER.submap_exists( addr ) ) {
                            continue;
                        }
                        auto sm = std::make_unique<submap>();
                        for( int mx = 0; mx < SEEX; mx++ ) {
                            for( int my = 0; my < SEEY; my++ ) {
                                sm->set_ter( point_sm_ms( mx, my ), t_open_air );
                            }
                        }
                        MAPBUFFER.add_submap( addr, sm );
                    }
                }
            }
        }
    }
    const tripoint_abs_sm csm = project_to<coords::sm>( center_abs );
    const tripoint_abs_sm origin( csm.x() - MAPSIZE / 2, csm.y() - MAPSIZE / 2, csm.z() );
    int missing = 0;
    for( int gx = 0; gx < MAPSIZE; gx++ ) {
        for( int gy = 0; gy < MAPSIZE; gy++ ) {
            const tripoint_abs_sm gas = origin + tripoint_rel_sm( gx, gy, 0 );
            const tripoint_abs_sm base = project_to<coords::sm>(
                                             project_to<coords::omt>( gas ) );
            for( int dx = 0; dx < 2; dx++ ) {
                for( int dy = 0; dy < 2; dy++ ) {
                    for( int z = -OVERMAP_DEPTH; z <= OVERMAP_HEIGHT; z++ ) {
                        if( !MAPBUFFER.submap_exists(
                                tripoint_abs_sm( base.x() + dx, base.y() + dy, z ) ) ) {
                            ++missing;
                        }
                    }
                }
            }
        }
    }
    const auto mp_fill_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - mp_bubble_t0 ).count();
    mp_inv_log( string_format( "native: submaps ready, missing=%d, overmap (fill=%lldms)",
                               missing, static_cast<long long>( mp_fill_ms ) ) );
    // map::loadn() asks the current overmap for weather settings; the client
    // never loaded one. Create/load the host's overmap chunk first (same world
    // seed, so the base world matches the host's).
    const auto mp_om_t0 = std::chrono::steady_clock::now();
    overmap_buffer.get( project_to<coords::om>( center_abs ).xy() );
    const auto mp_om_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::steady_clock::now() - mp_om_t0 ).count();
    // Populate the grid directly: map::load() also actualizes every submap
    // (map extras/monsters) and reconciles items, which assumes a real save.
    // Only the storage and caches are needed for rendering.
    const auto mp_load_t0 = std::chrono::steady_clock::now();
    here.mp_load_bubble_storage( origin );
    const auto mp_load_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - mp_load_t0 ).count();
    mp_inv_log( string_format( "native: overmap=%lldms load=%lldms",
                               static_cast<long long>( mp_om_ms ),
                               static_cast<long long>( mp_load_ms ) ) );

    mp_inv_log( "native: bubble loaded, placing avatar" );
    if( ready ) {
        mp_inv_log( string_format( "native: re-anchor %d,%d -> %d,%d",
                                   bubble_center.x(), bubble_center.y(),
                                   center_abs.x(), center_abs.y() ) );
    }
    get_avatar().setpos( center_abs, false );
    // The streamed terrain writes go through map::ter_set(), which calls
    // avatar::sees() and therefore indexes the visibility cache. A session
    // without a local world has never built one, so create it here (from the
    // empty bubble) before the first snapshot is applied.
    const auto mp_cache_t0 = std::chrono::steady_clock::now();
    here.build_map_cache_for( center_abs.z(), /*skip_lightmap=*/false, get_avatar() );
    here.update_visibility_cache( center_abs.z() );
    const auto mp_cache_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                 std::chrono::steady_clock::now() - mp_cache_t0 ).count();
    ready = true;
    bubble_center = center_abs;
    mp_inv_log( string_format( "native: bubble at %d,%d z%d (cache=%lldms total=%lldms)",
                               center_abs.x(), center_abs.y(), center_abs.z(),
                               static_cast<long long>( mp_cache_ms ),
                               static_cast<long long>(
                                   std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::steady_clock::now() - mp_bubble_t0 ).count() ) ) );
}

static void mp_native_apply_avatar_json()
{
    std::string json;
    if( !netplay::client_character_json( json ) || json.empty() ) {
        return;
    }
    static std::string applied;
    if( json == applied ) {
        return;
    }
    static const bool mp_look_on = []() {
        const char *v = std::getenv( "DDA_MP_NATIVE_LOOK" );
        return !( v != nullptr && v[0] != '\0' && std::string( v ) == "0" );
    }();
    if( !mp_look_on ) {
        return;
    }
    // DDA MP v4.0a-117: the panels (and several local UIs) evaluate math
    // conditions against the global avatar, not the passed Character. A stub
    // g->u with no body parts made 'u_val(bmi_permil)' and friends crash the
    // client. Mirror the whole character into g->u and keep only the facts
    // that belong to the client's own view (position, view offset).
    static const bool mp_full_on = []() {
        const char *v = std::getenv( "DDA_MP_FULL_AVATAR" );
        return v == nullptr || v[0] == '\0' || std::string( v ) != "0";
    }();
    if( mp_full_on ) {
        avatar &you = get_avatar();
        const tripoint_abs_ms keep_pos = you.pos_abs();
        const tripoint_rel_ms keep_off = you.view_offset;
        try {
            JsonValue jv = json_loader::from_string( json );
            you.deserialize( jv.get_object() );
            you.setpos( keep_pos, false );
            you.view_offset = keep_off;
            applied = json;
            mp_native_avatar_ready = true;
            mp_inv_log( "native: avatar mirror applied" );
            return;
        } catch( const std::exception &e ) {
            mp_inv_log( std::string( "native: avatar mirror error " ) + e.what() );
        } catch( ... ) {
            mp_inv_log( "native: avatar mirror error unknown" );
        }
    }
    npc *mirror = mp_mirror();
    if( mirror == nullptr ) {
        return;
    }
    // Copy only what the renderer reads for appearance. Deserializing the whole
    // character into g->u also moves it and replaces its sight/effects, which
    // breaks the streamed view; position/vision stay with the client avatar.
    avatar &you = get_avatar();
    you.male = mirror->male;
    static const bool mp_worn_on = []() {
        const char *v = std::getenv( "DDA_MP_NATIVE_WORN" );
        return !( v != nullptr && v[0] != '\0' && std::string( v ) == "0" );
    }();
    if( mp_worn_on ) {
        you.worn = mirror->worn;
        if( item_location w = mirror->get_wielded_item() ) {
            you.set_wielded_item( *w );
        } else {
            you.unwield();
        }
    }
    applied = json;
    mp_inv_log( "native: avatar look applied" );
}
static void mp_native_apply_creatures( const std::vector<snapshot::creature_frame> &creatures )
{
    creature_tracker &tracker = get_creature_tracker();
    tracker.clear();
    tracker.clear_npcs();
    for( const snapshot::creature_frame &cf : creatures ) {
        try {
            JsonValue jv = json_loader::from_string( cf.json );
            const tripoint_abs_ms ap( cf.ax, cf.ay, cf.az );
            if( cf.kind == 0 ) {
                auto mon = std::make_shared<monster>();
                mon->deserialize( jv.get_object() );
                mon->setpos( ap, false );
                tracker.add( mon );
            } else {
                auto guy = std::make_shared<npc>();
                guy->deserialize( jv );
                guy->setpos( ap, false );
                tracker.mp_add_npc( guy );
            }
        } catch( const std::exception &e ) {
            mp_inv_log( std::string( "native: creature " ) + e.what() );
        } catch( ... ) {
            mp_inv_log( "native: creature unknown error" );
        }
    }
}

static void mp_native_apply_snapshot( const snapshot &snap )
{
    const tripoint_abs_ms center_abs( snap.origin_x, snap.origin_y, snap.origin_z );
    mp_native_ensure_bubble( center_abs );
    map &here = get_map();
    get_avatar().setpos( center_abs, false );
    if( netplay::mp_diag_enabled() ) {
        static int mp_apply_no = 0;
        static bool mp_have_last = false;
        static int mp_last_x = 0;
        static int mp_last_y = 0;
        if( !mp_have_last || snap.origin_x != mp_last_x || snap.origin_y != mp_last_y ) {
            mp_have_last = true;
            mp_last_x = snap.origin_x;
            mp_last_y = snap.origin_y;
            ++mp_apply_no;
            mp_inv_log( string_format( "snapapply #%d origin=%d,%d av=%d,%d",
                                       mp_apply_no, snap.origin_x, snap.origin_y,
                                       get_avatar().pos_abs().x(),
                                       get_avatar().pos_abs().y() ) );
        }
    }
    const tripoint_bub_ms center = here.get_bub( center_abs );
    if( !here.inbounds( center ) ) {
        return;
    }
    if( snap.has_time ) {
        calendar::turn = time_point::from_turn( snap.turn );
        if( !snap.weather.empty() ) {
            const weather_type_id w( snap.weather );
            if( w.is_valid() ) {
                get_weather().weather_id = w;
            }
        }
        g->reset_light_level();
    }
    static const furn_id f_null( "f_null" );
    // DDA MP v4.0a-114: allocate this frame's memory region once; per-cell
    // writes below go to the client avatar's own map memory (the native
    // renderer reads it for everything outside the current sight).
    avatar &you = get_avatar();
    you.prepare_map_memory_region(
        here.get_abs( center + tripoint_rel_ms( -snap.width / 2, -snap.height / 2, 0 ) ),
        here.get_abs( center + tripoint_rel_ms( snap.width / 2, snap.height / 2, 0 ) ) );
    for( int y = 0; y < snap.height; y++ ) {
        for( int x = 0; x < snap.width; x++ ) {
            const tile_cell &t = snap.tiles[static_cast<size_t>( y ) * snap.width + x];
            const tripoint_bub_ms p = center + tripoint_rel_ms( x - snap.width / 2,
                                       y - snap.height / 2, 0 );
            if( !here.inbounds( p ) ) {
                continue;
            }
            if( !t.ter.empty() && ter_str_id( t.ter ).is_valid() ) {
                here.ter_set( p, ter_id( t.ter ) );
            }
            if( !t.furn.empty() && t.furn.rfind( "vp_", 0 ) != 0 &&
                furn_str_id( t.furn ).is_valid() ) {
                here.furn_set( p, furn_id( t.furn ) );
            } else {
                here.furn_set( p, f_null );
            }
            if( !t.trap.empty() && trap_str_id( t.trap ).is_valid() ) {
                here.trap_set( p, trap_id( t.trap ) );
            }
            // DDA MP v4.0a-114: mirror the raw field, so the native renderer
            // draws fire/smoke/blood the same way the host does.
            field &fld = here.field_at( p );
            if( fld.field_count() > 0 ) {
                fld.clear();
            }
            if( !t.field.empty() ) {
                const field_type_str_id ft( t.field );
                if( ft.is_valid() ) {
                    fld.add_field( ft.id(), t.field_intensity > 0 ? t.field_intensity : 1 );
                }
            }
            // DDA MP v4.0a-135: graffiti.
            if( here.has_graffiti_at( p ) ) {
                here.delete_graffiti( p );
            }
            if( !t.graffiti.empty() ) {
                here.set_graffiti( p, t.graffiti );
            }
            // The viewer's map memory for cells outside its sight.
            const tripoint_abs_ms ap = here.get_abs( p );
            if( !t.mem_ter.empty() ) {
                you.memorize_terrain( ap, t.mem_ter, t.mem_ter_sub, t.mem_ter_rot );
            }
            if( !t.mem_dec.empty() ) {
                you.memorize_decoration( ap, t.mem_dec, t.mem_dec_sub, t.mem_dec_rot );
            }
            if( t.mem_sym != 0 ) {
                you.memorize_symbol( ap, t.mem_sym );
            }
            here.i_clear( p );
            if( string_starts_with( t.item, "corpse_" ) ) {
                // The host names monster corpses "corpse_<mtype>"; the real
                // itype is always "corpse" with the mtype attached, so build
                // it the same way the engine does.
                const mtype_id mt( t.item.substr( 7 ) );
                if( mt.is_valid() ) {
                    here.add_item_or_charges( p, item::make_corpse( mt, calendar::turn, "" ),
                                              false );
                }
            } else if( !t.item.empty() && item::type_is_defined( itype_id( t.item ) ) ) {
                item itm( itype_id( t.item ), calendar::turn );
                if( !t.item_variant.empty() ) {
                    itm.set_itype_variant( t.item_variant );
                }
                here.add_item_or_charges( p, itm, false );
            }
        }
    }
    static const bool mp_creatures_on = []() {
        const char *v = std::getenv( "DDA_MP_NATIVE_CREATURES" );
        return !( v != nullptr && v[0] != '\0' && std::string( v ) == "0" );
    }();
    if( mp_creatures_on ) {
        mp_native_apply_creatures( snap.creatures );
    }
    here.build_map_cache_for( center.z(), /*skip_lightmap=*/false, get_avatar() );
    here.update_visibility_cache( center.z() );
    // DDA MP v4.0a-127: refresh the avatar's visible-creature cache. The
    // creature mirror is rebuilt on every snapshot, so a cache from an older
    // frame holds dangling npc/mtype pointers; the compass and the creature
    // panel read exactly this cache.
    g->mon_info_update();
    static int mp_light_logs = 0;
    if( mp_light_logs < 3 ) {
        ++mp_light_logs;
        const lit_level ll = here.get_cache_ref( center.z() ).visibility_cache[center.x()][center.y()];
        mp_inv_log( string_format( "native: light z=%d g=%d ll=%d turn=%d ter=%s furn=%s items=%d",
                                   center.z(), static_cast<int>( g->light_level( center.z() ) ),
                                   static_cast<int>( ll ), to_turn<int>( calendar::turn ),
                                   here.ter( center ).id().str().c_str(),
                                   here.furn( center ).id().str().c_str(),
                                   static_cast<int>( here.i_at( center ).size() ) ) );
    }
}
/// Draw the frame with the game's own renderer. False = caller falls back to
/// the replay backend for this frame.
static bool mp_native_draw_map( void *backend, const snapshot &snap, int dest_x, int dest_y,
                                int tw, int th )
{
    if( backend == nullptr || tw <= 0 || th <= 0 || snap.tiles.empty() || !snap.has_origin ) {
        return false;
    }
    try {
        mp_native_apply_snapshot( snap );
        // cata_tiles::draw places the view with the global POSX/POSY ("player
        // offset inside the terrain window"); the headless client has no
        // game::init_ui, so set them for the streamed viewport.
        const int old_posx = POSX;
        const int old_posy = POSY;
        POSX = snap.width / 2;
        POSY = snap.height / 2;
        const map &here = get_map();
        const tripoint_bub_ms center = here.get_bub(
                                           tripoint_abs_ms( snap.origin_x, snap.origin_y, snap.origin_z ) );
        const bool ok = netplay::tiles_backend_draw_native(
                            backend, dest_x, dest_y, center.x(), center.y(), center.z(),
                            snap.width * tw, snap.height * th );
        POSX = old_posx;
        POSY = old_posy;
        if( !ok ) {
            return false;
        }
        static int mp_native_ok = 0;
        if( mp_native_ok < 3 ) {
            ++mp_native_ok;
            mp_inv_log( string_format( "native: draw ok %dx%d at %d,%d", snap.width,
                                       snap.height, dest_x, dest_y ) );
        }
        return true;
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "native: " ) + e.what() );
    } catch( ... ) {
        mp_inv_log( "native: unknown error" );
    }
    return false;
}
#endif

static catacurses::window mp_native_terrain_win;
static int mp_ntw_w = -1;
static int mp_ntw_h = -1;
static int mp_ntw_x = -1;
static int mp_ntw_y = -1;

/// DDA MP v4.2: hand the map to the game's normal terrain-window path. The
/// window is marked dirty; curses_drawwindow() then calls cata_tiles::draw()
/// with g->ter_view_p during the regular present, exactly like single player
/// (no separate canvas pass, no extra display-buffer scope, one present).
static void mp_native_setup_terrain_window( const snapshot &snap, int dest_x, int dest_y,
        int tw, int th, int font_w, int font_h )
{
    if( font_w <= 0 || font_h <= 0 ) {
        return;
    }
    const int term_w = std::max( 1, ( snap.width * tw + font_w - 1 ) / font_w );
    const int term_h = std::max( 1, ( snap.height * th + font_h - 1 ) / font_h );
    const int cell_x = std::max( 0, dest_x ) / font_w;
    const int cell_y = std::max( 0, dest_y ) / font_h;
    if( !mp_native_terrain_win || term_w != mp_ntw_w || term_h != mp_ntw_h ||
        cell_x != mp_ntw_x || cell_y != mp_ntw_y ) {
        mp_native_terrain_win = catacurses::newwin( term_h, term_w, point( cell_x, cell_y ) );
        g->w_terrain = mp_native_terrain_win;
        mp_ntw_w = term_w;
        mp_ntw_h = term_h;
        mp_ntw_x = cell_x;
        mp_ntw_y = cell_y;
    }
    TERRAIN_WINDOW_WIDTH = snap.width;
    TERRAIN_WINDOW_HEIGHT = snap.height;
    TERRAIN_WINDOW_TERM_WIDTH = term_w;
    TERRAIN_WINDOW_TERM_HEIGHT = term_h;
    POSX = snap.width / 2;
    POSY = snap.height / 2;
    g->ter_view_p = get_map().get_bub(
                        tripoint_abs_ms( snap.origin_x, snap.origin_y, snap.origin_z ) );
    if( netplay::mp_diag_enabled() ) {
        static int mp_last_setup_x = 0;
        static int mp_last_setup_y = 0;
        static bool mp_setup_have_last = false;
        if( !mp_setup_have_last || snap.origin_x != mp_last_setup_x ||
            snap.origin_y != mp_last_setup_y ) {
            mp_setup_have_last = true;
            mp_last_setup_x = snap.origin_x;
            mp_last_setup_y = snap.origin_y;
            const map &hm = get_map();
            mp_inv_log( string_format(
                            "render origin=%d,%d av=%d,%d tv=%d,%d off=%d,%d sub=%d,%d",
                            snap.origin_x, snap.origin_y,
                            get_avatar().pos_bub().x(), get_avatar().pos_bub().y(),
                            g->ter_view_p.x(), g->ter_view_p.y(),
                            get_avatar().view_offset.x(), get_avatar().view_offset.y(),
                            hm.get_abs_sub().x(), hm.get_abs_sub().y() ) );
        }
    }
    werase( mp_native_terrain_win );
    wnoutrefresh( mp_native_terrain_win );
}

/* DDA MP v4.0a-159: queue the tileset's own "cursor" sprite, exactly like the
 * host's game::draw_cursor spends it. The previous hand-drawn ImGui brackets
 * used a different arm/line geometry than the tileset cursor. */
#if defined(TILES)
static void mp_queue_native_cursor( const snapshot &snap )
{
    tripoint_abs_ms ap;
    tripoint_bub_ms lp;
    if( !mp_cursor_target( snap, ap, lp ) ) {
        return;
    }
    g->draw_cursor( lp );
    // The terrain window is otherwise only refreshed when a new snapshot
    // arrives (a few times a second); the moved reticle must reach the next
    // present at view-loop speed.
    wnoutrefresh( g->w_terrain );
}
#else
static void mp_queue_native_cursor( const snapshot & )
{
}
#endif

/* DDA MP v4.0a-115/160: the sidebar itself, drawn by the game's own panel
 * code for the client's character mirror. The loop below mirrors
 * game::draw_panels(): spacer rows, disabled panels take no space, and a
 * panel whose draw returns a negative line count reclaims those rows (the
 * client used to keep the reserved height and left blank gaps). */
static int mp_native_sidebar_used_rows = 0;

static int mp_native_sidebar_width()
{
    panel_manager &mgr = panel_manager::get_manager();
    int width = 0;
    for( const window_panel &p : mgr.get_current_layout().panels() ) {
        if( !p.render() || p.get_height() == -2 || p.get_widget().is_null() ) {
            continue;
        }
        width = std::max( width, p.get_width() );
    }
    return width;
}

static void mp_draw_native_sidebar()
{
    static const bool enabled = []() {
        const char *v = std::getenv( "DDA_MP_NATIVE_SIDEBAR" );
        return v == nullptr || v[0] == '\0' || std::string( v ) != "0";
    }();
    if( !enabled ) {
        return;
    }
    // Panels evaluate math conditions against the global avatar; draw them
    // only after the character mirror has been applied to g->u, otherwise a
    // stub avatar (no body parts) crashes widgets like the needs block.
    if( !mp_native_avatar_ready ) {
        return;
    }
    // The avatar mirror is applied to g->u, so the panels read the same
    // character the widgets' global math conditions see.
    avatar &you = get_avatar();
    const bool right = get_option<std::string>( "SIDEBAR_POSITION" ) == "right";
    const int spacer = get_option<bool>( "SIDEBAR_SPACERS" ) ? 1 : 0;
    int y = 0;
    for( const window_panel &panel : panel_manager::get_manager().get_current_layout().panels() ) {
        if( !panel.render() ) {
            continue;
        }
        // Widget-less panels are the message log and the native minimap; the
        // client paints its own log/minimap in the space that follows.
        if( panel.get_widget().is_null() ) {
            continue;
        }
        int h = std::min( panel.get_height(), TERMY - y );
        // Height -2 is the message log: the client paints its own log/minimap
        // in the space that follows the panels.
        if( h == -2 ) {
            continue;
        }
        h += spacer;
        if( !panel.toggle || h <= 0 ) {
            continue;
        }
        const int w = panel.get_width();
        if( w <= 0 || w > TERMX ) {
            continue;
        }
        catacurses::window win = catacurses::newwin( h, w,
                                 point( right ? TERMX - w : 0, y ) );
        werase( win );
        h += panel.draw( { you, win, panel.get_widget() } );
        wnoutrefresh( win );
        y += h;
    }
    mp_native_sidebar_used_rows = y;
}

/* Black column behind the native panels. It is painted before the UI redraw,
 * not with the panels at the end of the frame: the pixel layer (message log,
 * minimap) is rendered during the redraw and must stay above it. */
static void mp_draw_native_sidebar_bg()
{
    if( !mp_native_render_enabled() || !mp_native_avatar_ready ) {
        return;
    }
    const int full_w = mp_native_sidebar_width();
    if( full_w <= 0 || full_w > TERMX ) {
        return;
    }
    const bool right = get_option<std::string>( "SIDEBAR_POSITION" ) == "right";
    catacurses::window bg = catacurses::newwin( TERMY, full_w,
                            point( right ? TERMX - full_w : 0, 0 ) );
    werase( bg );
    wnoutrefresh( bg );
}

void run_view_loop()
{
#if defined(TILES)
    // Diagnostics trace for the tileset replay renderer. Written next to the
    // exe when the game is launched without a changed working dir.
    static auto mp_tlog = []( const std::string & msg ) {
        if( !mp_netplay_diag ) {
            return;
        }
        try {
                std::ofstream f( netplay::mp_diag_dir() + "\\netplay_tiles.log", std::ios::app );
                if( f ) {
                    f << msg << "\n";
                }
        } catch( ... ) { }
    };
    static int trace_frames = 0;
    auto mp_frame_trace = [&]( const char *what ) {
        if( trace_frames < 200 ) {
            ++trace_frames;
            mp_tlog( string_format( "tiles frame #%d %s", trace_frames, what ) );
        }
    };
#endif
    // Stray debugmsgs (spawn errors, JSON glitches...) must never raise a
    // modal over our fullscreen replay: route them to the debug log and
    // restore the prompt behavior when the view loop ends (RAII handles the
    // early exits too).
    struct mp_prompt_guard {
        ~mp_prompt_guard() {
            debug_suppress_prompts( false );
        }
    } mp_prompts;
    debug_suppress_prompts( true );
    // Track whether the user left on purpose (ESC) to skip the reconnect
    // offer — sudden disconnects offer to rejoin instead.
    bool voluntary_quit = false;
    bool mp_dead = false;

    // Guard against garbage frames: no sane session needs more than this.
    constexpr int SNAP_MAX = 400;
    // DDA MP v3.9: the ASCII replay grid is now a debug feature. The client
    // renders the tileset sprites as soon as a snapshot arrives; flip this to
    // true in code (then rebuild) to also paint the legacy ASCII grid/border
    // under the tile canvas.
    constexpr bool mp_ascii_debug = false;

#if defined(TILES)
    // Tileset replay backend (optional; kept across frames, freed on exit).
    void *tiles_backend = nullptr;
    int font_px_w = 0;
    int font_px_h = 0;
    auto tiles_destroy = [&]() {
        if( tiles_backend != nullptr ) {
            netplay::tiles_backend_destroy( tiles_backend );
            tiles_backend = nullptr;
        }
    };
#endif

    // DDA MP v3.9: the host streams snapshots only on its own turn, so an
    // idle host would leave this loop without a single frame until somebody
    // pressed a key (the menu persisted). Ask for the current picture right
    // after the load and re-ask every second until the first one arrives.
    bool mp_have_picture = false;
    uint32_t mp_last_pic_request = 0;
    auto mp_request_picture = [&]() {
        if( mp_have_picture ) {
            return;
        }
        const uint32_t now = GetTicks();
        if( mp_last_pic_request != 0 && now - mp_last_pic_request < 1000 ) {
            return;
        }
        mp_last_pic_request = now;
        netplay::client_request_snapshot();
    };

    int cur_w = -1;
    int cur_h = -1;
    catacurses::window win;
    // DDA MP v4.0a-7: one-line turn-pacing hint drawn over the picture.
    catacurses::window mp_hint_win;
    std::string mp_hint_text;
    // DDA MP v4.0a-8: character sheet panel (C toggles it).
    catacurses::window mp_sheet_win;
    std::string mp_sheet_text;
    bool mp_sheet_open = true; // the status panel is docked by default
    snapshot mp_sidebar;
    sidebar_frame mp_side;
    // DDA MP v4.0a-11: the client keeps its own message log (the host's log is
    // not streamed). Fed by state diffs and client-side events.
    struct mp_log_line {
        std::string text;
        int type = 0;
        int turn = 0;
    };
    static std::vector<mp_log_line> mp_log;
    static mp_sheet_data mp_prev;
    static int mp_cur_turn = 0;
    auto mp_log_add = []( const std::string & s, int type ) {
        // The host formats its event lines in ITS language. Translate the
        // plain ones again with the client's catalog, so a Russian client
        // does not show the host's English log entries. Formatted strings
        // with names/args are not msgids and remain as sent.
        const std::string txt = to_translation( s ).translated();
        if( mp_log.size() >= 200 ) {
            mp_log.erase( mp_log.begin() );
        }
        mp_log.push_back( { txt, type, mp_cur_turn } );
        mp_inv_log( "clientlog: " + txt );
        // DDA MP v4.0a-170: keep the game's own message history in sync, so the
        // client's "messages" screen (the host's M key) shows the same journal
        // instead of an empty list.
        Messages::add_msg( game_message_params( static_cast<game_message_type>( type ) ), txt );
    };
    static float mp_sb_overlay_w = 0.0f;
    static minimap_frame mp_mini_map;
    static bool mp_inv_open = false;
    static int mp_inv_scroll = 0;
    static int mp_inv_sel = 0;
    static std::string mp_inv_text;
    static std::string mp_char_json;
    // DDA MP v4.0a-97: 'g' with items on several adjacent tiles asks which one
    // first (the host's own choose_adjacent_highlight does the same).
    static bool mp_pick_tiles_active = false;
    static int mp_pick_gen = 0;
    static std::vector<point> mp_pick_tiles;
    static int mp_pick_sel = 0;
    // DDA MP v4.0a-10: draw the sidebar in the pixel layer (ImGui) so its
    // position matches the host's exactly and does not depend on the curses
    // grid scale.
    // DDA MP v4.0a-18: a permanent disable_uis_below made the client redraw
    // the whole frame every iteration (visible flicker), so the main menu is
    // hidden only while a popup is open (see mp_client_item_menu).
    // DDA MP v4.0a-120: the client loop only renders ImGui's draw data inside
    // an overlay pass (see netplay::overlay_begin/end). The sidebar pixel layer
    // is therefore a plain lambda invoked after the native panels, not a
    // ui_adaptor (whose commands were recorded but never painted).
    auto mp_draw_sidebar_overlay = [&]() {
        const ImGuiIO &io = ImGui::GetIO();
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        const float fs = ImGui::GetFontSize();
        const float pad = fs * 0.6f;
        // (throw reticle is drawn by the tileset pass, see line_trail/cursor)
        // DDA MP v4.0a-14: inventory panel (read-only, toggled with 'i').
        {
            const bool mp_panels_native = mp_native_render_enabled();
            const int mp_sb_cells = mp_panels_native
                                    ? mp_native_sidebar_width()
                                    : ( mp_side.width > 0 ? mp_side.width : 0 );
            if( mp_sb_cells <= 0 ) {
                return;
            }
            const float cw = ImGui::CalcTextSize( "M" ).x;
            const float ch = ImGui::GetTextLineHeight();
            // The native panels start exactly at TERMX - width cells; the log
            // must start there too (the old extra -cw put it one cell left of
            // the sidebar).
            const float x0 = std::max( 0.0f,
                                       io.DisplaySize.x - mp_sb_cells * cw );
            // DDA MP v4.0a-115: the panels themselves are painted by the
            // game's own panel renderer into curses windows; only the
            // client-only log and minimap stay in the pixel layer, below the
            // rows the panels occupy.
            int draw_y = 0;
            if( mp_panels_native ) {
                draw_y = mp_native_sidebar_used_rows;
            } else {
            dl->AddRectFilled( ImVec2( x0 - cw, 0.0f ), io.DisplaySize, IM_COL32( 0, 0, 0, 210 ) );
            // Real used height: the frame is TERMY rows tall, most of them empty.
            int used_rows = 0;
            for( int row = 0; row < mp_side.height; row++ ) {
                for( int col = 0; col < mp_side.width; col++ ) {
                    const std::string &t = mp_side.cells[static_cast<size_t>( row ) *
                                                          mp_side.width + col].text;
                    if( !t.empty() && t != " " ) {
                        used_rows = row + 1;
                        break;
                    }
                }
            }
            for( int row = 0; row < used_rows; row++ ) {
                // Skip blank rows: the host's panels leave empty lines between
                // blocks, the client shows a tight list.
                bool row_empty = true;
                for( int col = 0; col < mp_side.width; col++ ) {
                    const std::string &t = mp_side.cells[static_cast<size_t>( row ) *
                                                          mp_side.width + col].text;
                    if( !t.empty() && t != " " ) {
                        row_empty = false;
                        break;
                    }
                }
                if( row_empty ) {
                    continue;
                }
                const float y = draw_y * ch;
                if( y + ch > io.DisplaySize.y ) {
                    break;
                }
                int col = 0;
                while( col < mp_side.width ) {
                    const sidebar_cell &c0 = mp_side.cells[static_cast<size_t>( row ) *
                                                           mp_side.width + col];
                    if( c0.text.empty() || c0.text == " " ) {
                        ++col;
                        continue;
                    }
                    std::string run = c0.text;
                    int c = col + 1;
                    while( c < mp_side.width ) {
                        const sidebar_cell &cn = mp_side.cells[static_cast<size_t>( row ) *
                                                               mp_side.width + c];
                        if( cn.text.empty() || cn.color != c0.color ) {
                            break;
                        }
                        run += cn.text;
                        ++c;
                    }
                    dl->AddText( ImVec2( x0 + col * cw, y ),
                                 cataimgui::ImU32_from_color( decode_cell_color( c0.color ) ),
                                 run.c_str() );
                    col = c;
                }
                ++draw_y;
            }
            }
            // DDA MP v4.0a-11: the client's own message log, under the panels.
            // Native panels live on the curses grid (TERMY rows over the whole
            // window), so convert their row count to pixels with the grid's
            // own row height instead of the ImGui line height.
            const float mp_row_h = io.DisplaySize.y / static_cast<float>( std::max( 1, TERMY ) );
            float ly = mp_panels_native ? draw_y * mp_row_h : draw_y * ch;
            // DDA MP v4.0a-13: creatures this character can see (own list).
            // DDA MP v4.0a-77: the host's compass legend is now computed from
            // this character, so the sidebar already carries the list. The old
            // client-side list stays behind DDA_MP_CLIENT_CRITTER_LIST=1 so the
            // two layouts can still be compared.
            static const bool mp_client_critter_list = []() {
                const char *env = std::getenv( "DDA_MP_CLIENT_CRITTER_LIST" );
                return env != nullptr && env[0] != '\0' && std::string( env ) != "0";
            }();
            int mp_shown = 0;
            const float mp_list_w = std::max( 1.0f, io.DisplaySize.x - x0 );
            for( size_t bi = 0; mp_client_critter_list &&
                 bi < mp_mini_map.beacons.size() &&
                 mp_shown < 12; bi += 2 ) {
                if( ly + ch > io.DisplaySize.y ) {
                    break;
                }
                float slot_x = x0;
                for( int slot = 0; slot < 2 && bi + slot < mp_mini_map.beacons.size(); slot++ ) {
                    const minimap_frame::beacon &bb = mp_mini_map.beacons[bi + slot];
                    if( bb.name.empty() ) {
                        continue;
                    }
                    dl->AddText( ImVec2( slot_x, ly ),
                                 cataimgui::ImU32_from_color(
                                     decode_cell_color( static_cast<int>( bb.color ) ) ),
                                 bb.sym.c_str() );
                    const float sym_w = ImGui::CalcTextSize( bb.sym.c_str() ).x;
                    dl->AddText( ImVec2( slot_x + sym_w + cw * 0.5f, ly ),
                                 IM_COL32( 180, 180, 180, 255 ),
                                 bb.name.c_str() );
                    slot_x += sym_w + cw * 0.5f +
                              ImGui::CalcTextSize( bb.name.c_str() ).x + cw * 1.5f;
                    ++mp_shown;
                }
                ly += ch;
            }
            // Minimap is anchored at the bottom; the log fills the space above.
            const float mp_mini_top = mp_mini_map.width > 0 ?
                                      io.DisplaySize.y - mp_mini_map.height * 2.0f - 2.0f :
                                      io.DisplaySize.y;
            const int mp_fit = static_cast<int>( ( mp_mini_top - ch - ly ) / ch );
            if( !mp_log.empty() ) {
                const int mp_shown_lines = std::max( 1, mp_fit );
                const int first = std::max( 0, static_cast<int>( mp_log.size() ) - mp_shown_lines );
                for( int i = first; i < static_cast<int>( mp_log.size() ); i++ ) {
                    const mp_log_line &ml = mp_log[static_cast<size_t>( i )];
                    // Same ageing as the host's message log (game turns):
                    // fresh -> slightly old -> dark gray.
                    const int mp_age = mp_cur_turn - ml.turn;
                    nc_color ml_col;
                    if( mp_age <= 5 ) {
                        ml_col = msgtype_to_color( static_cast<game_message_type>( ml.type ), false );
                    } else if( mp_age <= 10 ) {
                        ml_col = msgtype_to_color( static_cast<game_message_type>( ml.type ), true );
                    } else {
                        ml_col = c_dark_gray;
                    }
                    dl->AddText( ImVec2( x0, ly ),
                                 cataimgui::ImU32_from_color( ml_col ), ml.text.c_str() );
                    ly += ch;
                }
            }
            // DDA MP v4.0a-12: overmap minimap (host-coloured tiles).
            if( mp_mini_map.width > 0 && mp_mini_map.height > 0 &&
                mp_mini_map.color.size() == static_cast<size_t>( mp_mini_map.width ) *
                mp_mini_map.height ) {
                const float dot = 2.0f;
                const float mw = mp_mini_map.width * dot;
                const float mh = mp_mini_map.height * dot;
                // Center the block inside the sidebar column (like the host's panel).
                const float mp_panel_w = mp_sb_cells * cw;
                const float mx = x0 + std::max( 0.0f, ( mp_panel_w - mw ) / 2.0f );
                if( mp_mini_top + mh <= io.DisplaySize.y + 1.0f ) {
                    const float mini_y = mp_mini_top;
                    dl->AddRectFilled( ImVec2( mx, mini_y ), ImVec2( mx + mw, mini_y + mh ),
                                       IM_COL32( 0, 0, 0, 255 ) );
                    for( int yy = 0; yy < mp_mini_map.height; yy++ ) {
                        for( int xx = 0; xx < mp_mini_map.width; xx++ ) {
                            const size_t mi = static_cast<size_t>( yy ) * mp_mini_map.width + xx;
                            const int light = mi < mp_mini_map.seen.size() ?
                                              static_cast<int>( mp_mini_map.seen[mi] ) : 0;
                            if( light == 0 ) {
                                continue; // DARK/BLANK: not drawn, like the host
                            }
                            const int ci = static_cast<int>( mp_mini_map.color[mi] );
                            // LOW light is desaturated by the host; use a dim gray here.
                            const ImU32 col = light == 1 ? IM_COL32( 110, 110, 110, 255 ) :
                                              cataimgui::ImU32_from_color( decode_cell_color( ci ) );
                            dl->AddRectFilled( ImVec2( mx + xx * dot + 0.5f, mini_y + yy * dot + 0.5f ),
                                               ImVec2( mx + ( xx + 1 ) * dot - 0.5f,
                                                       mini_y + ( yy + 1 ) * dot - 0.5f ), col );
                        }
                    }
                    // Critter beacons (diamonds), like the host's minimap.
                    for( const minimap_frame::beacon &b : mp_mini_map.beacons ) {
                        const float bx = mx + ( mp_mini_map.width / 2 + b.dx ) * dot + dot / 2.0f;
                        const float by = mini_y + ( mp_mini_map.height / 2 + b.dy ) * dot + dot / 2.0f;
                        const ImU32 bcol = cataimgui::ImU32_from_color(
                                               decode_cell_color( static_cast<int>( b.color ) ) );
                        dl->AddQuadFilled( ImVec2( bx, by - dot ), ImVec2( bx + dot, by ),
                                           ImVec2( bx, by + dot ), ImVec2( bx - dot, by ), bcol );
                    }
                    // Player marker: white diamond at the centre tile.
                    const float cx = mx + ( mp_mini_map.width / 2 ) * dot + dot / 2.0f;
                    const float cy = mini_y + ( mp_mini_map.height / 2 ) * dot + dot / 2.0f;
                    dl->AddQuadFilled( ImVec2( cx, cy - dot * 0.6f ), ImVec2( cx + dot * 0.6f, cy ),
                                       ImVec2( cx, cy + dot * 0.6f ), ImVec2( cx - dot * 0.6f, cy ),
                                       IM_COL32( 255, 255, 255, 255 ) );
                }
            }
            mp_sb_overlay_w = io.DisplaySize.x - x0;
            return;
        }
        if( !mp_sheet_open || mp_sheet_text.empty() ) {
            return;
        }
        const mp_sheet_data d = mp_parse_sheet( mp_sheet_text );
        if( !d.valid ) {
            return;
        }
        const std::vector<std::string> lines = mp_sheet_lines( d );
        float maxw = 0.0f;
        for( const std::string &l : lines ) {
            maxw = std::max( maxw, ImGui::CalcTextSize( l.c_str() ).x );
        }
        const float x0 = std::max( 0.0f, io.DisplaySize.x - maxw - pad * 2.0f );
        mp_sb_overlay_w = io.DisplaySize.x - x0;
        dl->AddRectFilled( ImVec2( x0, 0.0f ), io.DisplaySize, IM_COL32( 0, 0, 0, 170 ) );
        float y = pad;
        for( const std::string &l : lines ) {
            if( y + fs > io.DisplaySize.y ) {
                break;
            }
            dl->AddText( ImVec2( x0 + pad, y ), IM_COL32( 220, 220, 220, 255 ), l.c_str() );
            y += ImGui::GetTextLineHeight();
        }
    };

    auto ensure_window = [&]( int width, int height ) {
        if( cur_w == width && cur_h == height ) {
            return;
        }
        cur_w = width;
        cur_h = height;
        win = catacurses::newwin( cur_h + 2, cur_w + 2,
                                  point( std::max( 0, ( TERMX - ( cur_w + 2 ) ) / 2 ),
                                         std::max( 0, ( TERMY - ( cur_h + 2 ) ) / 2 ) ) );
    };

    while( !voluntary_quit ) {
        input_context ctxt( "NETPLAY_VIEW", keyboard_mode::keycode );
        // DDA MP v4.0a-7: always redraw at ~33 fps. A step's picture arrives
        // between world turns; polling only every 150 ms made it appear "after
        // the tick" depending on when it landed.
        ctxt.set_timeout( 30 );
        ctxt.register_action( "QUIT" );
        ctxt.register_action( "ANY_INPUT" );
        ctxt.register_directions();

        // DDA MP v4.0a-9: report our terminal size so the host renders the
        // sidebar to fit instead of letting the client clip it. Declared per
        // connection so a reconnect always resends it to the new session.
        int mp_last_cols = -1;
        int mp_last_rows = -1;

        while( client_is_connected() && !voluntary_quit ) {
            {
                int mp_cols = TERMX;
                int mp_rows = TERMY;
                // Curses-only builds report 0x0; keep the terminal fallback.
                int dc = 0;
                int dr = 0;
                netplay::drawable_cells( dc, dr );
                if( dc > 0 && dr > 0 ) {
                    mp_cols = dc;
                    mp_rows = dr;
                }
                // DDA MP v4.0a-117: the sidebar is drawn natively beside the
                // map, so the host must render only the map columns. Without
                // this the frame covers the sidebar's column and the panels
                // overlap the right edge of the map.
                if( mp_native_render_enabled() ) {
                    const int sb_cols = mp_native_sidebar_width();
                    if( sb_cols > 0 && mp_cols > sb_cols + 16 ) {
                        mp_cols -= sb_cols;
                    }
                }
                if( mp_cols != mp_last_cols || mp_rows != mp_last_rows ) {
                    mp_last_cols = mp_cols;
                    mp_last_rows = mp_rows;
                    netplay::client_send_view( mp_cols, mp_rows );
                }
            }
            // DDA MP v4.0a-88: long wait no longer sends WAIT orders; the client
            // only tells the host it is AFK (see the menu toggle), so the active
            // player's actions drive the world turns one by one.
            // DDA MP v4.0a-7: turn-pacing hint from the host ("waiting for
            // other players..."), shown above the picture and in the title.
            {
                std::string mp_hint;
                if( netplay::client_take_hover( mp_hint ) ) {
                    // Translate the host's hover line into the client's
                    // language (e.g. the host's English "Waiting for other
                    // players..."). The host long-wait case is suppressed on
                    // the host side entirely.
                    const std::string mp_hint_local = to_translation( mp_hint ).translated();
                    mp_hint_text = mp_hint_local;
                    netplay::client_status_hint( mp_hint_local );
                }
                std::string mp_sheet;
                if( netplay::client_sheet( mp_sheet ) ) {
                    mp_sheet_text = std::move( mp_sheet );
                    const mp_sheet_data d = mp_parse_sheet( mp_sheet_text );
                    if( d.valid ) {
                        if( d.turn > 0 ) {
                            mp_cur_turn = d.turn;
                        }
                        if( mp_prev.valid ) {
                            if( d.hp < mp_prev.hp ) {
                                mp_log_add( string_format( netplay::fork_text( "Lost %d HP" ),
                                                           mp_prev.hp - d.hp ),
                                            static_cast<int>( m_bad ) );
                            } else if( d.hp > mp_prev.hp ) {
                                mp_log_add( string_format( netplay::fork_text( "Regained %d HP" ),
                                                           d.hp - mp_prev.hp ),
                                            static_cast<int>( m_good ) );
                            }
                            if( d.hunger >= mp_prev.hunger + 10 ) {
                                mp_log_add( string_format( netplay::fork_text( "Hunger: %d" ), d.hunger ),
                                            static_cast<int>( m_warning ) );
                            }
                            if( d.thirst >= mp_prev.thirst + 10 ) {
                                mp_log_add( string_format( netplay::fork_text( "Thirst: %d" ), d.thirst ),
                                            static_cast<int>( m_info ) );
                            }
                            if( d.pain > mp_prev.pain ) {
                                mp_log_add( string_format( netplay::fork_text( "Pain: %d" ), d.pain ),
                                            static_cast<int>( m_bad ) );
                            }
                            if( d.effects != mp_prev.effects ) {
                                mp_log_add( netplay::fork_text( "Condition changed" ),
                                            static_cast<int>( m_info ) );
                            }
                        }
                        mp_prev = d;
                    }
                }
                // Host-rendered panels are no longer streamed: the client
                // draws the same panels itself from its character mirror.
                netplay::client_minimap( mp_mini_map );
                if( netplay::mp_diag_enabled() ) {
                    // Forensics: did the minimap data actually change?
                    static std::size_t mp_mini_hash = 0;
                    std::size_t h = 0;
                    for( uint8_t c : mp_mini_map.color ) {
                        h = h * 131u + c;
                    }
                    for( uint8_t c : mp_mini_map.seen ) {
                        h = h * 131u + c;
                    }
                    if( h != mp_mini_hash ) {
                        mp_mini_hash = h;
                        mp_inv_log( string_format( "minimap update h=%zu w=%d h=%d",
                                                   h, mp_mini_map.width, mp_mini_map.height ) );
                    }
                }
                mp_aim_minimap = mp_mini_map;
                netplay::client_aim_targets( mp_aim_targets );
                {
                    std::string mp_inv;
                    if( netplay::client_inventory( mp_inv ) ) {
                        mp_inv_text = std::move( mp_inv );
                    }
                }
                // DDA MP v4.0a-90: a pickup listing from the host opens the
                // client's own selector right away.
                {
                    std::string mp_pick;
                    if( netplay::client_pickup_packet( mp_pick ) ) {
                        mp_pick_tiles_active = false;
                        mp_run_pickup( mp_pick );
                    }
                }
                // DDA MP v4.0a-97: several adjacent tiles have items - ask
                // which one to pick from before listing them.
                {
                    std::string mp_tiles;
                    if( netplay::client_pickup_tiles( mp_tiles ) ) {
                        std::istringstream iss( mp_tiles );
                        std::string line;
                        std::vector<point> cand;
                        int gen = 0;
                        if( std::getline( iss, line ) ) {
                            try {
                                gen = std::stoi( line );
                            } catch( ... ) {
                                gen = 0;
                            }
                        }
                        while( std::getline( iss, line ) ) {
                            int dx = 0;
                            int dy = 0;
                            if( std::sscanf( line.c_str(), "%d %d", &dx, &dy ) == 2 ) {
                                cand.emplace_back( dx, dy );
                            }
                        }
                        if( gen != 0 && !cand.empty() ) {
                            mp_pick_gen = gen;
                            mp_pick_tiles = std::move( cand );
                            mp_pick_sel = 0;
                            mp_pick_tiles_active = true;
                            mp_tlog( string_format( "picktile: %d candidates gen=%d",
                                                    static_cast<int>( mp_pick_tiles.size() ), gen ) );
                        }
                    }
                }
                {
                    std::string mp_cj;
                    if( netplay::client_character_json( mp_cj ) ) {
                        mp_char_json = std::move( mp_cj );
                        mp_char_json_cache = mp_char_json;
                    }
                }
                {
                    std::string mp_ai;
                    if( netplay::client_aim_info( mp_ai ) ) {
                        mp_aim_text = std::move( mp_ai );
                    }
                }
                // Events for the client's own log.
                std::string mp_ev_text;
                int mp_ev_type = 0;
                while( netplay::client_take_event( mp_ev_text, mp_ev_type ) ) {
                    if( mp_ev_type == netplay::EVENT_DEATH ) {
                        mp_dead = true;
                        mp_log_add( netplay::format_event_text( mp_ev_text ), mp_ev_type );
                        continue;
                    }
                    // DDA MP v4.0a-176: the host reports that our avatar walked
                    // into another player. Open the interaction menu instead of
                    // logging the payload as a message.
                    if( mp_ev_type == netplay::EVENT_PLAYER ) {
                        const std::string::size_type sep = mp_ev_text.find( '\x1f' );
                        const std::string p_name = sep == std::string::npos
                                                   ? mp_ev_text : mp_ev_text.substr( 0, sep );
                        const std::string p_act = sep == std::string::npos
                                                  ? std::string() : mp_ev_text.substr( sep + 1 );
                        mp_run_player_interact( p_name, p_act );
                        continue;
                    }
                    // DDA MP v4.0a-177: the host is the authority on whether
                    // we are helping; reset the stop-confirmation flag when it
                    // ends the assistance (finished task, walked away, refused).
                    if( mp_ev_type == netplay::EVENT_HELP ) {
                        mp_client_helping = mp_ev_text == "on";
                        mp_inv_log( "help state: " + mp_ev_text );
                        continue;
                    }
                    // DDA MP v4.0a-188: a common NPC in the target tile opens
                    // the inspection menu; payload is name\x1fdx\x1fdy.
                    if( mp_ev_type == netplay::EVENT_NPC ) {
                        const std::string::size_type s1 = mp_ev_text.find( '\x1f' );
                        const std::string::size_type s2 = s1 == std::string::npos
                                                          ? std::string::npos
                                                          : mp_ev_text.find( '\x1f', s1 + 1 );
                        const std::string p_name = s1 == std::string::npos
                                                   ? mp_ev_text : mp_ev_text.substr( 0, s1 );
                        int ndx = 0;
                        int ndy = 0;
                        if( s2 != std::string::npos ) {
                            ndx = std::atoi( mp_ev_text.substr( s1 + 1, s2 - s1 - 1 ).c_str() );
                            ndy = std::atoi( mp_ev_text.substr( s2 + 1 ).c_str() );
                        }
                        mp_run_npc_interact( p_name, ndx, ndy );
                        continue;
                    }
                    // DDA MP v4.0a-189: one dialogue step from the host.
                    if( mp_ev_type == netplay::EVENT_DIALOGUE ) {
                        mp_run_dialogue( mp_ev_text );
                        continue;
                    }
                    // DDA MP v4.0a-191: mission offers / journal.
                    if( mp_ev_type == netplay::EVENT_MISSION_OFFERS ) {
                        mp_run_mission_offers( mp_ev_text );
                        continue;
                    }
                    if( mp_ev_type == netplay::EVENT_MISSION_JOURNAL ) {
                        mp_run_mission_journal( mp_ev_text );
                        continue;
                    }
                    if( mp_ev_type == netplay::EVENT_MISSION_TURNIN ) {
                        mp_run_mission_turnins( mp_ev_text );
                        continue;
                    }
                    if( mp_ev_type == netplay::EVENT_TRADE_LIST ) {
                        mp_run_trade( mp_ev_text );
                        continue;
                    }
                    if( mp_ev_type == netplay::EVENT_VEHICLE_INFO ) {
                        mp_run_vehicle_info( mp_ev_text );
                        continue;
                    }
                    if( mp_ev_type == netplay::EVENT_VEHICLE_PARTS ) {
                        mp_run_vehicle_parts( mp_ev_text );
                        continue;
                    }
                    if( mp_ev_type == netplay::EVENT_ZONES ) {
                        mp_run_zones( mp_ev_text );
                        continue;
                    }
                    mp_log_add( netplay::format_event_text( mp_ev_text ), mp_ev_type );
                }
                if( mp_dead ) {
                    mp_dead = false;
                    // DDA MP v4.0a-216: the single-player death screen, then
                    // a fresh character for this nick — same session, no
                    // reconnect. The host keeps serving the other sessions.
                    const char *test_auto = std::getenv( "DDA_MP_TEST_AVATAR_AUTO" );
                    const bool headless = test_auto != nullptr && test_auto[0] != '\0'
                                          && std::string( test_auto ) != "0";
                    if( headless ) {
                        mp_inv_log( "death: screens skipped (headless)" );
                    } else {
                        Messages::display_messages();
                        show_scores_ui();
                    }
                    avatar &dead_pc = get_avatar();
                    dead_pc = avatar();
                    mp_native_avatar_ready = false;
                    if( mp_session_nick.empty() ||
                        !run_avatar_join_flow( mp_session_nick ) ) {
                        // Creation cancelled or refused: leave like before.
                        voluntary_quit = true;
                        break;
                    }
                    mp_have_picture = false;
                    netplay::client_request_snapshot();
                    continue;
                }
            }
#if defined(TILES)
            // DDA MP v3.9: service any queued renderer-recovery work before
            // drawing. The game's own loop does this per tick and inside the
            // input pump; the view loop previously relied on the input pump
            // alone, so frames queued during the fat-client load stayed
            // undelivered until the first keypress.
            drain_renderer_recovery();
            // Clear stale UI remnants (e.g. debug-prompt text no window
            // repaints) before the windows redraw into a clean buffer.
            if( tiles_backend != nullptr && netplay::tiles_replay_enabled() ) {
                netplay::tiles_backend_clear_screen( tiles_backend );
            }
#endif
            // Ask for the current picture until the host answers once.
            mp_request_picture();

            // DDA MP v3.9: redraw() MUST run every iteration. The per-frame
            // clear_screen bumps the curses render epoch and invalidates all
            // ui adaptors; redraw() regenerates the registered windows here,
            // right before the tileset canvas stamps over them (that covered
            // form is proven stable). Removing it made the invalidated
            // windows repaint at random moments inside the input pump -
            // the screen alternated between stamped tiles and the cleared
            // black buffer (the observed flicker).
            // DDA MP v4.1: keep the UI redraw and the tileset canvas inside one
            // display-buffer scope. Previously each pass opened its own scope;
            // two bind/unbind cycles per iteration left the presented buffer on
            // a frame boundary, which flickered for a frame and stuck on the
            // wrong buffer once mouse motion changed the timing.
            // DDA MP v4.2: apply the avatar/creature mirrors before the UI
            // redraw. Their deserialization touches UI caches; doing it between
            // the window draw and the present cleared the whole frame.
            {
                // The character mirror is state, not rendering: keep g->u in
                // sync even on the legacy ascii path so the inventory and the
                // @ menu run on the same character as the native sidebar.
                static const bool mp_avatar_on = []() {
                    const char *v = std::getenv( "DDA_MP_NATIVE_AVATAR" );
                    return v == nullptr || v[0] == '\0' || std::string( v ) != "0";
                }();
                if( mp_avatar_on ) {
                    mp_native_apply_avatar_json();
                }
            }
            if( mp_native_render_enabled() ) {
                mp_draw_native_sidebar_bg();
            }
            ui_manager::redraw();

            // DDA MP test hook: walk N times without touching the OS window.
            // It uses the same client_send_keys path as a real key, so the
            // desktop focus never has to be stolen for automated runs. Kept
            // outside the "new snapshot" branch: while the host round gate
            // waits for this client's order it publishes no new pictures, and
            // a hook inside that branch could never send the order it waits
            // for.
            if( const char *test_move = std::getenv( "DDA_MP_TEST_CLIENT_MOVE" );
                test_move != nullptr && test_move[0] != '\0' ) {
                static int mp_test_moves_left = std::atoi( test_move );
                static const int mp_test_move_code = []() {
                    const char *dir = std::getenv( "DDA_MP_TEST_CLIENT_MOVE_DIR" );
                    const int code = dir != nullptr && dir[0] != '\0' ? std::atoi( dir ) : 2;
                    return code == 1 || code == 3 || code == 4 ? code : 2;
                }();
                static const bool mp_test_move_cycle = []() {
                    const char *dir = std::getenv( "DDA_MP_TEST_CLIENT_MOVE_DIR" );
                    return dir != nullptr && std::string( dir ) == "cycle";
                }();
                static int mp_test_cycle_index = 0;
                static auto mp_test_last_move =
                    std::chrono::steady_clock::now() - std::chrono::seconds( 10 );
                const auto mp_test_now = std::chrono::steady_clock::now();
                if( mp_test_moves_left > 0 &&
                    mp_test_now - mp_test_last_move > std::chrono::seconds( 2 ) ) {
                    mp_test_last_move = mp_test_now;
                    --mp_test_moves_left;
                    int step_code = mp_test_move_code;
                    if( mp_test_move_cycle ) {
                        // 6 steps per direction: E, S, W, N, then repeat.
                        static const int cycle[4] = { 2, 4, 1, 3 };
                        step_code = cycle[( mp_test_cycle_index / 6 ) % 4];
                        ++mp_test_cycle_index;
                    }
                    key_event test_ke;
                    test_ke.code = static_cast<uint32_t>( step_code );
                    client_send_keys( { test_ke } );
                    mp_tlog( string_format( "test: synthetic step %d", step_code ) );
                }
            }

            // DDA MP test hook: ask the same NPC for the turn-in list a few
            // seconds after the auto-accepted mission exists.
            if( const char *auto_turnin = std::getenv( "DDA_MP_TEST_TURNIN_AUTO" );
                auto_turnin != nullptr && auto_turnin[0] != '\0' ) {
                static bool mp_test_turnin_asked = false;
                const auto mp_turnin_now = std::chrono::steady_clock::now();
                if( !mp_test_turnin_asked &&
                    mp_test_mission_accepted.time_since_epoch().count() != 0 &&
                    mp_turnin_now - mp_test_mission_accepted >
                    std::chrono::seconds( 5 ) ) {
                    mp_test_turnin_asked = true;
                    netplay::client_send_intent(
                        "mission_turnin_list", 0,
                        { mp_mission_dx, mp_mission_dy } );
                    mp_inv_log( "test: turnin list auto" );
                }
            }

            bool mp_canvas_ok = false;
            snapshot snap;
            // DDA MP v4.1: never present a frame without the sprite payload if
            // the previous one had it. The host omits the optional tile array on
            // some snapshots; drawing the ASCII/black grid for that one frame is
            // exactly the flash seen while the screen is busy. Reuse the last
            // tile payload until a fresh one arrives.
            static std::vector<tile_cell> mp_tiles_cache;
            static int mp_tiles_w = 0;
            static int mp_tiles_h = 0;
            if( client_receive( snap ) && snap.width > 0 && snap.height > 0
                && snap.width <= SNAP_MAX && snap.height <= SNAP_MAX
                && snap.cells.size() == static_cast<size_t>( snap.width ) * snap.height ) {
                if( !snap.tiles.empty() &&
                    snap.tiles.size() == static_cast<size_t>( snap.width ) * snap.height ) {
                    mp_tiles_cache = snap.tiles;
                    mp_tiles_w = snap.width;
                    mp_tiles_h = snap.height;
                } else if( mp_tiles_w == snap.width && mp_tiles_h == snap.height &&
                           mp_tiles_cache.size() == snap.cells.size() ) {
                    snap.tiles = mp_tiles_cache;
                }
                // Dimensions feed the canvas centering below even without the
                // ASCII window. A valid picture also stops the snapshot
                // request loop above.
                mp_have_picture = true;
                if( snap.has_origin ) {
                    mp_origin_x = snap.origin_x;
                    mp_origin_y = snap.origin_y;
                    mp_origin_z = snap.origin_z;
                    mp_have_origin = true;
                }
                if( mp_throw_active ) {
                    mp_aim_w = snap.width;
                    mp_aim_h = snap.height;
                    mp_aim_flags.assign( static_cast<size_t>( snap.width ) * snap.height, 0 );
                    mp_aim_critters.assign( static_cast<size_t>( snap.width ) * snap.height,
                                            std::string() );
                    mp_aim_light.assign( static_cast<size_t>( snap.width ) * snap.height, 0 );
                    mp_aim_badges.assign( static_cast<size_t>( snap.width ) * snap.height, 0 );
                    mp_aim_glyphs.assign( static_cast<size_t>( snap.width ) * snap.height, 0 );
                    for( size_t ci = 0; ci < snap.tiles.size() &&
                         ci < mp_aim_critters.size(); ci++ ) {
                        mp_aim_critters[ci] = snap.tiles[ci].critter;
                        mp_aim_light[ci] = static_cast<int>( snap.tiles[ci].light );
                        mp_aim_badges[ci] = snap.tiles[ci].critter_badge;
                    }
                    for( size_t ci = 0; ci < snap.cells.size() && ci < mp_aim_glyphs.size(); ci++ ) {
                        mp_aim_glyphs[ci] = snap.cells[ci].glyph;
                    }
                    if( snap.tiles.size() == snap.cells.size() ) {
                        for( size_t ti = 0; ti < snap.tiles.size(); ti++ ) {
                            mp_aim_flags[ti] = snap.tiles[ti].flags;
                        }
                    }
                    mp_client_aim_recalc();
                }
                ensure_window( snap.width, snap.height );
                // DDA MP diag: when did the client actually adopt a picture
                // (signature of a few cells tells changed frames apart).
                if( mp_netplay_diag ) {
                    static int mp_rx_count = 0;
                    static std::size_t mp_last_hash = 0;
                    std::size_t mp_hash = 1469598103934665603ull;
                    for( const cell &cc : snap.cells ) {
                        mp_hash ^= static_cast<std::size_t>( cc.glyph );
                        mp_hash *= 1099511628211ull;
                        mp_hash ^= static_cast<std::size_t>( cc.color );
                        mp_hash *= 1099511628211ull;
                    }
                    if( mp_rx_count < 400 && mp_hash != mp_last_hash ) {
                        mp_last_hash = mp_hash;
                        ++mp_rx_count;
                        const long long wms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                                  std::chrono::system_clock::now().time_since_epoch() ).count();
                        mp_tlog( string_format( "cli snap wms=%lld #%d hash=%llx tiles=%zu",
                                                wms, mp_rx_count,
                                                static_cast<unsigned long long>( mp_hash ),
                                                snap.tiles.size() ) );
                    }
                }
                if( mp_ascii_debug ) {
                    werase( win );
                    draw_border( win, c_white, netplay::fork_text( "Cata server" ) );

                    for( int y = 0; y < snap.height; y++ ) {
                        for( int x = 0; x < snap.width; x++ ) {
                            const cell &c = snap.cells[y * snap.width + x];
                            mvwputch( win, point( 1 + x, 1 + y ),
                                      decode_cell_color( c.color ),
                                      static_cast<int>( c.glyph ) );
                        }
                    }
                    // Status line rides the bottom border row.
                    if( !snap.status_line.empty() ) {
                        std::string s = snap.status_line;
                        if( static_cast<int>( s.size() ) > cur_w ) {
                            s.resize( cur_w );
                        }
                        mvwprintz( win, point( 1, cur_h + 1 ), c_yellow, s );
                    }
                    wnoutrefresh( win );
                }

#if defined(TILES)
                // Tileset replay: draw the sprite payload on top of the fresh
                // ascii frame. Any failure here (renderer state, missing
                // tileset, exception...) must never kill the session: the
                // ascii frame underneath stays valid, so on error we drop the
                // tileset backend permanently and continue ascii-only.
                if( !snap.tiles.empty() && snap.tiles.size() == snap.cells.size()
                    && netplay::tiles_replay_enabled() ) {
                    try {
                        if( font_px_w == 0 ) {
                            netplay::tiles_backend_font_size( font_px_w, font_px_h );
                        }
                        if( tiles_backend == nullptr ) {
                            // DDA MP fat client: normally already loaded in
                            // show_join_dialog / before reconnect; this call
                            // is idempotent and covers a late-arriving WELCOME.
                            fat_client_load_data();
                            mp_tlog( "tiles: creating backend..." );
                            tiles_backend = netplay::tiles_backend_create();
                            mp_tlog( tiles_backend != nullptr ? "tiles: create ok"
                                      : "tiles: create FAILED (backend nil)" );
                        }
                        int tw = 0;
                        int th = 0;
                        if( tiles_backend != nullptr ) {
                            netplay::tiles_backend_tile_size( tiles_backend, tw, th );
                        }
                        if( tiles_backend != nullptr && tw > 0 && th > 0
                            && font_px_w > 0 && font_px_h > 0 ) {
                            // Center the sprite canvas on the window's interior
                            // (the same place the ascii grid and the host avatar
                            // sit). The pivot cell is the host avatar's cell.
                            const int win_x = std::max( 0, ( TERMX - ( cur_w + 2 ) ) / 2 );
                            const int win_y = std::max( 0, ( TERMY - ( cur_h + 2 ) ) / 2 );
                            const int center_px_x = ( win_x + cur_w / 2 + 1 ) * font_px_w;
                            const int center_px_y = ( win_y + cur_h / 2 + 1 ) * font_px_h;
                            const int canvas_w = snap.width * tw;
                            const int canvas_h = snap.height * th;
                            // DDA MP v4.0a-117: the map occupies the area left
                            // of the native sidebar; center it there instead of
                            // centering over the whole screen.
                            int dest_x = 0;
                            int dest_y = 0;
                            if( mp_native_render_enabled() ) {
                                const int sb_cells = mp_native_sidebar_width();
                                const int area_w = std::max( 0, TERMX * font_px_w -
                                                             sb_cells * font_px_w );
                                dest_x = std::max( 0, ( area_w - canvas_w ) / 2 );
                                dest_y = std::max( 0, ( TERMY * font_px_h - canvas_h ) / 2 );
                                mp_canvas_cx = static_cast<float>( dest_x ) + canvas_w / 2.0f;
                                mp_canvas_cy = static_cast<float>( dest_y ) + canvas_h / 2.0f;
                            } else {
                                // Center the sprite canvas on the window's
                                // interior and shift it left by half the
                                // sidebar width for the streamed layout.
                                const int win_x = std::max( 0, ( TERMX - ( cur_w + 2 ) ) / 2 );
                                const int win_y = std::max( 0, ( TERMY - ( cur_h + 2 ) ) / 2 );
                                const int center_px_x = ( win_x + cur_w / 2 + 1 ) * font_px_w;
                                const int center_px_y = ( win_y + cur_h / 2 + 1 ) * font_px_h;
                                dest_x = center_px_x - canvas_w / 2 -
                                         static_cast<int>( mp_sb_overlay_w / 2.0f );
                                dest_y = center_px_y - canvas_h / 2;
                                mp_canvas_cx = static_cast<float>( center_px_x ) -
                                               mp_sb_overlay_w / 2.0f;
                                mp_canvas_cy = static_cast<float>( center_px_y );
                            }
                            mp_canvas_tw = static_cast<float>( tw );
                            mp_canvas_th = static_cast<float>( th );
                            constexpr int PIVOT = 32; // inside the 48-tile bubble
                            // DDA MP v4.0a-4: feed the streamed absolute origin
                            // so sprite variant seeds are world-stable.
                            netplay::tiles_backend_set_origin( tiles_backend,
                                                               snap.origin_x, snap.origin_y,
                                                               snap.origin_z, snap.has_origin );
                            if( mp_native_render_enabled() ) {
                                // DDA MP v4.2: native path. Mirror the map and
                                // mark the terrain window dirty; the map itself
                                // is drawn by curses_drawwindow ->
                                // cata_tiles::draw during the regular present.
                                mp_native_apply_snapshot( snap );
                                mp_native_setup_terrain_window( snap, dest_x, dest_y, tw, th,
                                                                font_px_w, font_px_h );
                                mp_canvas_ok = true;
                            } else if( netplay::tiles_backend_begin( tiles_backend, dest_x, dest_y,
                                    canvas_w, canvas_h, PIVOT, PIVOT ) ) {
                                mp_canvas_ok = true;
                                mp_frame_trace( "begin ok" );
                                // DDA MP v4.2: draw the frame with the game's
                                // own renderer over the map mirror. When it
                                // succeeds the replay pass only adds the
                                // pre-resolved creature overlays; terrain,
                                // items and lighting come from cata_tiles.
                                const bool mp_native = mp_native_render_enabled() &&
                                                       mp_native_draw_map( tiles_backend, snap, dest_x, dest_y, tw, th );
                                // DDA MP debug: sizes of the snapshot feeding
                                // this frame (the light prepass and the draw
                                // loops both iterate snap.width/height and
                                // tile the tiles vector with that stride).
                                mp_tlog( string_format(
                                             "snap #%d w=%d h=%d tiles=%zu",
                                             trace_frames, snap.width, snap.height,
                                             snap.tiles.size() ) );
                                static int mp_skip_log_frames = 0;
                                auto mp_trace_frame_early = [&]() {
                                    return mp_skip_log_frames < 2;
                                };
                                if( mp_trace_frame_early() ) {
                                    ++mp_skip_log_frames;
                                }
                                static int trace_draws = 0;
                                auto trace_draw = [&]( int kind, const char *id ) {
                                    if( trace_draws < 400 ) {
                                        ++trace_draws;
                                        mp_tlog( string_format( "draw #%d kind=%d id=%s",
                                                              trace_draws, kind, id ) );
                                    }
                                };
                                // DDA MP: prepass - stream every cell's
                                // light texel color (the host's own
                                // smooth-lighting values), then bind the lit
                                // frame so the sprites take the real GPU lit
                                // path (same gray/tone/colored-light/memory
                                // formulas as the host's own renderer).
                                // Cells without valid light bytes (host
                                // without smooth lighting) leave the classic
                                // fallback on.
                                for( int y = 0; y < snap.height; y++ ) {
                                    for( int x = 0; x < snap.width; x++ ) {
                                        const tile_cell &t = snap.tiles[y * snap.width + x];
                                        if( !( t.flags & 16 ) ) {
                                            // DDA MP debug: count the cells the
                                            // prepass skips (no valid light
                                            // bytes) for the first frames, so
                                            // the skip pattern can be checked
                                            // cell for cell.
                                            if( mp_trace_frame_early() ) {
                                                mp_tlog( string_format(
                                                             "skip w=%d h=%d y=%d x=%d flags=%d",
                                                             snap.width, snap.height, y, x,
                                                             static_cast<int>( t.flags ) ) );
                                            }
                                            continue;
                                        }
                                        netplay::tiles_backend_set_cell_light(
                                            tiles_backend, x - snap.width / 2,
                                            y - snap.height / 2,
                                            static_cast<int>( t.light ),
                                            t.light_r, t.light_g, t.light_b,
                                            ( t.flags & 8 ) != 0,
                                            ( t.flags & 32 ) != 0 );
                                    }
                                }
                                const bool mp_light_ok = netplay::tiles_backend_enable_lighting(
                                        tiles_backend, snap.width, snap.height );
                                mp_frame_trace( mp_light_ok ? "begin ok (lit)"
                                                : "begin ok (classic light)" );
                                for( int y = 0; y < snap.height; y++ ) {
                                    for( int x = 0; x < snap.width; x++ ) {
                                        const tile_cell &t = snap.tiles[y * snap.width + x];
                                        if( t.ter.empty() && t.furn.empty() && t.trap.empty() &&
                                            t.critter.empty() && t.item.empty() &&
                                            t.vpart.empty() && !( t.flags & 1 ) ) {
                                            continue;
                                        }
                                        // DDA MP: pass the cell's real lit_level -
                                        // the lit frame (or, in the fallback, the
                                        // overlay below) replicas the host's light.
                                        const int mp_lit = static_cast<int>( t.light );
                                        if( !mp_native && !t.ter.empty() ) {
                                            trace_draw( 0, t.ter.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 0,
                                                    t.ter.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit,
                                                    static_cast<int>( t.ter_sub ),
                                                    static_cast<int>( t.ter_rot ) );
                                        }
                                        if( !mp_native && !t.trap.empty() ) {
                                            trace_draw( 5, t.trap.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 5,
                                                    t.trap.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, 0 );
                                        }
                                        if( !mp_native && !t.furn.empty() ) {
                                            if( t.furn.rfind( "vp_", 0 ) == 0 ) {
                                                // Legacy (pre-v3.3) vehicle part
                                                // carried in the furniture slot:
                                                // same treatment as t.vpart.
                                                std::string vp_id = t.furn;
                                                std::string vp_variant;
                                                const size_t sep = vp_id.find( '#' );
                                                if( sep != std::string::npos ) {
                                                    vp_variant = vp_id.substr( sep + 1 );
                                                    vp_id.resize( sep );
                                                }
                                                trace_draw( 4, vp_id.c_str() );
                                                netplay::tiles_backend_draw( tiles_backend, 4,
                                                        vp_id.c_str(), vp_variant.c_str(),
                                                        x - snap.width / 2, y - snap.height / 2,
                                                        mp_lit,
                                                        static_cast<int>( t.furn_sub ),
                                                        static_cast<int>( t.furn_rot ) );
                                            } else {
                                                trace_draw( 1, t.furn.c_str() );
                                                netplay::tiles_backend_draw( tiles_backend, 1,
                                                        t.furn.c_str(), "", x - snap.width / 2,
                                                        y - snap.height / 2, mp_lit,
                                                        static_cast<int>( t.furn_sub ),
                                                        static_cast<int>( t.furn_rot ) );
                                            }
                                        }
                                        // DDA MP v3.9.1: tileset item-layer
                                        // sprites first - draw_field_or_item
                                        // draws the "layering.json" layers
                                        // (sink dishes, cupboard clutter...)
                                        // before the plain top item.
                                        if( !mp_native ) for( const tile_item_layer &l : t.item_layers ) {
                                            trace_draw( 2, l.sprite.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 2,
                                                    l.sprite.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, 0, false,
                                                    l.variant.c_str(),
                                                    static_cast<int>( l.dx ),
                                                    static_cast<int>( l.dy ) );
                                        }
                                        if( !mp_native && !t.item.empty() && !t.item_skip_top ) {
                                            trace_draw( 2, t.item.c_str() );
                                            // v3.9.1: the itype variant the host
                                            // drew this item with (see tile_cell).
                                            netplay::tiles_backend_draw( tiles_backend, 2,
                                                    t.item.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, 0,
                                                    false, t.item_variant.c_str() );
                                        }
                                        if( !mp_native && !t.vpart.empty() ) {
                                            // Tileset id keeps the variant in
                                            // "vp_{id}#{variant}" form; the
                                            // vehicle-part path expects the
                                            // variant separately.
                                            std::string vp_id = t.vpart;
                                            std::string vp_variant;
                                            const size_t sep = vp_id.find( '#' );
                                            if( sep != std::string::npos ) {
                                                vp_variant = vp_id.substr( sep + 1 );
                                                vp_id.resize( sep );
                                            }
                                            trace_draw( 4, vp_id.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 4,
                                                    vp_id.c_str(), vp_variant.c_str(),
                                                    x - snap.width / 2, y - snap.height / 2,
                                                    mp_lit,
                                                    static_cast<int>( t.vpart_sub ),
                                                    static_cast<int>( t.vpart_rot ) );
                                        }
                                        if( !t.critter.empty() ) {
                                            const int kind = ( t.critter.rfind( "player_", 0 ) == 0 ||
                                                               t.critter.rfind( "npc_", 0 ) == 0 ) ? 6 : 3;
                                            // DDA MP v3.8: facing flip and the
                                            // clothing/effect overlays the host
                                            // renderer draws (draw_entity_with_overlays
                                            // facts streamed per cell). -2 means no
                                            // facing: draw the static sprite like before.
                                            const int rot = t.critter_rot == -1 ? -1 :
                                                            t.critter_rot == 0 ? 0 : 0;
                                            {
                                                static int mp_cdraw_cnt = 0;
                                                if( mp_cdraw_cnt < 150 ) {
                                                    ++mp_cdraw_cnt;
                                                    mp_tlog( string_format(
                                                                 "CD n=%d y=%d x=%d kind=%d id=%s rot=%d badge=%d ovl=%s",
                                                                 mp_cdraw_cnt, y, x, kind,
                                                                 t.critter.c_str(), rot,
                                                                 static_cast<int>( t.critter_badge ),
                                                                 t.critter_overlays.c_str() ) );
                                                }
                                            }
                                            trace_draw( kind, t.critter.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, kind,
                                                    t.critter.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, rot );
                                            if( !t.critter_overlays.empty() ) {
                                                std::string ov = t.critter_overlays;
                                                size_t sep;
                                                while( ( sep = ov.find( ';' ) ) != std::string::npos ) {
                                                    const std::string ovl = ov.substr( 0, sep );
                                                    ov.erase( 0, sep + 1 );
                                                    if( ovl.empty() ) {
                                                        continue;
                                                    }
                                                    trace_draw( 6, ovl.c_str() );
                                                    netplay::tiles_backend_draw( tiles_backend, 6,
                                                            ovl.c_str(), "",
                                                            x - snap.width / 2,
                                                            y - snap.height / 2, mp_lit, 0, rot );
                                                }
                                                if( !ov.empty() ) {
                                                    trace_draw( 6, ov.c_str() );
                                                    netplay::tiles_backend_draw( tiles_backend, 6,
                                                            ov.c_str(), "",
                                                            x - snap.width / 2,
                                                            y - snap.height / 2, mp_lit, 0, rot );
                                                }
                                            }
                                            if( t.critter_badge != 0 ) {
                                                // DDA MP v3.8.1: the attitude icon
                                                // the host paints over the drawn
                                                // critter (draw_critter_at): fixed
                                                // light at LIT, no facing flip.
                                                const Creature::Attitude att = static_cast<Creature::Attitude>(
                                                                                   t.critter_badge >> 1 );
                                                const std::string bid = "overlay_" +
                                                        Creature::attitude_raw_string( att ) +
                                                        ( ( t.critter_badge & 1 ) != 0 ?
                                                          "_sees_player" : "" );
                                                netplay::tiles_backend_draw( tiles_backend, 6,
                                                        bid.c_str(), "", x - snap.width / 2,
                                                        y - snap.height / 2,
                                                        static_cast<int>( lit_level::LIT ), 0, 0,
                                                        true );
                                            }
                                        }
                                        if( !mp_light_ok ) {
                                            netplay::tiles_backend_cell_light( tiles_backend,
                                                    x - snap.width / 2, y - snap.height / 2,
                                                    static_cast<int>( t.light ) );
                                        }
                                        if( t.flags & 2 ) {
                                            // Pile marker: the game also draws the
                                            // highlight_item badge only when a tile
                                            // holds more than one item. Fixed light
                                            // (draw_light::fixed at LIT): bright
                                            // like the host's draw_item_highlight.
                                            trace_draw( 6, "highlight_item" );
                                            netplay::tiles_backend_draw( tiles_backend, 6,
                                                    "highlight_item", "", x - snap.width / 2,
                                                    y - snap.height / 2,
                                                    static_cast<int>( lit_level::LIT ), 0, 0,
                                                    true );
                                        }
                                        if( !t.vpart.empty() && ( t.flags & 4 ) ) {
                                            // Cargo part with items inside: the
                                            // game draws the badge on the part
                                            // tile too (cata_tiles::draw_vpart).
                                            trace_draw( 6, "highlight_item" );
                                            netplay::tiles_backend_draw( tiles_backend, 6,
                                                    "highlight_item", "",
                                                    x - snap.width / 2, y - snap.height / 2,
                                                    static_cast<int>( lit_level::LIT ), 0, 0,
                                                    true );
                                        }
                                        if( t.marker != 0 ) {
                                            // DDA MP v4.0a-2: the avatar's own
                                            // heard-sound "?" - the host places
                                            // it (deterministically) and
                                            // streams the overlay id; drawn
                                            // exactly like the game's own
                                            // draw_footsteps_frame, fixed light
                                            // so it stays readable in the dark.
                                            const std::string mid = t.marker == 2 ? "footstep_above" :
                                                                    t.marker == 3 ? "footstep_below" :
                                                                    "footstep";
                                            trace_draw( 6, mid.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 6,
                                                    mid.c_str(), "",
                                                    x - snap.width / 2, y - snap.height / 2,
                                                    static_cast<int>( lit_level::LIT ), 0, 0,
                                                    true );
                                        }
                                    }
                                }
                                // DDA MP v4.0a-42: host target_ui draws its
                                // feedback through the tileset, not ImGui:
                                // "line_trail" sprites along the trajectory and
                                // the "cursor" sprite on the target tile.
                                if( mp_throw_active ||
                                    mp_cursor != mp_cursor_mode::none ) {
                                    const int rdx = mp_throw_active ? mp_throw_dx :
                                                    mp_cursor_dx;
                                    const int rdy = mp_throw_active ? mp_throw_dy :
                                                    mp_cursor_dy;
                                    const int acx = snap.width / 2;
                                    const int acy = snap.height / 2;
                                    int tx = acx + rdx;
                                    int ty = acy + rdy;
                                    if( tx >= 0 && ty >= 0 && tx < snap.width &&
                                        ty < snap.height ) {
                                        int x0 = acx;
                                        int y0 = acy;
                                        const int dxs = std::abs( tx - x0 );
                                        const int dys = std::abs( ty - y0 );
                                        const int sx = x0 < tx ? 1 : -1;
                                        const int sy = y0 < ty ? 1 : -1;
                                        int err = dxs - dys;
                                        while( x0 != tx || y0 != ty ) {
                                            const int e2 = 2 * err;
                                            if( e2 > -dys ) {
                                                err -= dys;
                                                x0 += sx;
                                            }
                                            if( e2 < dxs ) {
                                                err += dxs;
                                                y0 += sy;
                                            }
                                            if( x0 == tx && y0 == ty ) {
                                                break;
                                            }
                                            netplay::tiles_backend_draw(
                                                tiles_backend, 6, "line_trail", "",
                                                x0 - snap.width / 2, y0 - snap.height / 2,
                                                static_cast<int>( lit_level::LIT ), 0, 0,
                                                true );
                                        }
                                        netplay::tiles_backend_draw(
                                            tiles_backend, 6, "cursor", "",
                                            tx - snap.width / 2, ty - snap.height / 2,
                                            static_cast<int>( lit_level::LIT ), 0, 0, true );
                                    }
                                }
                                if( mp_pick_tiles_active ) {
                                    // The host's own choose_adjacent_highlight
                                    // marks every candidate with the tileset's
                                    // "highlight" overlay (cata_tiles::
                                    // draw_highlight uses the same id). Draw it
                                    // on all of them, not just one cursor.
                                    for( const point &c : mp_pick_tiles ) {
                                        netplay::tiles_backend_draw(
                                            tiles_backend, 6, "highlight", "",
                                            c.x, c.y,
                                            static_cast<int>( lit_level::LIT ), 0, 0, true );
                                    }
                                }
                                netplay::tiles_backend_end( tiles_backend );
                                // The sidebar/message adaptors were already
                                // redrawn earlier in this same iteration, before
                                // the canvas was stamped. Re-running a full
                                // ui_manager::redraw() here (the old signature
                                // check did that on every sidebar change)
                                // repainted the window background over the just
                                // drawn tiles for one frame - the flicker seen
                                // while input arrives in bursts. Keep the frame
                                // single-pass: adaptors first, canvas last.
                                mp_frame_trace( "end ok" );
                            }
                        }
                    } catch( const std::exception &err ) {
                        mp_tlog( std::string( "tiles: EXCEPTION " ) + err.what() );
                        tiles_destroy();
                    } catch( ... ) {
                        mp_tlog( "tiles: EXCEPTION unknown" );
                        tiles_destroy();
                    }
                }
#endif
            } else {
                // DDA MP v3.9: no valid picture yet - hold a quiet
                // placeholder over the menu instead of flashing it. redraw()
                // above already regenerated the registered windows; the
                // stdscr paint wraps them into the neutral wait screen. Safe
                // here: this iteration paints no tileset canvas.
                werase( catacurses::stdscr );
                mvwprintz( catacurses::stdscr, point( 1, 1 ), c_light_gray,
                           _( "Waiting for the host picture..." ) );
                wnoutrefresh( catacurses::stdscr );
            }

            // DDA MP v4.0a-7: overlay the turn-pacing hint on top of the
            // freshly stamped canvas (the canvas is stamped earlier in this
            // iteration, so the text survives until the present below).
            // DDA MP v4.0a-86: show the long wait state in the same line.
            if( !mp_hint_text.empty() || netplay::long_wait_active() ) {
                const int mp_hint_w = std::min( TERMX, 48 );
                if( !mp_hint_win ) {
                    mp_hint_win = catacurses::newwin( 1, mp_hint_w,
                                                      point( std::max( 0, ( TERMX - mp_hint_w ) / 2 ), 0 ) );
                }
                werase( mp_hint_win );
                std::string s = mp_hint_text;
                if( netplay::long_wait_active() ) {
                    s = _( "Long wait mode is on" );
                    if( !mp_hint_text.empty() ) {
                        s += "  " + mp_hint_text;
                    }
                }
                if( static_cast<int>( s.size() ) > mp_hint_w ) {
                    s.resize( mp_hint_w );
                }
                mvwprintz( mp_hint_win, point( 0, 0 ),
                           netplay::long_wait_active() ? c_light_green : c_yellow, s );
                wnoutrefresh( mp_hint_win );
            }

            // DDA MP v4.0a-147: adopt the host's panel layout before the
            // native sidebar is built, so its height and the log start row
            // match the host instead of the client's local panel options.
            {
                static std::string mp_applied_panel_layout;
                std::string mp_host_layout;
                if( netplay::client_panel_layout( mp_host_layout ) &&
                    mp_host_layout != mp_applied_panel_layout ) {
                    mp_inv_log( string_format( "panels: got len=%d",
                                               static_cast<int>( mp_host_layout.size() ) ) );
                    try {
                        JsonValue jv = json_loader::from_string( mp_host_layout );
                        panel_manager::get_manager().deserialize( jv.get_array() );
                        mp_applied_panel_layout = mp_host_layout;
                        mp_inv_log( "panels: host layout applied" );
                    } catch( ... ) {
                        // Malformed layout: keep the client's own.
                    }
                }
            }
            // DDA MP v4.0a-115: draw the mirrored sidebar with the game's own
            // panel renderer, after the map so it always sits on top.
            if( mp_native_render_enabled() ) {
                mp_draw_native_sidebar();
            }
            // ImGui pass on top of the map and the native panels: message log,
            // minimap and the character-sheet fallback.
            netplay::overlay_begin();
            // While looking/aiming the whole sidebar belongs to the look
            // window (the host does the same); skip the client log/minimap so
            // their ImGui layer cannot paint over the native look panel.
            if( mp_cursor == mp_cursor_mode::none && !mp_throw_active ) {
                mp_draw_sidebar_overlay();
            }
            mp_draw_cursor_info( snap );
            if( mp_throw_active ) {
                mp_draw_aim_panel();
            }
            // DDA MP v4.0a-159: on the native renderer use the tileset's own
            // "cursor" sprite through game::draw_cursor, exactly like the host.
            // The hand-drawn brackets remain only for the replay fallback.
            if( mp_native_render_enabled() ) {
                mp_queue_native_cursor( snap );
            } else {
                bool have_target = false;
                int rdx = 0;
                int rdy = 0;
                if( mp_throw_active ) {
                    have_target = true;
                    rdx = mp_throw_dx;
                    rdy = mp_throw_dy;
                } else if( mp_cursor != mp_cursor_mode::none ) {
                    have_target = true;
                    rdx = mp_cursor_dx;
                    rdy = mp_cursor_dy;
                }
                const int tx = snap.width / 2 + rdx;
                const int ty = snap.height / 2 + rdy;
                if( have_target && tx >= 0 && ty >= 0 && tx < snap.width &&
                    ty < snap.height && mp_canvas_tw > 0.0f && mp_canvas_th > 0.0f ) {
                    // The renderer places a cell's top-left at
                    // canvas_center + (cell - size/2) * tile; the cursor sprite
                    // covers that whole cell. Subtracting another half tile put
                    // the brackets on the intersection of four cells.
                    const float px = mp_canvas_cx +
                                     ( tx - snap.width / 2.0f ) * mp_canvas_tw;
                    const float py = mp_canvas_cy +
                                     ( ty - snap.height / 2.0f ) * mp_canvas_th;
                    const float w = mp_canvas_tw;
                    const float h = mp_canvas_th;
                    const float arm = std::max( 5.0f, std::min( w, h ) * 0.25f );
                    const float lw = std::max( 3.0f, std::min( w, h ) * 0.09f );
                    ImDrawList *dl = ImGui::GetForegroundDrawList();
                    const ImU32 col = IM_COL32( 255, 255, 255, 235 );
                    // top-left
                    dl->AddLine( ImVec2( px, py ), ImVec2( px + arm, py ), col, lw );
                    dl->AddLine( ImVec2( px, py ), ImVec2( px, py + arm ), col, lw );
                    // top-right
                    dl->AddLine( ImVec2( px + w - arm, py ), ImVec2( px + w, py ), col, lw );
                    dl->AddLine( ImVec2( px + w, py ), ImVec2( px + w, py + arm ), col, lw );
                    // bottom-left
                    dl->AddLine( ImVec2( px, py + h - arm ), ImVec2( px, py + h ), col, lw );
                    dl->AddLine( ImVec2( px, py + h ), ImVec2( px + arm, py + h ), col, lw );
                    // bottom-right
                    dl->AddLine( ImVec2( px + w - arm, py + h ), ImVec2( px + w, py + h ), col, lw );
                    dl->AddLine( ImVec2( px + w, py + h - arm ), ImVec2( px + w, py + h ), col, lw );
                }
            }
            // DDA MP v4.0a-154: the host's line_trail is a dot on every tile
            // between the character and the target. Draw the same dots for
            // fire/throw, so the trajectory is readable even if the tileset
            // line sprite does not resolve in the replay backend.
            if( ( mp_throw_active || mp_cursor == mp_cursor_mode::fire ) &&
                mp_canvas_tw > 0.0f && mp_canvas_th > 0.0f ) {
                const int rdx = mp_throw_active ? mp_throw_dx : mp_cursor_dx;
                const int rdy = mp_throw_active ? mp_throw_dy : mp_cursor_dy;
                const int cx = snap.width / 2;
                const int cy = snap.height / 2;
                const int tx = cx + rdx;
                const int ty = cy + rdy;
                if( tx >= 0 && ty >= 0 && tx < snap.width && ty < snap.height ) {
                    ImDrawList *dl = ImGui::GetForegroundDrawList();
                    const ImU32 dot = IM_COL32( 255, 255, 255, 200 );
                    const float rad = std::max( 2.0f, std::min( mp_canvas_tw,
                                                                mp_canvas_th ) * 0.08f );
                    int x = cx;
                    int y = cy;
                    const int dx = std::abs( tx - cx );
                    const int sx = cx < tx ? 1 : -1;
                    const int dy = -std::abs( ty - cy );
                    const int sy = cy < ty ? 1 : -1;
                    int err = dx + dy;
                    while( true ) {
                        if( !( x == cx && y == cy ) && !( x == tx && y == ty ) ) {
                            const float pcx = mp_canvas_cx +
                                              ( x - snap.width / 2.0f ) * mp_canvas_tw;
                            const float pcy = mp_canvas_cy +
                                              ( y - snap.height / 2.0f ) * mp_canvas_th;
                            dl->AddCircleFilled( ImVec2( pcx, pcy ), rad, dot );
                        }
                        if( x == tx && y == ty ) {
                            break;
                        }
                        const int e2 = 2 * err;
                        if( e2 >= dy ) {
                            err += dy;
                            x += sx;
                        }
                        if( e2 <= dx ) {
                            err += dx;
                            y += sy;
                        }
                    }
                }
            }
            if( mp_map_open ) {
                ImDrawList *mdl = ImGui::GetForegroundDrawList();
                const ImGuiIO &mio = ImGui::GetIO();
                const float mfs = ImGui::GetFontSize();
                mdl->AddRectFilled( ImVec2( 0, 0 ), mio.DisplaySize, IM_COL32( 0, 0, 0, 240 ) );
                mdl->AddText( ImVec2( mfs, mfs ), IM_COL32( 235, 235, 235, 255 ),
                              fork_text( "Map" ).c_str() );
                if( mp_mini_map.width > 0 && mp_mini_map.height > 0 &&
                    mp_mini_map.color.size() == static_cast<size_t>( mp_mini_map.width ) *
                    mp_mini_map.height ) {
                    const float avail_w = mio.DisplaySize.x - mfs * 4.0f;
                    const float avail_h = mio.DisplaySize.y - mfs * 6.0f;
                    const float cell = std::max( 2.0f, std::min( avail_w / mp_mini_map.width,
                                                                 avail_h / mp_mini_map.height ) );
                    const float mw = mp_mini_map.width * cell;
                    const float mh = mp_mini_map.height * cell;
                    const float mx = ( mio.DisplaySize.x - mw ) / 2.0f;
                    const float my = ( mio.DisplaySize.y - mh ) / 2.0f;
                    mdl->AddRectFilled( ImVec2( mx - 2, my - 2 ),
                                        ImVec2( mx + mw + 2, my + mh + 2 ),
                                        IM_COL32( 24, 28, 28, 255 ) );
                    for( int yy = 0; yy < mp_mini_map.height; ++yy ) {
                        for( int xx = 0; xx < mp_mini_map.width; ++xx ) {
                            const size_t mi = static_cast<size_t>( yy ) * mp_mini_map.width + xx;
                            if( mi >= mp_mini_map.color.size() ||
                                mi >= mp_mini_map.seen.size() || mp_mini_map.seen[mi] == 0 ) {
                                continue;
                            }
                            const int ci = static_cast<int>( mp_mini_map.color[mi] );
                            const ImU32 col = cataimgui::ImU32_from_color(
                                                  decode_cell_color( ci ) );
                            mdl->AddRectFilled(
                                ImVec2( mx + xx * cell, my + yy * cell ),
                                ImVec2( mx + ( xx + 1 ) * cell, my + ( yy + 1 ) * cell ), col );
                        }
                    }
                    // Avatar at the block centre, then critter beacons.
                    const float cx = mx + mw / 2.0f;
                    const float cy = my + mh / 2.0f;
                    mdl->AddCircle( ImVec2( cx, cy ), std::max( 4.0f, cell * 1.5f ),
                                    IM_COL32( 255, 255, 255, 255 ), 0, 2.0f );
                    for( const minimap_frame::beacon &b : mp_mini_map.beacons ) {
                        const float bx = mx + ( mp_mini_map.width / 2 + b.dx + 0.5f ) * cell;
                        const float by = my + ( mp_mini_map.height / 2 + b.dy + 0.5f ) * cell;
                        const ImU32 bcol = cataimgui::ImU32_from_color(
                                               decode_cell_color( static_cast<int>( b.color ) ) );
                        mdl->AddCircleFilled( ImVec2( bx, by ), std::max( 2.0f, cell * 0.7f ),
                                              bcol );
                    }
                } else {
                    mdl->AddText( ImVec2( mfs, mfs * 3.0f ), IM_COL32( 180, 180, 180, 255 ),
                                  fork_text( "No map data yet." ).c_str() );
                }
                mdl->AddText( ImVec2( mfs, mio.DisplaySize.y - mfs * 2.0f ),
                              IM_COL32( 180, 180, 180, 255 ), fork_text( "[Esc] close" ).c_str() );
            }
            netplay::overlay_end();

            // DDA MP v4.0a-15: context windows above the canvas (overlay pass).
            mp_overlay_inventory( mp_inv_text, mp_inv_open, mp_inv_sel, mp_inv_scroll );

            // Present the freshly drawn frame. The game's own main loop calls
            // refresh_display() every iteration; here input pumping alone is
            // not a reliable present trigger, so the first frame would sit
            // in the buffer until the first keypress without this.
            refresh_display();

            // DDA MP v3.9 probe: first iterations with ticks/snap facts, so a
            // "menu until first input" phase can be pinned to a present-gate
            // rejection (see the "present #N [client]" lines around it).
            {
                static int mp_view_probe = 0;
                if( mp_view_probe < 200 ) {
                    ++mp_view_probe;
                    mp_tlog( string_format(
                                 "view it=%d t=%u recv=%zu canvas=%d ph=%d",
                                 mp_view_probe, GetTicks(),
                                 snap.cells.size(), mp_canvas_ok ? 1 : 0,
                                 mp_have_picture ? 0 : 1 ) );
                }
            }

            // DDA MP test hook: open the look cursor after the first picture so
            // an automated run can verify the reticle/info panel rendering.
            if( const char *test_look = std::getenv( "DDA_MP_TEST_CLIENT_LOOK" );
                test_look != nullptr && test_look[0] != '\0' ) {
                static bool mp_test_look_once = false;
                if( !mp_test_look_once && mp_have_picture ) {
                    mp_test_look_once = true;
                    mp_cursor = mp_cursor_mode::look;
                    mp_cursor_dx = 8;
                    mp_cursor_dy = 0;
                    mp_inv_log( "test: look cursor" );
                    mp_tlog( string_format(
                                 "look geom cx=%.1f cy=%.1f tw=%.1f th=%.1f term=%dx%d font=%dx%d",
                                 mp_canvas_cx, mp_canvas_cy, mp_canvas_tw, mp_canvas_th,
                                 TERMX, TERMY, font_px_w, font_px_h ) );
                }
                // Prefer an NPC/monster tile so the creature section of the
                // look panel (character block, worn gear, attitude background)
                // is exercised too. Keep scanning: the first picture often
                // arrives before any creature is streamed.
                static bool mp_test_look_on_creature = false;
                if( mp_cursor == mp_cursor_mode::look && !mp_test_look_on_creature ) {
                    for( const snapshot::creature_frame &cf : snap.creatures ) {
                        if( cf.az != snap.origin_z ) {
                            continue;
                        }
                        const int ddx = cf.ax - snap.origin_x;
                        const int ddy = cf.ay - snap.origin_y;
                        if( std::abs( ddx ) < snap.width / 2 &&
                            std::abs( ddy ) < snap.height / 2 ) {
                            mp_cursor_dx = ddx;
                            mp_cursor_dy = ddy;
                            mp_test_look_on_creature = true;
                            mp_inv_log( string_format( "test: look creature offset %d,%d",
                                                       ddx, ddy ) );
                            break;
                        }
                    }
                }
            }
            // DDA MP test hook: report NPC frames once the first snapshots are
            // in, so an automated run can tell whether the test world has one.
            if( const char *test_npcs = std::getenv( "DDA_MP_TEST_LOG_NPCS" );
                test_npcs != nullptr && test_npcs[0] != '\0' ) {
                static bool mp_test_npcs_done = false;
                static int mp_test_npc_frames = 0;
                if( !mp_test_npcs_done && ++mp_test_npc_frames >= 10 ) {
                    mp_test_npcs_done = true;
                    const tripoint_abs_ms avpos = get_avatar().pos_abs();
                    int found = 0;
                    for( const snapshot::creature_frame &cf : snap.creatures ) {
                        if( cf.kind != 1 ) {
                            continue;
                        }
                        ++found;
                        mp_inv_log( string_format(
                                        "test: npc dx=%d dy=%d json=%.160s",
                                        cf.ax - avpos.x(), cf.ay - avpos.y(), cf.json.c_str() ) );
                    }
                    if( found == 0 ) {
                        mp_inv_log( "test: npc none" );
                    }
                }
            }
            // DDA MP test hook: put the client into long wait/AFK so the host
            // can advance rounds without it.
            if( const char *test_afk = std::getenv( "DDA_MP_TEST_CLIENT_AFK" );
                test_afk != nullptr && test_afk[0] != '\0' ) {
                static bool mp_test_afk_once = false;
                if( !mp_test_afk_once ) {
                    mp_test_afk_once = true;
                    netplay::client_send_afk( true );
                    mp_inv_log( "test: client afk" );
                }
            }
            // DDA MP test hook: send one engine action from the client after
            // the first picture, so automated runs can exercise the host's
            // per-action message routing (smash emits action messages).
            if( const char *test_action = std::getenv( "DDA_MP_TEST_CLIENT_ACTION" );
                test_action != nullptr && test_action[0] != '\0' ) {
                static bool mp_test_action_once = false;
                const std::string test_action_name( test_action );
                const bool test_needs_wield =
                    test_action_name == "use_wielded" || test_action_name == "mend";
                bool test_action_ready = true;
                if( test_action_name == "autoattack" ) {
                    // Wait until the host's spawned test monster is streamed,
                    // otherwise the action can run before the spawn.
                    test_action_ready = false;
                    const tripoint_abs_ms avpos = get_avatar().pos_abs();
                    for( const snapshot::creature_frame &cf : snap.creatures ) {
                        if( cf.kind == 0 && cf.ax - avpos.x() == 1 &&
                            cf.ay - avpos.y() == 0 ) {
                            test_action_ready = true;
                            break;
                        }
                    }
                }
                if( !mp_test_action_once && mp_native_avatar_ready && test_action_ready &&
                    ( !test_needs_wield || get_avatar().get_wielded_item() ) ) {
                    const std::string action_name( test_action );
                    bool sent = true;
                    const auto mp_test_find_npc = [&]( int &dx, int &dy ) -> bool {
                        const tripoint_abs_ms avpos = get_avatar().pos_abs();
                        for( const snapshot::creature_frame &cf : snap.creatures ) {
                            if( cf.kind == 1 ) {
                                dx = cf.ax - avpos.x();
                                dy = cf.ay - avpos.y();
                                return true;
                            }
                        }
                        return false;
                    };
                    if( action_name == "smash" ) {
                        netplay::client_send_client_action( ACTION_SMASH );
                    } else if( action_name == "cycle_move" ) {
                        netplay::client_send_client_action( ACTION_CYCLE_MOVE );
                    } else if( action_name == "bodygraph" ) {
                        mp_client_dispatch_action( ACTION_BODYSTATUS, "", "" );
                    } else if( action_name == "medical" ) {
                        mp_client_dispatch_action( ACTION_MEDICAL, "", "" );
                    } else if( action_name == "factions" ) {
                        mp_client_dispatch_action( ACTION_FACTIONS, "", "" );
                    } else if( action_name == "sky" ) {
                        mp_client_dispatch_action( ACTION_SKY, "", "" );
                    } else if( action_name == "map" ) {
                        mp_client_dispatch_action( ACTION_MAP, "", "" );
                    } else if( action_name == "style" ) {
                        mp_client_dispatch_action( ACTION_PICK_STYLE, "", "" );
                    } else if( action_name == "npc" ) {
                        mp_run_npc_interact( "Test NPC", 1, 0 );
                    } else if( action_name == "dialogue" ) {
                        mp_run_dialogue(
                            "Test NPC: Hello." "\x1f" "0" "\x1e" "Option A" "\x1f"
                            "1" "\x1e" "Option B" "\x1f" "-1" "\x1e" "Close" );
                    } else if( action_name == "npc_talk" ) {
                        netplay::client_send_intent( "talk_npc", 0, { 0, -1 } );
                    } else if( action_name == "npc_real_talk" ||
                               action_name == "npc_real_missions" ||
                               action_name == "npc_real_trade" ||
                               action_name == "npc_real_sell" ) {
                        int dx = 0;
                        int dy = 0;
                        if( mp_test_find_npc( dx, dy ) ) {
                            if( action_name == "npc_real_talk" ) {
                                netplay::client_send_intent( "talk_npc", 0, { dx, dy } );
                            } else if( action_name == "npc_real_missions" ) {
                                mp_mission_dx = dx;
                                mp_mission_dy = dy;
                                // Timestamp for the optional auto turn-in step.
                                mp_test_mission_accepted = std::chrono::steady_clock::now();
                                netplay::client_send_intent( "mission_offers", 0, { dx, dy } );
                            } else if( action_name == "npc_real_sell" ) {
                                mp_trade_dx = dx;
                                mp_trade_dy = dy;
                                netplay::client_send_intent( "trade_list", 0, { dx, dy, 1 } );
                            } else {
                                mp_trade_dx = dx;
                                mp_trade_dy = dy;
                                netplay::client_send_intent( "trade_list", 0, { dx, dy, 0 } );
                            }
                            mp_inv_log( string_format( "test: npc target %d,%d", dx, dy ) );
                        } else {
                            sent = false;
                        }
                    } else if( action_name == "npc_real_turnin" ) {
                        int dx = 0;
                        int dy = 0;
                        if( mp_test_find_npc( dx, dy ) ) {
                            mp_mission_dx = dx;
                            mp_mission_dy = dy;
                            netplay::client_send_intent( "mission_turnin_list", 0,
                                                         { dx, dy } );
                            mp_inv_log( string_format( "test: turnin list %d,%d", dx, dy ) );
                        } else {
                            sent = false;
                        }
                    } else if( action_name == "missions" ) {
                        mp_run_mission_journal(
                            "Test mission" "\x1e" "In progress" "\x1e" "Do the thing" "\x1f"
                            "Second mission" "\x1e" "Completed" "\x1e" "Already done" );
                    } else if( action_name == "diary" ) {
                        mp_client_dispatch_action( ACTION_DIARY, "", "" );
                    } else if( action_name == "spells" ) {
                        mp_client_dispatch_action( ACTION_CAST_SPELL, "", "" );
                    } else if( action_name == "spell_cast" ) {
                        netplay::client_send_intent( "test_spell_cast", 0, {} );
                    } else if( action_name == "use_wielded" ) {
                        mp_client_dispatch_action( ACTION_USE_WIELDED, "", "" );
                    } else if( action_name == "mend" ) {
                        mp_client_dispatch_action( ACTION_MEND, "", "" );
                    } else if( action_name == "autoattack" ) {
                        netplay::client_send_intent( "autoattack", 0, {} );
                    } else if( action_name == "wield" ) {
                        // Carried items live inside container pockets, so walk
                        // the top-level location tree instead of inv_dump().
                        sent = false;
                        avatar &you = get_avatar();
                        const item_location mirror_wielded =
                            you.get_wielded_item();
                        const std::string wielded_uid =
                            mirror_wielded ?
                            mirror_wielded->get_var( "mp_uid", std::string() ) :
                            std::string();
                        for( item_location &top : you.top_items_loc() ) {
                            if( !top ) {
                                continue;
                            }
                            top.visit_items( [&]( const item_location & il ) {
                                if( !il || il->typeId() != itype_id( "knife_combat" ) ) {
                                    return VisitResponse::NEXT;
                                }
                                const std::string uid =
                                    il->get_var( "mp_uid", std::string() );
                                if( uid.empty() || uid == wielded_uid ) {
                                    return VisitResponse::NEXT;
                                }
                                netplay::client_send_inventory_uid( uid, 'w' );
                                sent = true;
                                return VisitResponse::ABORT;
                            } );
                            if( sent ) {
                                break;
                            }
                        }
                        if( sent ) {
                            mp_inv_log( "action: wield from mirror" );
                        }
                    } else if( action_name == "trade" ) {
                        mp_run_trade(
                            "0" "\x1f" "0" "\x1f" "0" "\x1f" "0" "\x1f"
                            "0" "\x1e" "Test item" "\x1e" "100"
                            "\x1e" "1" "\x1e" "0" "\x1f"
                            "1" "\x1e" "Second item" "\x1e" "250" "\x1e" "1" "\x1e" "0" );
                    } else if( action_name == "morale" ) {
                        mp_client_dispatch_action( ACTION_MORALE, "", "" );
                    } else if( action_name == "haul" ) {
                        netplay::client_send_intent( "haul_toggle", 0, {} );
                    } else if( action_name == "vehicle_info" ) {
                        netplay::client_send_intent( "vehicle_info", 0, {} );
                    } else if( action_name == "vehicle_refuel" ) {
                        netplay::client_send_intent( "test_vehicle_refuel", 0, {} );
                    } else if( action_name == "vehicle_parts" ) {
                        netplay::client_send_intent( "test_vehicle_parts", 0, {} );
                    } else if( action_name == "vehicle_repair" ) {
                        netplay::client_send_intent( "test_vehicle_repair", 0, {} );
                    } else if( action_name == "grab" ) {
                        netplay::client_send_intent( "test_grab_start", 0, {} );
                    } else if( action_name == "grab_drag" ) {
                        netplay::client_send_intent( "test_grab_drag", 0, {} );
                    } else if( action_name == "movement" ) {
                        netplay::client_send_intent( "movement_mode", 0, {}, "run" );
                    } else if( action_name == "sort_armor" ) {
                        std::vector<item *> items;
                        get_avatar().worn.inv_dump( items );
                        std::string data;
                        for( auto rit = items.rbegin(); rit != items.rend(); ++rit ) {
                            if( *rit == nullptr ) {
                                continue;
                            }
                            const std::string uid =
                                ( *rit )->get_var( "mp_uid", std::string() );
                            if( uid.empty() ) {
                                continue;
                            }
                            if( !data.empty() ) {
                                data += ',';
                            }
                            data += uid;
                        }
                        if( data.empty() ) {
                            sent = false;
                        } else {
                            netplay::client_send_intent( "sort_armor", 0, {}, data );
                        }
                    } else if( action_name == "dir_drop" ) {
                        sent = false;
                        for( item *it : get_avatar().inv_dump() ) {
                            if( it == nullptr ) {
                                continue;
                            }
                            const std::string uid = it->get_var( "mp_uid", std::string() );
                            if( !uid.empty() ) {
                                netplay::client_send_inventory_uid( uid, 'F', {}, { 1, 0 } );
                                sent = true;
                                break;
                            }
                        }
                    } else if( action_name == "zones" ) {
                        netplay::client_send_intent( "zones", 0, {} );
                    } else if( action_name == "suicide" ) {
                        netplay::client_send_intent( "suicide", 0, {} );
                    } else if( action_name == "autofeatures" ) {
                        netplay::client_send_client_action( ACTION_TOGGLE_AUTO_FEATURES );
                    } else if( action_name == "safemode" ) {
                        netplay::client_send_client_action( ACTION_TOGGLE_SAFEMODE );
                    } else if( action_name == "workout" ) {
                        netplay::client_send_intent( "workout", 0, { 1, 5 } );
                    } else if( action_name == "thief" ) {
                        netplay::client_send_client_action( ACTION_TOGGLE_THIEF_MODE );
                    } else if( action_name == "bionics" ) {
                        mp_client_dispatch_action( ACTION_BIONICS, "", "" );
                    } else if( action_name == "mutations" ) {
                        mp_client_dispatch_action( ACTION_MUTATIONS, "", "" );
                    } else if( action_name == "butcher" ) {
                        netplay::client_send_intent( "butcher", 0, { 0, 0 } );
                    } else if( action_name == "construct" ) {
                        netplay::client_send_intent( "construct", 0, { 1, 0 },
                                                     "board_up_window" );
                    } else if( action_name == "help" ) {
                        netplay::client_send_intent( "help", 0, {} );
                    } else if( action_name == "disassemble" ) {
                        // Address the first carried item by its host uid, exactly
                        // like the inventory item menu does. The mirror arrives
                        // with a status push, so retry until an item is there.
                        sent = false;
                        for( item *it : get_avatar().inv_dump() ) {
                            if( it == nullptr ) {
                                continue;
                            }
                            const std::string uid = it->get_var( "mp_uid", std::string() );
                            if( !uid.empty() ) {
                                netplay::client_send_inventory_uid( uid, 'D' );
                                sent = true;
                                break;
                            }
                        }
                    }
                    if( sent ) {
                        mp_test_action_once = true;
                        mp_inv_log( "test: client action " + action_name );
                    }
                }
            }
            // DDA MP test hook: draw the throw reticle/info without a real
            // throw, so an automated run can verify the trajectory dots.
            if( const char *test_throw = std::getenv( "DDA_MP_TEST_CLIENT_THROW" );
                test_throw != nullptr && test_throw[0] != '\0' ) {
                static bool mp_test_throw_once = false;
                if( !mp_test_throw_once && mp_have_picture ) {
                    mp_test_throw_once = true;
                    mp_throw_active = true;
                    mp_throw_dx = 5;
                    mp_throw_dy = 0;
                    mp_inv_log( "test: throw cursor" );
                }
            }
            if( mp_map_open ) {
                // Full-screen map: any non-timeout key closes it.
                const std::string map_action = ctxt.handle_input();
                if( map_action != "TIMEOUT" ) {
                    mp_map_open = false;
                }
                continue;
            }
            const std::string action = ctxt.handle_input();
            // DDA MP v4.0a-97: choose the tile to pick from (several tiles
            // with items): arrows cycle the candidates, ENTER confirms.
            if( mp_pick_tiles_active ) {
                const input_event mp_ev = ctxt.get_raw_input();
                const int mp_key = mp_ev.get_first_input();
                if( mp_key == 27 || action == "QUIT" ) {
                    mp_pick_tiles_active = false;
                    mp_tlog( "picktile: cancelled" );
                    continue;
                }
                // Same as the host's choose_adjacent_highlight: every candidate
                // tile is highlighted and the movement key towards one of them
                // picks that tile.
                point dir( 0, 0 );
                bool have_dir = false;
                if( action == "UP" ) {
                    dir = point( 0, -1 );
                    have_dir = true;
                } else if( action == "DOWN" ) {
                    dir = point( 0, 1 );
                    have_dir = true;
                } else if( action == "LEFT" ) {
                    dir = point( -1, 0 );
                    have_dir = true;
                } else if( action == "RIGHT" ) {
                    dir = point( 1, 0 );
                    have_dir = true;
                } else if( action == "LEFTUP" ) {
                    dir = point( -1, -1 );
                    have_dir = true;
                } else if( action == "RIGHTUP" ) {
                    dir = point( 1, -1 );
                    have_dir = true;
                } else if( action == "LEFTDOWN" ) {
                    dir = point( -1, 1 );
                    have_dir = true;
                } else if( action == "RIGHTDOWN" ) {
                    dir = point( 1, 1 );
                    have_dir = true;
                }
                if( have_dir ) {
                    for( const point &c : mp_pick_tiles ) {
                        if( c == dir ) {
                            netplay::client_send_pickup_tile( mp_pick_gen, c.x, c.y );
                            mp_pick_tiles_active = false;
                            mp_tlog( string_format( "picktile: chose %d,%d", c.x, c.y ) );
                            break;
                        }
                    }
                }
                continue;
            }
            // DDA MP v4.0a-150: look/fire cursor. Arrow keys move the reticle,
            // Enter confirms (fire sends the offset to the host), Esc cancels.
            if( mp_cursor != mp_cursor_mode::none ) {
                const input_event mp_ev = ctxt.get_raw_input();
                const int mp_key = mp_ev.get_first_input();
                if( mp_key == 27 || action == "QUIT" ) {
                    mp_cursor = mp_cursor_mode::none;
                    continue;
                }
                // The host's LOOK actions are not registered in the view
                // context; map the raw key through the same context the host
                // uses so the bottom hints are real controls.
                input_context lctxt( "LOOK", keyboard_mode::keycode );
                lctxt.register_action( "EXTENDED_DESCRIPTION" );
                lctxt.register_action( "TOGGLE_FAST_SCROLL" );
                const std::string look_act = lctxt.input_to_action( mp_ev );
                if( look_act == "TOGGLE_FAST_SCROLL" ) {
                    mp_look_fast_scroll = !mp_look_fast_scroll;
                    continue;
                }
                if( look_act == "EXTENDED_DESCRIPTION" ) {
                    tripoint_abs_ms look_ap;
                    tripoint_bub_ms look_lp;
                    if( mp_cursor_target( snap, look_ap, look_lp ) ) {
                        try {
                            extended_description_window ext_desc( look_lp );
                            ext_desc.show();
                        } catch( ... ) {
                        }
                    }
                    continue;
                }
                const int mp_step = mp_look_fast_scroll
                                    ? std::max( 1, get_option<int>( "FAST_SCROLL_OFFSET" ) ) : 1;
                if( action == "UP" ) {
                    mp_cursor_dy -= mp_step;
                    continue;
                }
                if( action == "DOWN" ) {
                    mp_cursor_dy += mp_step;
                    continue;
                }
                if( action == "LEFT" ) {
                    mp_cursor_dx -= mp_step;
                    continue;
                }
                if( action == "RIGHT" ) {
                    mp_cursor_dx += mp_step;
                    continue;
                }
                if( action == "CONFIRM" || mp_key == '\n' || mp_key == '\r' ) {
                    if( mp_cursor == mp_cursor_mode::fire ) {
                        netplay::client_send_intent(
                            "fire", 0, { mp_cursor_dx, mp_cursor_dy, 1 } );
                        mp_inv_log( string_format( "fire: target %d,%d",
                                                   mp_cursor_dx, mp_cursor_dy ) );
                    } else if( mp_cursor == mp_cursor_mode::spell ) {
                        mp_last_spell = mp_pending_spell;
                        mp_last_spell_dx = mp_cursor_dx;
                        mp_last_spell_dy = mp_cursor_dy;
                        netplay::client_send_intent(
                            "cast_spell", 0, { mp_cursor_dx, mp_cursor_dy },
                            mp_pending_spell );
                        mp_inv_log( string_format( "spell: cast %s at %d,%d",
                                                   mp_pending_spell.c_str(),
                                                   mp_cursor_dx, mp_cursor_dy ) );
                    }
                    mp_cursor = mp_cursor_mode::none;
                    continue;
                }
                continue;
            }
            // DDA MP v4.0a-35: throw targeting (arrows move the crosshair).
            if( mp_throw_active ) {
                const input_event mp_ev = ctxt.get_raw_input();
                const int mp_key = mp_ev.get_first_input();
                if( mp_key == 27 || action == "QUIT" ) {
                    mp_throw_active = false;
                    mp_aim_text.clear();
                    mp_have_aim_panel = false;
                    continue;
                }
                if( mp_key == '0' ) { // host TARGET: CENTER -> target self
                    mp_throw_dx = 0;
                    mp_throw_dy = 0;
                    mp_client_aim_recalc();                    continue;
                }
                if( action == "UP" ) {
                    --mp_throw_dy;                    mp_client_aim_recalc();
                    continue;
                }
                if( action == "DOWN" ) {
                    ++mp_throw_dy;                    mp_client_aim_recalc();
                    continue;
                }
                if( action == "LEFT" ) {
                    --mp_throw_dx;                    mp_client_aim_recalc();
                    continue;
                }
                if( action == "RIGHT" ) {
                    ++mp_throw_dx;                    mp_client_aim_recalc();
                    continue;
                }
                if( action == "CONFIRM" || mp_key == '\n' || mp_key == '\r' ) {
                    netplay::client_send_inventory_uid(
                        mp_throw_uid, 't', {}, { mp_throw_dx, mp_throw_dy } );
                    mp_inv_log( string_format( "action: uid key=t target=%d,%d",
                                               mp_throw_dx, mp_throw_dy ) );
                    mp_throw_active = false;
                    continue;
                }
                continue;
            }
            // DDA MP v4.0a-16: inventory input is handled locally; orders are
            // not sent to the host while the window is open.
            if( mp_inv_open ) {
                const input_event mp_ev = ctxt.get_raw_input();
                const int mp_key = mp_ev.get_first_input();
                if( mp_key == 27 || mp_key == 'i' || mp_key == 'I' ||
                    action == "QUIT" ) {
                    mp_inv_open = false;
                    continue;
                }
                if( action == "UP" || mp_key == 'w' || mp_key == 'k' || mp_key == '8' ) {
                    mp_inv_sel = std::max( 0, mp_inv_sel - 1 );
                    continue;
                }
                if( action == "DOWN" || mp_key == 's' || mp_key == 'j' || mp_key == '2' ) {
                    ++mp_inv_sel;
                    continue;
                }
                if( mp_key == KEY_PPAGE ) {
                    mp_inv_sel = std::max( 0, mp_inv_sel - 10 );
                    continue;
                }
                if( mp_key == KEY_NPAGE ) {
                    mp_inv_sel += 10;
                    continue;
                }
                if( action == "LEFT" || action == "RIGHT" || mp_key == '\t' ) {
                    const bool to_carried = action == "RIGHT" ||
                                            ( mp_key == '\t' && mp_inv_sel < mp_inv_split );
                    mp_inv_sel = to_carried ? mp_inv_split : 0;
                    continue;
                }
                continue;
            }
            {
                const input_event mp_ev = ctxt.get_raw_input();
                const int mp_ch = mp_ev.get_first_input();
                if( mp_ch == 'i' || mp_ch == 'I' ) {
                    // DDA MP v4.0a-17: run the game's real inventory UI on a
                    // temporary copy built from the host's data. The legacy
                    // ImGui panel stays available for comparison via env.
                    if( mp_legacy_inventory_enabled() ) {
                        mp_inv_open = true;
                        mp_inv_scroll = 0;
                        mp_inv_sel = 0;
                    } else {
                        mp_run_host_inventory( mp_inv_text );
                    }
                    continue;
                }
                if( mp_ch == '@' ) {
                    mp_run_character_info( mp_char_json );
                    continue;
                }
                if( mp_ch == 'T' ) {
                    // DDA MP v4.0a-137/181: text chat to every player.
                    mp_run_client_chat();
                    continue;
                }
            }
            // DDA MP v4.0a-14: '.' / '5' passes the turn (WAIT action).
            {
                const input_event mp_ev = ctxt.get_raw_input();
                const int mp_ch = mp_ev.get_first_input();
                if( mp_ch == '.' || mp_ch == '5' ) {
                    key_event ke;
                    ke.code = 20;
                    client_send_keys( { ke } );
                    continue;
                }
            }
            if( action == "QUIT" ) {
                // DDA MP v4.0a-72: the client's own in-game main menu. Every
                // entry the host has and a remote player can use is here;
                // local settings UIs run on the client, action orders travel to
                // the host, and Disconnect replaces quit.
                const int mp_sel = mp_client_main_menu();
                if( mp_client_run_menu_entry( mp_sel, mp_inv_text, mp_char_json ) ) {
                    voluntary_quit = true;
                    break;
                }
                continue;
            }
            const long code = key_to_remote( action );
            if( code != 0 ) {
                key_event ke;
                ke.code = static_cast<uint32_t>( code );
                client_send_keys( { ke } );
                continue;
            }
            // DDA MP v4.0a-78: keys the view context does not consume are looked
            // up in the default game context, so the client reacts to the same
            // keys as the host. Client-local UIs run here; every other action
            // travels to the host as an action order (the host answers with a
            // message when its own bridge for it is not there yet).
            if( mp_key_action_parity() && action != "TIMEOUT" ) {
                const input_event mp_ev = ctxt.get_raw_input();
                input_context dctxt = get_default_mode_input_context();
                const std::string act_name = dctxt.input_to_action( mp_ev );
                if( !act_name.empty() ) {
                    const action_id mp_act = look_up_action( act_name );
                    switch( mp_act ) {
                        case ACTION_NULL:
                            continue;
                        case ACTION_MAIN_MENU: {
                            const int mp_sel = mp_client_main_menu();
                            if( mp_client_run_menu_entry( mp_sel, mp_inv_text,
                                                          mp_char_json ) ) {
                                voluntary_quit = true;
                            }
                            continue;
                        }
                        case ACTION_OPTIONS: {
                            ui_adaptor mp_ui_blocker( ui_adaptor::disable_uis_below{} );
                            get_options().show( true );
                            continue;
                        }
                        default:
                            // DDA MP v4.0a-181: one dispatcher for direct keys
                            // and the action menu, so both see the same local
                            // UIs and host orders.
                            mp_client_dispatch_action( static_cast<int>( mp_act ),
                                                       mp_inv_text, mp_char_json );
                            continue;
                    }
                }
            }
        }

        if( voluntary_quit ) {
            break;
        }
        const std::string addr = client_last_address();
        if( addr.empty() || !ask_reconnect( addr ) ) {
            break;
        }
        if( !client_connect( addr ) ) {
            popup( netplay::fork_text( "Could not reconnect: the host is unreachable." ) );
            break;
        }
        fat_client_load_data();
        if( !client_is_connected() ) {
            const std::string reason = client_deny_reason();
            if( reason.empty() ) {
                popup( netplay::fork_text( "Connection refused: host unreachable." ) );
            } else {
                popup( string_format( netplay::fork_text( "Connection refused: %s" ), reason ) );
            }
            break;
        }
    }
    if( voluntary_quit ) {
        // Leave the session gracefully; the caller is otherwise responsible
        // for cleanup.
        netplay::client_disconnect();
    }
#if defined(TILES)
    tiles_destroy();
    // Wipe replay remnants so the menu / main UI starts from a clean buffer.
    netplay::tiles_backend_clear_screen( nullptr );
#endif
}

} // namespace netplay

