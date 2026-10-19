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

/// Distance-agnostic socket handle (sockfd on POSIX; SOCKET on Windows).
using socket_t = std::int64_t;

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

/// Optional per-cell sprite info supporting the tileset replay renderer
/// (client-side). All strings carry CDDA tile/image ids; empty = the ascii
/// fallback (cell glyph/color) is used for that layer.
struct tile_cell {
    uint8_t light = 0;      ///< lit_level as plain int (see lightmap.h)
    std::string ter;        ///< terrain id ("t_grass")
    std::string furn;       ///< furniture id, also vehicle part from memory
    std::string trap;       ///< trap id
    std::string critter;    ///< monster type id
    std::string item;       ///< uppermost ground item type id
    /// Vehicle part tileset id as resolved by the host renderer
    /// ("vp_{id}" or "vp_{id}_#{variant}"), empty = no vehicle here.
    std::string vpart;
    /// MULTI_TILE subtile + rotation as resolved by the host renderer
    /// (0..7 / 0..3, see MULTITILE_TYPE). Empty ids ignore these.
    uint8_t ter_sub = 0;
    uint8_t ter_rot = 0;
    uint8_t furn_sub = 0;
    uint8_t furn_rot = 0;
    uint8_t vpart_sub = 0;
    uint8_t vpart_rot = 0;
    /// bit0 = visible items on the cell (draw the highlight_item marker),
    /// bit1 = more than one item (pile indicator),
    /// bit2 = the vpart on this cell has cargo with items (part badge),
    /// bit3 = the cell blocks light (a smooth-lighting barrier; the client
    ///        builds its own reach masks from this so the smooth filter
    ///        keeps light out of walled areas),
    /// bit4 = the light texel color bytes below are valid (the host runs
    ///        smooth lighting and sampled its light map for this cell)
    uint8_t flags = 0;
    /// Smooth-lighting light texel color when flags & 16: r = light 0..255,
    /// g/b = hue shares. The client's smooth filter and lit shader consume
    /// these exactly like the host's own light map, so the continuous
    /// gradient and any colored light match cell for cell. The texel's a
    /// channel is not sent: detail is derivable from `light` (lit_level)
    /// and the barrier bit rides on flags.
    uint8_t light_r = 0;
    uint8_t light_g = 0;
    uint8_t light_b = 0;
};

