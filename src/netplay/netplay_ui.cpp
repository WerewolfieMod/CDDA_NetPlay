// CataDDA Multiplayer ("DDA MP") — user-facing flows:
//  * in-game "Open world to network" host dialog,
//  * in-game/main-menu "Join by IP…" client flow,
//  * the remote-session fullscreen ASCII viewer (`run_view_loop`).

#include "netplay.h"
#include "inventory_source.h"

#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <string>

#include "cursesdef.h"
#include "avatar.h"      // get_avatar (native character creation)
#include "creature.h"    // Creature::attitude_raw_string (attitude badge)
#include "debug.h"       // debug_suppress_prompts (fat client load)
#include "init.h"        // DynamicDataLoader (fat client finalize guard)
#include "json.h"        // JsonOut (avatar profile serialization)
#include "json_loader.h" // profile/json cfg parsing
#include "cata_utility.h" // write_to_file / read_from_file_optional (client prefs)
#include "path_info.h"   // PATH_INFO::config_dir (client prefs)
#include "sdl_wrappers.h" // GetTicks (view probe timestamps)
#include "game.h"        // g (fat client world data load)
#include "lightmap.h"    // lit_level (client light overlay)
#include "input_context.h"
#include "input.h"
#include "inventory_ui.h"
#include "inventory.h"
#include "itype.h"
#include "flag.h"
#include "ranged.h"
#include "npc.h"
#include "loading_ui.h"  // loading_ui::done (releases the splash adaptors)
#include "output.h"
#include "uilist.h"
#include "ui_manager.h"
#include "cata_imgui.h"
#include "imgui/imgui.h"
#include "worldfactory.h" // world_generator / WORLD (fat client)

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

/* ------------------------------------------------------------------ */
/* chunk helpers                                                       */
/* ------------------------------------------------------------------ */

std::string describe_endpoint()
{
    if( host_is_running() ) {
        return string_format( _( "[%d] Listening on %d" ), host_session_count(), host_listen_port() );
    }
    return _( "[x] Not listening" );
}

/* ------------------------------------------------------------------ */
/* in-game host dialog ("Open world to network")                       */
/* ------------------------------------------------------------------ */

