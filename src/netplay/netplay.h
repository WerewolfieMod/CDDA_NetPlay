#pragma once
// CataDDA Multiplayer ("DDA MP") — netplay core API.
//
// Architecture (v1):
//  - Host (authority): the normal single-player game process, plus a TCP
//    listener on DEFAULT_PORT. Remote players join over TCP and control
//    per-session avatars. The host streams self-contained "screen frames"
//    (text-mode cells) for each remote session and rank-input keys back.
//  - Client: connects to a host IP, receives frames, renders them in a
//    local fullscreen window, sends keystrokes/actions upstream.
//
// The netplay layer is intentionally decoupled from game internals; the
// glue lives in the game-side hooks (main_menu, handle_action, main).

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace netplay
{

/// Default TCP port (player request: Minecraft-style 25565).
constexpr int DEFAULT_PORT = 25565;

/// Frame opcodes.
enum class frame : int {
    HELLO = 1,      ///< client -> host: protocol hi, player name
    WELCOME = 2,    ///< host -> client: session params
    DENY = 3,       ///< host -> client: reject, text reason
    KEYS = 4,       ///< client -> host: input event batch
    SNAPSHOT = 5,   ///< host -> client: full/delta screen frame
    HOVER = 6,      ///< host -> client: hud/status text payload
    PING = 7,
    PONG = 8,
    BYE = 9,        ///< either side: graceful close
};

/// One screen cell as streamed in SNAPSHOT frames.
struct cell {
    int glyph;      ///< unicode codepoint
    int color;      ///< ncurses-style color pair id (pair number)
};

/// Full screen frame for a remote session.
struct snapshot {
    int width = 0;
    int height = 0;
    std::vector<cell> cells;
    std::string status_line;
};

/// One keyboard pseudo-event from the client.
struct key_event {
    uint32_t code = 0;   ///< engine-neutral key code
    int32_t mods = 0;
};

class netplay_error : public std::runtime_error
{
    public:
        using std::runtime_error::runtime_error;
};

/** One-time subsystem setup/teardown (WinSock on Windows). Call once. */
bool init();
void shutdown();

/* ---- host bindings ---- */

/// Start the host listener; port 25565 unless overridden. Idempotent.
bool host_start( int port = DEFAULT_PORT );
/// Stop the host.  Idempotent.
void host_stop();
/// True while host is listening.
bool host_is_running();
/// Port the host is bound to (DEFAULT_PORT when off).
int host_listen_port();
/// Address string of the host, e.g. "192.168.1.10:25565".
std::string host_listen_address();
/// Number of currently connected sessions.
int host_session_count();
/// Per-turn maintenance (accept/expire, flush queues). Safe to call often.
void host_pump();

/* ---- client bindings ---- */

bool client_connect( const std::string &host, int port = DEFAULT_PORT );
void client_disconnect();
bool client_is_connected();
std::string client_deny_reason();
/// Take the freshest received snapshot (empty if none yet).
bool client_receive( snapshot &out );
/// Send an input batch upstream.
void client_send_keys( const std::vector<key_event> &keys );
/// Per-turn maintenance. Safe to call often.
void client_pump();

/* ---- interactive glass ---- */

/// In-game menu: "Open world to network" (host) dialogs.
void show_host_dialog();
/// In-game menu: client "Join by IP" console dialogs.
void show_join_dialog();
/// Main-menu "Join by IP": ask IP, connect, drop into remote-view mode.
/// Returns true when the user went into a net session that ended.
void run_view_loop();

} // namespace netplay
