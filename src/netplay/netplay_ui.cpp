// CataDDA Multiplayer ("DDA MP") — user-facing flows:
//  * in-game "Open world to network" host dialog,
//  * in-game/main-menu "Join by IP…" client flow,
//  * the remote-session fullscreen ASCII viewer (`run_view_loop`).

#include "netplay.h"

#include <cmath>
#include <string>

#include "cursesdef.h"
#include "input_context.h"
#include "output.h"
#include "ui_manager.h"

#include "string_input_popup.h"
#include "catacharset.h"
#include "color.h"
#include "string_formatter.h"
#include "translations.h"

#include <thread>

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
    const input_context ctxt( "NETPLAY_HOST", keyboard_mode::keycode );
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
                       _( "<ENTER>: stop listener  <S>: show players" ) );
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
/* join dialogs                                                        */
/* ------------------------------------------------------------------ */

void show_join_dialog()
{
    const std::string addr = string_input_popup()
        .title( _( "Host address (like 127.0.0.1 or 192.168.1.5:25565)" ) )
        .text( "127.0.0.1" )
        .width( 40 )
        .query();

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

    run_view_loop();
    netplay::client_disconnect();
}

/* ------------------------------------------------------------------ */
/* remote viewer / remote console                                      */
/* ------------------------------------------------------------------ */

static long key_to_remote( const std::string &action )
{
    // v1 maps a fixed action namespace to neutral codes.
    if( action == "LEFT" )  { return 1; }
    if( action == "RIGHT" ) { return 2; }
    if( action == "UP" )    { return 3; }
    if( action == "DOWN" )  { return 4; }
    if( action == "CONFIRM" ) { return 10; }
    if( action == "MENU" )  { return 11; }
    return 0; // no map
}

void run_view_loop()
{
    const input_context ctxt( "NETPLAY_VIEW", keyboard_mode::keycode );
    ctxt.set_timeout( 100 );     // ~10 fps cadence for view mode
    ctxt.register_action( "QUIT" );
    ctxt.register_action( "ANY_INPUT" );
    ctxt.register_directions();

    while( client_is_connected() ) {
        ui_manager::redraw();

        snapshot snap;
        if( !client_receive( snap ) ) {
            continue;
        }

        const int width  = snap.width;
        const int height = snap.height;

        catacurses::window win = catacurses::newwin(
                                     height + 2, width + 2,
                                     point( ( TERMX - ( width + 2 ) ) / 2,
                                            ( TERMY - ( height + 2 ) ) / 2 ) );
        werase( win );
        draw_border( win, c_white, _( "Cata server" ) );

        for( int y = 0; y < height; y++ ) {
            for( int x = 0; x < width; x++ ) {
                const cell &c = snap.cells[y * width + x];
                mvwputch( win, point( 1 + x, 1 + y ), static_cast<nc_color>( c.color ),
                          static_cast<int>( c.glyph ) );
            }
        }
        wnoutrefresh( win );

        const std::string action = ctxt.handle_input();
        if( action == "QUIT" ) {
            break;
        }
        const long code = key_to_remote( action );
        if( code != 0 ) {
            key_event ke;
            ke.code = static_cast<uint32_t>( code );
            client_send_keys( { ke } );
        }
    }
}

} // namespace netplay
