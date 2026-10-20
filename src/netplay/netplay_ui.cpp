// CataDDA Multiplayer ("DDA MP") вЂ” user-facing flows:
//  * in-game "Open world to network" host dialog,
//  * in-game/main-menu "Join by IPвЂ¦" client flow,
//  * the remote-session fullscreen ASCII viewer (`run_view_loop`).

#include "netplay.h"

#include <array>
#include <cmath>
#include <string>

#include "cursesdef.h"
#include "debug.h"       // debug_suppress_prompts (fat client load)
#include "game.h"        // g (fat client world data load)
#include "init.h"        // DynamicDataLoader (fat client finalize guard)
#include "lightmap.h"    // lit_level (client light overlay)
#include "input_context.h"
#include "loading_ui.h"  // loading_ui::done (releases the splash adaptors)
#include "output.h"
#include "uilist.h"
#include "ui_manager.h"
#include "worldfactory.h" // world_generator / WORLD (fat client)

#include "string_input_popup.h"
#include "catacharset.h"
#include "color.h"
#include "string_formatter.h"
#include "translations.h"

#include <fstream>
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

// Appends to netplay_tiles.log (same channel as the replay renderer trace).
static void mp_fat_log( const std::string &msg )
{
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
/* join dialogs                                                        */
/* ------------------------------------------------------------------ */

void show_join_dialog()
{
    const std::string addr = string_input_popup()
        .title( _( "Host address (like 127.0.0.1 or 192.168.1.5:25565)" ) )
        .text( "127.0.0.1" )
        .width( 40 )
        .query_string();

    if( addr.empty() ) {
        return;
    }
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

    // DDA MP fat client: the host announced its world in WELCOME. Load its
    // mod data now вЂ” before entering the fullscreen view вЂ” so the loading
    // starts immediately upon the user's join confirmation, not after the
    // first input is pumped in the view loop.
    fat_client_load_data();

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
            // Clear stale UI remnants (e.g. debug-prompt text no window
            // repaints) before the windows redraw into a clean buffer.
            if( tiles_backend != nullptr && netplay::tiles_replay_enabled() ) {
                netplay::tiles_backend_clear_screen( tiles_backend );
            }
#endif
            ui_manager::redraw();

            snapshot snap;
            if( client_receive( snap ) && snap.width > 0 && snap.height > 0
                && snap.width <= SNAP_MAX && snap.height <= SNAP_MAX
                && snap.cells.size() == static_cast<size_t>( snap.width ) * snap.height ) {
                ensure_window( snap.width, snap.height );
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
                                mp_frame_trace( "begin ok" );
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
                                        if( !t.item.empty() ) {
                                            trace_draw( 2, t.item.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, 2,
                                                    t.item.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, 0 );
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
                                            trace_draw( kind, t.critter.c_str() );
                                            netplay::tiles_backend_draw( tiles_backend, kind,
                                                    t.critter.c_str(), "", x - snap.width / 2,
                                                    y - snap.height / 2, mp_lit, 0, 0 );
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
            }

            // Present the freshly drawn frame. The game's own main loop calls
            // refresh_display() every iteration; here input pumping alone is
            // not a reliable present trigger, so the first frame would sit
            // in the buffer until the first keypress without this.
            refresh_display();

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
