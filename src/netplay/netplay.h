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
    SNAP_REQ = 10,  ///< client -> host: "send me the current picture now"
    /// DDA MP v4.0: per-player avatars. All av frames travel to/from a
    /// single session; the host tracks an avatar per connected sid.
    AVATAR_LIST_REQ = 20,   ///< client -> host: "which avatars exist" (empty)
    AVATAR_LIST = 21,       ///< host -> client: [n][str name + u8 alive]*
    AVATAR_RESUME = 22,     ///< client -> host: bind me to an existing avatar
    AVATAR_PROFILE = 23,    ///< client -> host: a freshly created avatar (json)
    AVATAR_OK = 24,         ///< host -> client: bind/creation accepted
    AVATAR_DENIED = 25,     ///< host -> client: bind/creation rejected (text)
};

/// DDA MP v3.9.1: one item-layer sprite the host's tileset draws on a cell
/// (tileset "layering.json" item_variants: a furniture/terrain context makes
/// the renderer draw its own sprite, e.g. the sink dishes, at a pixel
/// offset, instead of the plain item sprite).
struct tile_item_layer {
    std::string sprite;   ///< tileset sprite id the host rolled
    std::string variant;  ///< variant string the layer draw received
    int8_t dx = 0;        ///< pixel offset the tileset applies
    int8_t dy = 0;
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
    /// DDA MP v3.9.1 item appearance: the itype variant the host renderer
    /// draws items with (draw_item passes itype_variant().id; tilesets
    /// resolve e.g. "mug" -> "mug_var_<blue>"). Empty = no variant.
    std::string item_variant;
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
    ///        smooth lighting and sampled its light map for this cell),
    /// bit5 = the cell is seen in detail per the light texel's own texel
    ///        detail bit (same renderer state the color bytes came from;
    ///        deriving visibility from the lit_level byte races the
    ///        renderer's light map turn and paints lit path blue over
    ///        empty texels),
    /// bit6 = the tileset item layer consumed the uppermost item; the
    ///        plain item draw is skipped for this cell (drawtop=false)
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
    /// DDA MP v3.8 critter appearance: facing flip of the creature's sprites
    /// (0 = right, -1 = left, -2 = no facing, draw the static sprite) and
    /// the overlay draw ids in the host renderer's draw order, ';' separated
    /// (clothing overlays, wielded items, effect overlays...). Both are the
    /// exact facts cata_tiles::draw_entity_with_overlays resolves.
    int8_t critter_rot = -2;
    std::string critter_overlays;
    /// DDA MP v3.8.1 attitude badge the host renderer paints bottom-right:
    /// 0 = none, else (Creature::Attitude << 1) | sees_player_bit. The
    /// sees_player bit already folds the host viewer's INATTENTIVE check.
    /// Drawn as a fixed-lit sprite ("overlay_<attitude>[_sees_player]").
    uint8_t critter_badge = 0;
    /// DDA MP v4.0a-2: avatar's own heard-sound marker on this cell
    /// (sounds::process_avatar_markers on the host, drawn as overlay on the
    /// client exactly like cata_tiles::draw_footsteps_frame): 0 = none,
    /// 1 = "footstep" ("?"), 2 = "footstep_above" ("^"), 3 =
    /// "footstep_below" ("v"). Fixed-lit sprite, drawn last.
    uint8_t marker = 0;
    /// DDA MP v3.9.1 item-layer draws for this cell (see tile_item_layer).
    /// Empty = no tileset layer sprites on this cell.
    std::vector<tile_item_layer> item_layers;
    /// DDA MP v3.9.1: the tileset layer consumed the uppermost item, so the
    /// host skips the plain item draw (draw_field_or_item's drawtop=false).
    /// Streamed as flags bit 6.
    bool item_skip_top = false;
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

/// DDA MP v4.0: one netplay-controlled avatar in the host's world, as the
/// host reports it to a connecting client. `alive` = the avatar's npc is
/// still present and not dead in the host's save.
struct avatar_entry {
    std::string name;
    bool alive = false;        ///< npc exists in the host world and !dead
    bool in_session = false;   ///< currently driven by a live client session
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

/// Pop the pending snapshot request flag (a client asked for the current
/// picture). Reader threads set it; the host main thread consumes it.
bool host_pop_snapshot_request();

/* ---- DDA MP v4.0 avatar protocol (game glue in netplay_world.cpp) ---- */

/// One queued avatar operation from a session's reader thread; consumed on
/// the host main thread (whatever the host is doing — the UI mirrors and
/// dialogs all pump this).
struct avatar_op {
    int sid = 0;    ///< the requesting session
    int kind = 0;   ///< 1 = list request, 2 = resume(nick), 3 = profile(nick)
    std::string nick;
    std::string json; ///< avatar::serialize() profile (kind 3 only)
};

/// Pop one queued avatar op. False = nothing queued.
bool host_pop_avatar_op( avatar_op &out );

/// Send an encoded avatar frame to exactly one session (thread-safe).
void host_send_avatar_reply( int sid, frame op,
                             const std::vector<char> &payload );

/// Encode an AVATAR_LIST payload ([u16 n][str name + u8 alive + u8 busy]*).
std::vector<char> encode_avatar_list( const std::vector<avatar_entry> &list );
/// Text payload for AVATAR_DENIED (utf-8 reason); AVATAR_OK uses "{}".
std::vector<char> encode_avatar_text( const std::string &text );

/// Pump queued avatar ops on the host main thread; called from the game
/// loop (next to the snapshot request pump).
void host_pump_avatar_ops();

/// Pump the per-session keyboard: keys of a session bound to an avatar
/// step that avatar in the host world, others fall back to the legacy
/// watcher queue. Called from the game loop next to the avatar ops pump.
void host_pump_avatar_keys();

/* ---- DDA MP v4.0a-2: per-session input routing ---- */

/// One movement keystroke from a client session, alongside the session id
/// so the host glue can route it to that session's avatar (not to the host
/// player like the legacy keyboard queue does for watchers).
struct sid_key {
    int sid = -1;
    uint32_t code = 0;
};

/// Pop one queued per-session key event (thread-safe). False when empty.
bool host_pop_sid_key( sid_key &out );
/* DDA MP v4.0a-2: return an inspected-but-not-executed key (its session's
 * move pool was exhausted): the action waits for the next world turn
 * instead of being eaten by the pump. */
void host_put_back_sid_key( const sid_key &in );
/* DDA MP v4.0a-2: true while the client key queue still holds steps. */
bool host_avatar_keys_pending();

/// Re-queue a key onto the legacy watcher queue (the sid it came from has
/// no bound avatar, so it drives the host player like in v3.x).
void host_push_legacy_key( uint32_t code );

/// Session ids currently connected (copy, thread-safe).
std::vector<int> host_session_ids();

/// Send a screen frame to exactly one session (like the broadcast, minus
/// the fan-out). False when the session vanished meanwhile.
bool host_send_snapshot_to_sid( int sid, const snapshot &snap );

/* ---- client avatar bindings ---- */

/// Ask the host for the avatar owned by `nick` (must be connected). Also
/// re-arms the client's bind state to "waiting" so the join UI can poll.
void client_send_avatar_list_req( const std::string &nick );
/// Request to be bound to an existing (alive) avatar owned by `nick`.
void client_send_avatar_resume( const std::string &nick );
/// Offer a freshly created avatar profile; the host spawns it near its own
/// avatar and binds it to `nick`.
void client_send_avatar_profile( const std::string &nick,
                                 const std::string &profile_json );
/// Client-side avatar flow status, polled by the join UI:
/// 0 = idle (no exchange in progress), 1 = waiting for the host,
/// 2 = bound / accepted, 3 = denied (see client_avatar_error()).
int client_avatar_bind_state();
/// Copy of the last received avatar list (valid when bind state != 0).
std::vector<avatar_entry> client_avatar_list();
/// True once the host answered the list request (the list itself may be
/// empty — a fresh world simply has no avatars yet).
bool client_avatar_list_ready();
/// Last denial reason from the host (bind state 3).
std::string client_avatar_error();

/* ---- host game glue (netplay_world.cpp; needs game internals) ---- */

/// Game-side per-turn hook: builds and broadcasts an ascii snapshot of the
/// host's current view (~45x25 cells around the host avatar). Call at the
/// end of world turns; no-op when not hosting or no sessions.
void host_game_tick();
/// Service one pending client snapshot request by building and broadcasting
/// the current picture (host_game_tick). Call from a main-thread loop that
/// also runs while the host is idle; no-op without a pending request.
void host_pump_snapshot_requests();
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
/// Ask the host to send its current picture right away. The host streams
/// snapshots only on its own turn, so a freshly-loaded client needs this to
/// see anything before somebody presses a key. Safe to call often.
void client_request_snapshot();
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
                         bool mp_fixed = false, const char *mp_item_variant = "",
                         int off_x = 0, int off_y = 0 );
// DDA MP: multiply-blend a per-cell light overlay that replicates the host's
// lightmap gradient (the client has no lightmap). DARK/LOW/BRIGHT_ONLY and
// MEMORIZED cells get a dimming diamond (iso) or rect; LIT+ stays untouched.
// Must be called after the cell's scene sprites, before the bright badges.
// Only a fallback: skipped entirely when enable_lighting took over.
void tiles_backend_cell_light( void *handle, int dx, int dy, int light );
/// DDA MP: stream one cell's smooth-lighting light texel color (the same
/// values the host's own shader consumes; lr = light 0..255, lg/lb = hue
/// shares) plus the barrier and texel-detail bits (both from the same
/// renderer state the color bytes came from). Call for every cell with
/// valid light bytes BEFORE enable_lighting.
void tiles_backend_set_cell_light( void *handle, int dx, int dy, int light,
                                   uint8_t lr, uint8_t lg, uint8_t lb,
                                   bool barrier, bool detail );
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
