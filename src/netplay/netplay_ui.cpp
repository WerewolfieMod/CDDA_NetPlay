// CataDDA Multiplayer ("DDA MP") вЂ” user-facing flows:
//  * in-game "Open world to network" host dialog,
//  * in-game/main-menu "Join by IPвЂ¦" client flow,
//  * the remote-session fullscreen ASCII viewer (`run_view_loop`).

#include "netplay.h"

#include <array>
#include <cmath>
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
#include "loading_ui.h"  // loading_ui::done (releases the splash adaptors)
#include "output.h"
#include "uilist.h"
#include "ui_manager.h"
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
 * traces, view probes). Flip to true and rebuild to re-enable. */
constexpr bool mp_netplay_diag = false;

// Appends to netplay_tiles.log (same channel as the replay renderer trace).
static void mp_fat_log( const std::string &msg )
{
    if( !mp_netplay_diag ) {
        return;
    }
    try {
        std::ofstream f( "netplay_tiles.log", std::ios::app );
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
    // the first (only) living entry automatically — no choice is offered.
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
        // missing/broken prefs: fall back to the defaults — never block joining
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

    // DDA MP v3.9: ask for the current picture right after the WELCOME —
    // the host answers while the client loads its world data, so the view
    // loop starts with a frame already buffered and shows tiles instantly.
    // If the host plays during the load, its tick snapshots keep the buffer
    // fresh; the view loop's own request loop stays as a fallback.
    netplay::client_request_snapshot();

    // DDA MP fat client: the host announced its world in WELCOME. Load its
    // mod data now вЂ” before entering the fullscreen view вЂ” so the loading
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
    // first tile frame — multi seconds with the stale main-menu frame
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
            std::ofstream f( "netplay_tiles.log", std::ios::app );
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
    // offer вЂ” sudden disconnects offer to rejoin instead.
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
        ctxt.set_timeout( 150 ); // view mode cadence; handle_input blocks up to this
        ctxt.register_action( "QUIT" );
        ctxt.register_action( "ANY_INPUT" );
        ctxt.register_directions();

        while( client_is_connected() ) {
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
            // windows repaint at random moments inside the input pump —
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
                ensure_window( snap.width, snap.height );
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
                            const int dest_x = center_px_x - canvas_w / 2;
                            const int dest_y = center_px_y - canvas_h / 2;
                            constexpr int PIVOT = 32; // inside the 48-tile bubble
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
                                // DDA MP: prepass — stream every cell's
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
                                        // DDA MP: pass the cell's real lit_level —
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
                                    }
                                }
                                netplay::tiles_backend_end( tiles_backend );
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
                // DDA MP v3.9: no valid picture yet — hold a quiet
                // placeholder over the menu instead of flashing it. redraw()
                // above already regenerated the registered windows; the
                // stdscr paint wraps them into the neutral wait screen. Safe
                // here: this iteration paints no tileset canvas.
                werase( catacurses::stdscr );
                mvwprintz( catacurses::stdscr, point( 1, 1 ), c_light_gray,
                           _( "Waiting for the host picture..." ) );
                wnoutrefresh( catacurses::stdscr );
            }

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