/// Full screen frame for a remote session.
struct snapshot {
    int width = 0;
    int height = 0;
    std::vector<cell> cells;
    std::string status_line;
    /// Optional parallel sprite payload, same layout as cells. May be empty
    /// (host without tile mining, synthetic test frames...).
    std::vector<tile_cell> tiles;
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
/// Display names of the currently connected sessions (copy, stable order).
std::vector<std::string> host_session_names();
/// Per-turn maintenance (accept/expire, flush queues). Safe to call often.
void host_pump();

/// Broadcast a screen frame to every live session (thread-safe; session
/// sockets that fail to receive are dropped).
void host_broadcast_snapshot( const snapshot &snap );

/// Pop one queued client key code (0 = none arrived since last call).
/// Returns false when the queue is empty.
bool host_pop_key( uint32_t &code );

/* ---- host game glue (netplay_world.cpp; needs game internals) ---- */

/// Game-side per-turn hook: builds and broadcasts an ascii snapshot of the
/// host's current view (~45x25 cells around the host avatar). Call at the
/// end of world turns; no-op when not hosting or no sessions.
void host_game_tick();
/// Convert one queued client key into a DEFAULTMODE action id string.
/// Returns false when there is nothing queued (or the code isn't mapped).
bool host_pop_action( std::string &action );

/* ---- client bindings ---- */

bool client_connect( const std::string &host, int port = DEFAULT_PORT );
void client_disconnect();
bool client_is_connected();
std::string client_deny_reason();
/// Address of the last successfull client_connect("host:port") attempt;
/// empty when no session was ever started. Used for reconnect prompts.
std::string client_last_address();
/// Take the freshest received snapshot (empty if none yet).
bool client_receive( snapshot &out );
/// Send an input batch upstream.
void client_send_keys( const std::vector<key_event> &keys );
/// Per-turn maintenance. Safe to call often.
void client_pump();

/* ---- fat client: host world static data ----
 *
 * Host announces its active world name in WELCOME; the client then loads
 * that world's mod data (game::load_world_modfiles) BEFORE the tileset
 * replay backend is created, so tileset layering contexts (and item
 * templates) resolve the same way they do on the host. Requires the client
 * to run from the same installation as the host (shared worlds directory).
 */
void host_set_world_name( const std::string &name );
std::string client_host_world_name();

/* ---- game-side glass ---- */
void show_host_dialog();
void show_join_dialog();
void run_view_loop();

/// True unless DDA_MP_TILES=0 in the environment (debug bisection switch
/// for the tileset replay feature: disables host mining AND client drawing).
bool tiles_replay_enabled();

#if defined(TILES)
/* ---- tileset replay backend (implemented in sdltiles.cpp) ----
 *
 * Lets netplay code render SNAPSHOT sprite data through the CDDA tileset
 * without a loaded world. Opaque handle: nullptr = unavailable/failure. */
void *tiles_backend_create();
void tiles_backend_destroy( void *handle );
/// Start one frame batch: clip rect (dest + pixel size), pivot in bubble
/// coords (window center). Returns false when the frame must be skipped.
bool tiles_backend_begin( void *handle, int dest_x, int dest_y, int px_w,
                          int px_h, int pivot_bub_x, int pivot_bub_y );
/// Draw one sprite layer. kind: 0=terrain, 1=furniture, 2=item, 3=monster,
/// 4=vehicle part, 5=trap, 6=character sprite (player_male/npc_*; category
/// NONE). dx/dy are pivot-relative cell offsets; lit is lit_level as int;
/// sub is an optional tileset subcategory ("" = none); subtile/rota are the
/// multi-tile variant values (MULTITILE_TYPE / rotation 0..3, or 0/0).
/// mp_fixed: the sprite keeps its lit_level's look whatever the scene light
/// is (highlight badges etc., like the host's draw_light::fixed draws).
void tiles_backend_draw( void *handle, int kind, const char *id, const char *sub,
                         int dx, int dy, int lit, int subtile, int rota,
                         bool mp_fixed = false );
// DDA MP: multiply-blend a per-cell light overlay that replicates the host's
// lightmap gradient (the client has no lightmap). DARK/LOW/BRIGHT_ONLY and
// MEMORIZED cells get a dimming diamond (iso) or rect; LIT+ stays untouched.
// Must be called after the cell's scene sprites, before the bright badges.
// Only a fallback: skipped entirely when enable_lighting took over.
void tiles_backend_cell_light( void *handle, int dx, int dy, int light );
/// DDA MP: stream one cell's smooth-lighting light texel color (the same
/// values the host's own shader consumes; lr = light 0..255, lg/lb = hue
/// shares) plus the barrier bit; the lit_level byte fixes which cells are
/// seen in detail. Call for every cell with valid light bytes BEFORE
/// enable_lighting.
void tiles_backend_set_cell_light( void *handle, int dx, int dy, int light,
                                   uint8_t lr, uint8_t lg, uint8_t lb,
                                   bool barrier );
/// DDA MP: inject the streamed light texels into the shared smooth-lighting
/// light map, compute the filter's reach masks from the streamed barrier
/// bits and bind the lit frame exactly like the game's own renderer, so the
/// replay sprites take the real GPU lit path (gray/tone/colored light/
/// memory look identical to the host). snap_w/h are the snapshot cell
/// dimensions. False (no streamed light) leaves the classic fallback path
/// active.
bool tiles_backend_enable_lighting( void *handle, int snap_w, int snap_h );
/// Black out the entire display buffer so stale UI remnants (debug prompt
/// text etc.) never persist behind the replay canvas. Call BEFORE the ui
/// window redraw of the same iteration.
void tiles_backend_clear_screen( void *handle );
/// Per-tile pixel size of the loaded tileset; {0,0} on failure.
void tiles_backend_tile_size( void *handle, int &w, int &h );
/// Terminal font cell size in pixels (for aligning the sprite canvas to
/// the curses window interior); {0,0} on failure.
void tiles_backend_font_size( int &w, int &h );
void tiles_backend_end( void *handle );
#endif // TILES

} // namespace netplay