void show_host_dialog()
{
    input_context ctxt( "NETPLAY_HOST", keyboard_mode::keycode );
    ctxt.register_action( "CONFIRM" );
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "HELP_KEYBINDINGS" );

    while( true ) {
        ui_manager::redraw();

        const int height = 12;
        const int width  = 60;
        const int x_start = std::max( 0, ( TERMX - width ) / 2 );
        const int y_start = std::max( 0, ( TERMY - height ) / 2 );

        catacurses::window win =
            catacurses::newwin( height, width, point( x_start, y_start ) );

        werase( win );
        draw_border( win, c_white, _( "Open world to network" ) );
        mvwprintw( win, point( 2, 2 ), _( "Protocol: DDA MP 0.1  Port: %d" ), DEFAULT_PORT );
        mvwprintw( win, point( 2, 3 ), describe_endpoint() );
        if( !host_is_running() ) {
            mvwprintw( win, point( 2, 4 ),
                       _( "<ENTER>: start listener on port 25565" ) );
        } else {
            mvwprintw( win, point( 2, 4 ),
                       _( "<ENTER>: stop listener" ) );
            const std::vector<std::string> names = host_session_names();
            int line = 5;
            if( names.empty() ) {
                mvwprintw( win, point( 2, line ), _( "No players connected yet." ) );
                line++;
            } else {
                mvwprintw( win, point( 2, line ), _( "Players (%d):" ), static_cast<int>( names.size() ) );
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
                    popup( _( "Failed to bind the listener. Port %d may be in use." ), DEFAULT_PORT );
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
 * traces, view probes) � built-in (mp_diag_dir). Default ON without any
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
            WORLD *w = world_generator ? world_generator->get_world( wname ) : nullptr;
            if( w != nullptr && g != nullptr ) {
                world_generator->set_active_world( w );
                debug_suppress_prompts( true );
                g->load_world_modfiles();
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
    // Character creation needs the host's world data: the fat client
    // loaded it exactly when DynamicDataLoader finalized and the host's
    // world is the active one.
    const std::string wname = netplay::client_host_world_name();
    WORLD *w = world_generator ? world_generator->get_world( wname ) : nullptr;
    if( w == nullptr || world_generator->active_world != w ) {
        popup( _( "The host world '%s' is not available on this machine, "
                  "so a new avatar cannot be created here (watch mode only)." ), wname );
        return false;
    }
    if( !DynamicDataLoader::get_instance().is_data_finalized() ) {
        popup( _( "The host world data is not loaded on this machine, "
                  "so a new avatar cannot be created here (watch mode only)." ) );
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
static void run_avatar_join_flow( const std::string &use_nick )
{
    netplay::client_send_avatar_list_req( use_nick );
    for( int i = 0; i < 150; ++i ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
        if( !netplay::client_is_connected() ) {
            popup( _( "The host disconnected." ) );
            return;
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
        return;
    }
    if( netplay::client_avatar_bind_state() == 3 ) {
        popup( _( "The host cut the avatar exchange short: %s" ),
               netplay::client_avatar_error() );
    }

    std::vector<const netplay::avatar_entry *> alive;
    for( const netplay::avatar_entry &e : netplay::client_avatar_list() ) {
        if( e.alive ) {
            alive.push_back( &e );
        }
    }

    if( alive.empty() ) {
        // v4.0a-1: no live avatar for this nick goes straight to creation
        avatar_create_flow( use_nick );
        return;
    }

    // v4.0a-1: the host keeps one avatar per nick, so the client resumes
    // the first (only) living entry automatically � no choice is offered.
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
    }
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
        // missing/broken prefs: fall back to the defaults � never block joining
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
                   _( "<UP>/<DOWN>: select   <ENTER>: connect   <ESC>: cancel" ) );

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
                popup( _( "The host address can not be empty." ) );
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
        popup( _( "Could not connect: the host rejected the connection." ) );
        return;
    }

    // A brief welcome is likely streaming immediately; blocking poll.
    for( int i = 0; i < 50 && !client_is_connected(); i++ ) {
        std::this_thread::sleep_for( std::chrono::milliseconds( 100 ) );
    }
    if( !client_is_connected() ) {
        const std::string reason = client_deny_reason();
        if( reason.empty() ) {
            popup( _( "Connection refused: host unreachable." ) );
        } else {
            popup( _( "Connection refused: %s" ), reason );
        }
        return;
    }

    // DDA MP v3.9: ask for the current picture right after the WELCOME �
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
    // first tile frame � multi seconds with the stale main-menu frame
    // sitting on the glass. create() is idempotent (shared instance).
    if( netplay::client_is_connected() ) {
        netplay::tiles_backend_create();
    }
#endif

    run_view_loop();
    netplay::client_disconnect();
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
    if( action == "CONFIRM" ) { return 10; }
    if( action == "MENU" )  { return 11; }
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
    out.push_back( string_format( "%s %d", _( "Perception" ), d.per ) );
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
static int mp_throw_node = 0;
static int mp_throw_gen = 0;
static int mp_throw_dx = 1;
static int mp_throw_dy = 0;
static float mp_canvas_cx = 0.0f;
static float mp_canvas_cy = 0.0f;
static float mp_canvas_tw = 0.0f;
static float mp_canvas_th = 0.0f;
/* DDA MP v4.0a-41: aiming view-model. The client has no game map, so the
 * snapshot supplies the only facts the stock target_ui needs for a throw:
 * terrain blocking (smooth-light barrier bit) and creature ids. */
static std::vector<uint8_t> mp_aim_flags;
static std::vector<std::string> mp_aim_critters;
static int mp_aim_w = 0;
static int mp_aim_h = 0;
static std::string mp_throw_name;
static int mp_throw_range = 1;
static std::string mp_aim_text;
/* World origin of the last snapshot (absolute coords of the session avatar). */
static int mp_origin_x = 0;
static int mp_origin_y = 0;
static int mp_origin_z = 0;
static bool mp_have_origin = false;

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
                                 string_format( _( "Item hotkeys assigned: %d/%d" ),
                                                pkt.header.hotkeys_assigned, pkt.header.hotkeys_total ) :
                                 string_format( _( "Items: %d" ), static_cast<int>( items.size() ) );
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
                dl->AddText( ImVec2( x, y ), col_section, e->category.c_str() );
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
            const std::string nm = mp_clip_text( ( e->depth > 0 ? "> " : "" ) + e->name, name_w );
            dl->AddText( ImVec2( name_x, y ), col, nm.c_str() );
            dl->AddText( ImVec2( x + col_w - qw, y ), col_qty, qty.c_str() );
            y += lh;
            if( is_sel ) {
                // Inline details, like the host expands the highlighted entry.
                std::string d1 = string_format( "%s %s   %s %s", _( "Weight" ),
                                                mp_fmt_kg( e->weight_g ).c_str(), _( "Volume" ),
                                                mp_fmt_l( e->volume_ml ).c_str() );
                if( e->side_mm > 0 ) {
                    d1 += "   " + string_format( "%s %s", _( "Longest side" ),
                                                 mp_fmt_cm( e->side_mm ).c_str() );
                }
                std::string d2 = e->category;
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
                 _( "[Esc] close   [Up/Down] select   [Tab] column   [PgUp/PgDn] page" ) );
    if( chosen != nullptr ) {
        const std::string right = string_format( "%s: %s", _( "Category" ), chosen->category );
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

static void mp_run_client_throw( const item &thrown, int node, int generation,
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
    mp_menu_entry( menu, 'V', pgettext( "action", "view recipe" ), hint_rating::cant );
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
    menu.query();
    return menu.ret;
}

/* DDA MP v4.0a-27: the client's own @ menu. The host streams the session NPC
 * as JSON; deserializing it into a temporary NPC gives Character::disp_info()
 * the real stats/traits/skills instead of a blank shell. Gated by try/catch so
 * a bad payload can never take the client down. */
static void mp_run_character_info( const std::string &text )
{
    static const bool mp_info_disabled = []() {
        const char *env = std::getenv( "DDA_MP_NO_INFO_JSON" );
        return env != nullptr && env[0] != '\0' && std::string( env ) != "0";
    }();
    if( mp_info_disabled || text.empty() ) {
        return;
    }
    try {
        JsonValue jv = json_loader::from_string( text );
        JsonObject jo = jv.get_object();
        npc tmp;
        tmp.deserialize( jo );
        mp_inv_log( "info: opening @ menu" );
        // Same layering rule as the inventory: hide the pre-session main menu,
        // keep the @ menu and its own submenus above the blocker.
        ui_adaptor mp_info_blocker( ui_adaptor::disable_uis_below{} );
        tmp.disp_info( false );
        mp_inv_log( "info: closed" );
    } catch( const std::exception &e ) {
        mp_inv_log( std::string( "info error: " ) + e.what() );
    } catch( ... ) {
        mp_inv_log( "info error: unknown" );
    }
}

/* DDA MP v4.0a-17: exact host inventory on the client.
 *
 * Instead of re-drawing an approximation, rebuild the avatar's item tree from
 * the streamed rows (real itypes, counts, charges, invlets, container nesting)
 * on a temporary NPC and hand it to the game's own inventory_pick_selector.
 * The UI is then drawn by the same code path as on the host; actions only ever
 * touch the temporary copy, so the client avatar and host stay untouched. */
static void mp_run_host_inventory( const std::string &text )
{
    try {
    // Block only the UI that existed before the session (the main menu we
    // joined from). Created before the inventory windows, so every session
    // window and popup draws above it while pre-session UI stays hidden.
    ui_adaptor mp_session_blocker( ui_adaptor::disable_uis_below{} );
    const netplay::inv_packet pkt = netplay::parse_inv_packet( text );
    mp_inv_log( string_format( "open: rows=%d bytes=%d hotkeys=%d/%d",
                               static_cast<int>( pkt.entries.size() ),
                               static_cast<int>( text.size() ),
                               pkt.header.hotkeys_assigned, pkt.header.hotkeys_total ) );
    npc viewer;
    viewer.name = "client";

    std::size_t i = 0;
    std::function<item()> build = [&]() -> item {
        const netplay::inv_entry_view &e = pkt.entries[i];
        const int depth = e.depth;
        const std::size_t node = i;
        ++i;
        item it( itype_id( e.id ), calendar::turn );
        // Row index travels with the item; the host uses it to address the
        // very same entry when an action comes back.
        it.set_var( "mp_node", std::to_string( node ) );
        if( e.charges > 0 &&
            ( it.count_by_charges() || it.is_magazine() || it.is_ammo() || it.is_tool() ) ) {
            it.charges = e.charges;
        }
        if( e.invlet != '\0' ) {
            it.invlet = e.invlet;
        }
        it.is_favorite = e.favorite;
        if( e.damage > 0 ) {
            it.set_damage( e.damage );
        }
        if( e.filthy ) {
            it.set_flag( flag_FILTHY );
        }
        it.active = e.active;
        while( i < pkt.entries.size() && pkt.entries[i].depth > depth ) {
            item child = build();
            pocket_type pk = pocket_type::CONTAINER;
            if( !it.get_contents().has_pocket_type( pk ) ) {
                if( it.get_contents().has_pocket_type( pocket_type::MAGAZINE_WELL ) ) {
                    pk = pocket_type::MAGAZINE_WELL;
                } else if( it.get_contents().has_pocket_type( pocket_type::MOD ) ) {
                    pk = pocket_type::MOD;
                } else {
                    continue;
                }
            }
            it.force_insert_item( child, pk );
        }
        return it;
    };

    int lost = 0;
    while( i < pkt.entries.size() ) {
        const netplay::inv_entry_view &e = pkt.entries[i];
        if( e.depth != 0 ) {
            ++i;
            continue;
        }
        if( !item::type_is_defined( itype_id( e.id ) ) ) {
            const int depth = e.depth;
            ++i;
            while( i < pkt.entries.size() && pkt.entries[i].depth > depth ) {
                ++i;
            }
            continue;
        }
        item it = build();
        if( e.wielded ) {
            viewer.set_wielded_item( it );
        } else if( e.worn ) {
            if( !viewer.wear_item( it, false, true, false, true ) ) {
                if( !viewer.i_add( it, false, nullptr, nullptr, false, false ) ) {
                    ++lost;
                    mp_inv_log( "warn: cannot wear/store " + e.id );
                }
            }
        } else {
            if( !viewer.i_add( it, false, nullptr, nullptr, false, false ) ) {
                ++lost;
                mp_inv_log( "warn: cannot store " + e.id );
            }
        }
    }
    if( lost > 0 ) {
        mp_inv_log( string_format( "warn: %d top-level items lost", lost ) );
    }

    // Same preset/state as game_menus::inv::common, so the client's category
    // view/sort mode matches the host's inventory instead of a fresh default.
    inventory_selector_preset inv_preset = default_preset;
    inv_preset.save_state = &inventory_ui_default_state;
    inventory_pick_selector sel( viewer, inv_preset );
    sel.set_title( _( "Inventory" ) );
    int assigned = 0;
    for( const netplay::inv_entry_view &e : pkt.entries ) {
        if( e.invlet != '\0' ) {
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
        const std::string node_var = it.get_var( "mp_node", std::string() );
        if( node_var.empty() ) {
            mp_inv_log( "action: row has no mp_node id, ignored" );
            break;
        }
        const int node = std::atoi( node_var.c_str() );
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
            std::vector<int> insert_nodes;
            for( const drop_location &dl : picked ) {
                const std::string src_node = dl.first->get_var( "mp_node", std::string() );
                if( !src_node.empty() ) {
                    insert_nodes.push_back( std::atoi( src_node.c_str() ) );
                }
            }
            if( !insert_nodes.empty() ) {
                netplay::client_send_inventory_action(
                    node, 'i', pkt.header.generation, insert_nodes );
            }
            mp_inv_log( string_format( "action: node=%d key=i targets=%d", node,
                                       static_cast<int>( insert_nodes.size() ) ) );
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
            netplay::client_send_inventory_settings( node, pkt.header.generation, os.str() );
            mp_inv_log( string_format( "action: node=%d key=v settings", node ) );
            break;
        }
        if( act == '=' ) {
            const int invlet = popup_getkey(
                                   _( "Enter new letter.  Press SPACE to clear a manually-assigned letter, ESCAPE to cancel." ) );
            if( invlet != KEY_ESCAPE && ( invlet == ' ' || inv_chars.valid( invlet ) ) ) {
                netplay::client_send_inventory_action( node, '=', pkt.header.generation,
                                                       { invlet } );
            }
            break;
        }
        if( act == 't' ) {
            static const bool mp_legacy_throw = []() {
                const char *env = std::getenv( "DDA_MP_LEGACY_THROW" );
                return env != nullptr && env[0] != '\0' && std::string( env ) != "0";
            }();
            // Running the stock target_ui against the client's local map needs
            // a fully initialised client game/map; it is opt-in until the
            // client has a real map view (otherwise it can hard-crash).
            static const bool mp_try_host_aim = []() {
                const char *env = std::getenv( "DDA_MP_TRY_HOST_AIM" );
                return env != nullptr && env[0] != '\0' && std::string( env ) != "0";
            }();
            if( !mp_legacy_throw && mp_try_host_aim && mp_have_origin ) {
                mp_run_client_throw( it, node, pkt.header.generation,
                                     tripoint_abs_ms( mp_origin_x, mp_origin_y, mp_origin_z ) );
            } else {
                mp_throw_node = node;
                mp_throw_gen = pkt.header.generation;
                mp_throw_dx = 1;
                mp_throw_dy = 0;
                mp_throw_name = it.display_name();
                mp_throw_range = std::max( 1, viewer.throw_range( it ) );
                mp_throw_active = true;
                mp_aim_text.clear();
                netplay::client_send_aim_info( node, pkt.header.generation,
                                               mp_throw_dx, mp_throw_dy );
            }
            break;
        }
        if( act > 0 ) {
            netplay::client_send_inventory_action( node, act, pkt.header.generation );
            mp_inv_log( string_format( "action: node=%d key=%c gen=%d", node,
                                       static_cast<char>( act ), pkt.header.generation ) );
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

/* DDA MP v4.0a-39: the host's real aiming UI on the client.
 *
 * target_ui is now Character-based. The client's local avatar is temporarily
 * moved to the streamed world origin (the session avatar's position) and the
 * bubble is re-anchored, so g->draw/mode_throw show the host terrain and the
 * stock reticle. Only the chosen offset is sent to the host. */
static void mp_run_client_throw( const item &thrown, int node, int generation,
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
            netplay::client_send_inventory_action(
                node, 't', generation,
                { tgt.x() - from.x(), tgt.y() - from.y() } );
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
        if( mp_log.size() >= 200 ) {
            mp_log.erase( mp_log.begin() );
        }
        mp_log.push_back( { s, type, mp_cur_turn } );
    };
    static float mp_sb_overlay_w = 0.0f;
    static minimap_frame mp_mini_map;
    static bool mp_inv_open = false;
    static int mp_inv_scroll = 0;
    static int mp_inv_sel = 0;
    static std::string mp_inv_text;
    static std::string mp_char_json;
    // DDA MP v4.0a-10: draw the sidebar in the pixel layer (ImGui) so its
    // position matches the host's exactly and does not depend on the curses
    // grid scale.
    // DDA MP v4.0a-18: a permanent disable_uis_below made the client redraw
    // the whole frame every iteration (visible flicker), so the main menu is
    // hidden only while a popup is open (see mp_client_item_menu).
    ui_adaptor mp_sb_adaptor;
    mp_sb_adaptor.on_redraw( [&]( const ui_adaptor & ) {
        const ImGuiIO &io = ImGui::GetIO();
        ImDrawList *dl = ImGui::GetForegroundDrawList();
        const float fs = ImGui::GetFontSize();
        const float pad = fs * 0.6f;
        // (throw reticle is drawn by the tileset pass, see line_trail/cursor)
        // DDA MP v4.0a-14: inventory panel (read-only, toggled with 'i').
        if( !mp_side.cells.empty() ) {
            const float cw = ImGui::CalcTextSize( "M" ).x;
            const float ch = ImGui::GetTextLineHeight();
            const float x0 = std::max( 0.0f, io.DisplaySize.x - mp_side.width * cw - cw );
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
            int draw_y = 0;
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
            // DDA MP v4.0a-11: the client's own message log, under the panels.
            float ly = draw_y * ch;
            // DDA MP v4.0a-13: creatures this character can see (own list).
            int mp_shown = 0;
            for( const minimap_frame::beacon &bb : mp_mini_map.beacons ) {
                if( bb.name.empty() || mp_shown >= 12 || ly + ch > io.DisplaySize.y ) {
                    continue;
                }
                dl->AddText( ImVec2( x0, ly ),
                             cataimgui::ImU32_from_color(
                                 decode_cell_color( static_cast<int>( bb.color ) ) ),
                             ( bb.sym + " " + bb.name ).c_str() );
                ly += ch;
                ++mp_shown;
            }
            // Minimap is anchored at the bottom; the log fills the space above.
            const float mp_mini_top = mp_mini_map.width > 0 ?
                                      io.DisplaySize.y - mp_mini_map.height * 2.0f - 2.0f :
                                      io.DisplaySize.y;
            const int mp_fit = static_cast<int>( ( mp_mini_top - ch - ly ) / ch );
            if( mp_fit > 0 && !mp_log.empty() ) {
                const int first = std::max( 0, static_cast<int>( mp_log.size() ) - mp_fit );
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
                const float mp_panel_w = mp_side.width > 0 ? mp_side.width * cw : ( io.DisplaySize.x - x0 );
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
    } );

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

        while( client_is_connected() ) {
            // DDA MP v4.0a-9: report our terminal size so the host renders the
            // sidebar to fit instead of letting the client clip it.
            {
                static int mp_last_cols = -1;
                static int mp_last_rows = -1;
                if( TERMX != mp_last_cols || TERMY != mp_last_rows ) {
                    mp_last_cols = TERMX;
                    mp_last_rows = TERMY;
                    netplay::client_send_view( TERMX, TERMY );
                }
            }
            // DDA MP v4.0a-7: turn-pacing hint from the host ("waiting for
            // other players..."), shown above the picture and in the title.
            {
                std::string mp_hint;
                if( netplay::client_take_hover( mp_hint ) ) {
                    mp_hint_text = mp_hint;
                    netplay::client_status_hint( mp_hint );
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
                                mp_log_add( string_format( "Потеряно %d HP", mp_prev.hp - d.hp ),
                                            static_cast<int>( m_bad ) );
                            } else if( d.hp > mp_prev.hp ) {
                                mp_log_add( string_format( "Восстановлено %d HP", d.hp - mp_prev.hp ),
                                            static_cast<int>( m_good ) );
                            }
                            if( d.hunger >= mp_prev.hunger + 10 ) {
                                mp_log_add( string_format( "Голод: %d", d.hunger ),
                                            static_cast<int>( m_warning ) );
                            }
                            if( d.thirst >= mp_prev.thirst + 10 ) {
                                mp_log_add( string_format( "Жажда: %d", d.thirst ),
                                            static_cast<int>( m_info ) );
                            }
                            if( d.pain > mp_prev.pain ) {
                                mp_log_add( string_format( "Боль: %d", d.pain ),
                                            static_cast<int>( m_bad ) );
                            }
                            if( d.effects != mp_prev.effects ) {
                                mp_log_add( "Состояние изменилось", static_cast<int>( m_info ) );
                            }
                        }
                        mp_prev = d;
                    }
                }
                // Host-rendered panels (preferred: identical look).
                netplay::client_sidebar( mp_side );
                netplay::client_minimap( mp_mini_map );
                {
                    std::string mp_inv;
                    if( netplay::client_inventory( mp_inv ) ) {
                        mp_inv_text = std::move( mp_inv );
                    }
                }
                {
                    std::string mp_cj;
                    if( netplay::client_character_json( mp_cj ) ) {
                        mp_char_json = std::move( mp_cj );
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
                    mp_log_add( mp_ev_text, mp_ev_type );
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
            // windows repaint at random moments inside the input pump �
            // the screen alternated between stamped tiles and the cleared
            // black buffer (the observed flicker).
            ui_manager::redraw();

            bool mp_canvas_ok = false;
            snapshot snap;
            if( client_receive( snap ) && snap.width > 0 && snap.height > 0
                && snap.width <= SNAP_MAX && snap.height <= SNAP_MAX
                && snap.cells.size() == static_cast<size_t>( snap.width ) * snap.height ) {
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
                    for( size_t ci = 0; ci < snap.tiles.size() &&
                         ci < mp_aim_critters.size(); ci++ ) {
                        mp_aim_critters[ci] = snap.tiles[ci].critter;
                    }
                    if( snap.tiles.size() == snap.cells.size() ) {
                        for( size_t ti = 0; ti < snap.tiles.size(); ti++ ) {
                            mp_aim_flags[ti] = snap.tiles[ti].flags;
                        }
                    }
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
                    draw_border( win, c_white, _( "Cata server" ) );

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
                            // Shift the picture left by half the sidebar width: the
                            // host keeps the map and its sidebar side by side, and the
                            // streamed sidebar must not sit under the sprites.
                            const int dest_x = center_px_x - canvas_w / 2 -
                                               static_cast<int>( mp_sb_overlay_w / 2.0f );
                            const int dest_y = center_px_y - canvas_h / 2;
                            // Throw crosshair anchor: the avatar sits at the
                            // centre of the streamed canvas.
                            mp_canvas_cx = static_cast<float>( center_px_x ) -
                                           mp_sb_overlay_w / 2.0f;
                            mp_canvas_cy = static_cast<float>( center_px_y );
                            mp_canvas_tw = static_cast<float>( tw );
                            mp_canvas_th = static_cast<float>( th );
                            constexpr int PIVOT = 32; // inside the 48-tile bubble
                            // DDA MP v4.0a-4: feed the streamed absolute origin
                            // so sprite variant seeds are world-stable.
                            netplay::tiles_backend_set_origin( tiles_backend,
                                                               snap.origin_x, snap.origin_y,
                                                               snap.origin_z, snap.has_origin );
                            if( netplay::tiles_backend_begin( tiles_backend, dest_x, dest_y,
                                                              canvas_w, canvas_h, PIVOT, PIVOT ) ) {
                                mp_canvas_ok = true;
                                mp_frame_trace( "begin ok" );
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
                                // DDA MP: prepass � stream every cell's
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
                                        // DDA MP: pass the cell's real lit_level �
                                        // the lit frame (or, in the fallback, the
                                        // overlay below) replicas the host's light.
                                        const int mp_lit = static_cast<int>( t.light );
                                        if( !t.ter.empty() ) {
                                            trace_draw( 0, t.ter.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 0,
                                                    t.ter.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit,
                                                    static_cast<int>( t.ter_sub ),
                                                    static_cast<int>( t.ter_rot ) );
                                        }
                                        if( !t.trap.empty() ) {
                                            trace_draw( 5, t.trap.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 5,
                                                    t.trap.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, 0 );
                                        }
                                        if( !t.furn.empty() ) {
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
                                        for( const tile_item_layer &l : t.item_layers ) {
                                            trace_draw( 2, l.sprite.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 2,
                                                    l.sprite.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, 0, false,
                                                    l.variant.c_str(),
                                                    static_cast<int>( l.dx ),
                                                    static_cast<int>( l.dy ) );
                                        }
                                        if( !t.item.empty() && !t.item_skip_top ) {
                                            trace_draw( 2, t.item.c_str() );
                                            // v3.9.1: the itype variant the host
                                            // drew this item with (see tile_cell).
                                            netplay::tiles_backend_draw( tiles_backend, 2,
                                                    t.item.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, 0,
                                                    false, t.item_variant.c_str() );
                                        }
                                        if( !t.vpart.empty() ) {
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
                                            // heard-sound "?" � the host places
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
                                if( mp_throw_active ) {
                                    const int acx = snap.width / 2;
                                    const int acy = snap.height / 2;
                                    int tx = acx + mp_throw_dx;
                                    int ty = acy + mp_throw_dy;
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
                                netplay::tiles_backend_end( tiles_backend );
                                // DDA MP v4.0a-37: the reticle must be drawn in
                                // the post-canvas overlay pass; the session
                                // adaptor pass can end up below the tiles.
                                if( mp_throw_active && mp_canvas_tw > 0.0f &&
                                    mp_canvas_th > 0.0f ) {
                                    netplay::overlay_begin();
                                    ImDrawList *tdl = ImGui::GetForegroundDrawList();
                                    const float fsz = ImGui::GetFontSize();
                                    const float lh = ImGui::GetTextLineHeight();
                                    // Host-authored aiming panel (range,
                                    // target, hit chances) on the right side,
                                    // like target_ui's w_target window.
                                    std::vector<std::string> lines;
                                    {
                                        std::istringstream ls( mp_aim_text );
                                        std::string line;
                                        while( std::getline( ls, line ) ) {
                                            lines.push_back( line );
                                        }
                                    }
                                    const float pw = fsz * 30.0f;
                                    const float ph = lh * ( static_cast<float>( lines.size() ) + 2.0f ) +
                                                     12.0f;
                                    const float px0 = std::max( 8.0f,
                                                                ImGui::GetIO().DisplaySize.x - pw - 8.0f );
                                    tdl->AddRectFilled( ImVec2( px0, 8.0f ),
                                                        ImVec2( px0 + pw, 8.0f + ph ),
                                                        IM_COL32( 0, 0, 0, 230 ) );
                                    tdl->AddRect( ImVec2( px0, 8.0f ),
                                                  ImVec2( px0 + pw, 8.0f + ph ),
                                                  IM_COL32( 190, 190, 190, 255 ), 0.0f, 0, 1.0f );
                                    float ty = 8.0f + lh * 0.4f;
                                    tdl->AddText( ImVec2( px0 + 8, ty ), IM_COL32( 255, 255, 255, 255 ),
                                                  mp_clip_text( string_format( "%s %s",
                                                              _( "Throwing:" ), mp_throw_name ),
                                                              pw - 16 ).c_str() );
                                    ty += lh;
                                    for( const std::string &line : lines ) {
                                        tdl->AddText( ImVec2( px0 + 8, ty ),
                                                      IM_COL32( 215, 215, 215, 255 ),
                                                      mp_clip_text( line, pw - 16 ).c_str() );
                                        ty += lh;
                                    }
                                    tdl->AddText( ImVec2( px0 + 8, ty ), IM_COL32( 190, 190, 190, 255 ),
                                                  _( "[Enter] throw   [Esc] cancel" ) );
                                    netplay::overlay_end();
                                }
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
                // DDA MP v3.9: no valid picture yet � hold a quiet
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
            if( !mp_hint_text.empty() ) {
                const int mp_hint_w = std::min( TERMX, 48 );
                if( !mp_hint_win ) {
                    mp_hint_win = catacurses::newwin( 1, mp_hint_w,
                                                      point( std::max( 0, ( TERMX - mp_hint_w ) / 2 ), 0 ) );
                }
                werase( mp_hint_win );
                std::string s = mp_hint_text;
                if( static_cast<int>( s.size() ) > mp_hint_w ) {
                    s.resize( mp_hint_w );
                }
                mvwprintz( mp_hint_win, point( 0, 0 ), c_yellow, s );
                wnoutrefresh( mp_hint_win );
            }

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

            const std::string action = ctxt.handle_input();
            // DDA MP v4.0a-35: throw targeting (arrows move the crosshair).
            if( mp_throw_active ) {
                const input_event mp_ev = ctxt.get_raw_input();
                const int mp_key = mp_ev.get_first_input();
                if( mp_key == 27 || action == "QUIT" ) {
                    mp_throw_active = false;
                    mp_aim_text.clear();
                    continue;
                }
                if( action == "UP" ) {
                    --mp_throw_dy;
                    netplay::client_send_aim_info( mp_throw_node, mp_throw_gen,
                                                   mp_throw_dx, mp_throw_dy );
                    continue;
                }
                if( action == "DOWN" ) {
                    ++mp_throw_dy;
                    netplay::client_send_aim_info( mp_throw_node, mp_throw_gen,
                                                   mp_throw_dx, mp_throw_dy );
                    continue;
                }
                if( action == "LEFT" ) {
                    --mp_throw_dx;
                    netplay::client_send_aim_info( mp_throw_node, mp_throw_gen,
                                                   mp_throw_dx, mp_throw_dy );
                    continue;
                }
                if( action == "RIGHT" ) {
                    ++mp_throw_dx;
                    netplay::client_send_aim_info( mp_throw_node, mp_throw_gen,
                                                   mp_throw_dx, mp_throw_dy );
                    continue;
                }
                if( action == "CONFIRM" || mp_key == '\n' || mp_key == '\r' ) {
                    const float dist = std::sqrt(
                                           static_cast<float>( mp_throw_dx * mp_throw_dx +
                                                   mp_throw_dy * mp_throw_dy ) );
                    if( dist > mp_throw_range + 0.5f ) {
                        continue; // host target_ui does not accept out-of-range aims
                    }
                    netplay::client_send_inventory_action(
                        mp_throw_node, 't', mp_throw_gen,
                        { mp_throw_dx, mp_throw_dy } );
                    mp_inv_log( string_format( "action: node=%d key=t target=%d,%d",
                                               mp_throw_node, mp_throw_dx, mp_throw_dy ) );
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
                // DDA MP: ESC opens an in-game-style menu instead of instantly
                // disconnecting the session.
                uilist mp_menu;
                mp_menu.text = _( "Netplay session" );
                mp_menu.addentry( 0, true, 'c', _( "Continue watching" ) );
                mp_menu.addentry( 1, true, 'd', _( "Disconnect from server" ) );
                mp_menu.query();
                if( mp_menu.ret == 1 ) {
                    voluntary_quit = true;
                    break;
                }
                // Continue: fall through to the next input cycle.
            }
            const long code = key_to_remote( action );
            if( code != 0 ) {
                key_event ke;
                ke.code = static_cast<uint32_t>( code );
                client_send_keys( { ke } );
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
            popup( _( "Could not reconnect: the host is unreachable." ) );
            break;
        }
        fat_client_load_data();
        if( !client_is_connected() ) {
            const std::string reason = client_deny_reason();
            if( reason.empty() ) {
                popup( _( "Connection refused: host unreachable." ) );
            } else {
                popup( _( "Connection refused: %s" ), reason );
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
