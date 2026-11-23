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
#include <cstdio>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

#include "coords_fwd.h"
#include "type_id.h"

class Character;
class Creature;
class npc;

namespace netplay
{

/// Distance-agnostic socket handle (sockfd on POSIX; SOCKET on Windows).
using socket_t = std::int64_t;

/// Default TCP port for DDA NetPlay.
constexpr int DEFAULT_PORT = 24224;

/// DDA MP built-in diagnostics.
/// Default OFF. The host menu can switch them on for a session; logs then land
/// in %TEMP%\opencode\ddadiag — never inside the game folder. The env variable
/// overrides the switch: DDA_MP_DIAG=<dir> forces them on and reroutes the
/// folder, DDA_MP_DIAG=0|off forces them off. Returns "" when disabled.
std::string mp_diag_dir();

/// True when diagnostics are enabled in this process.
bool mp_diag_enabled();

/// Host menu switch: write diagnostics to the default folder. Has no effect
/// when DDA_MP_DIAG is set.
void set_diag_enabled( bool enabled );
bool diag_enabled_by_user();

/// Opens `name` (e.g. "netplay_tiles.log") inside the diag folder for
/// appending; nullptr when diagnostics are disabled.
std::FILE *mp_diag_open( const char *name );

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
    /// DDA MP v4.0a-8: serialized character sheet of the session's avatar
    /// (plain text lines), refreshed by the host once per world turn.
    CHARACTER_SHEET = 26,   ///< host -> client: avatar sheet text
    /// DDA MP v4.0a-9: the host's real sidebar rendered for the session's
    /// avatar (colour cells), so the client sees the same panels.
    SIDEBAR = 27,           ///< host -> client: rendered sidebar cells
    CLIENT_VIEW = 28,       ///< client -> host: terminal size (u16 cols, u16 rows)
    /// DDA MP v4.0a-11: one short event line for the client's message log.
    EVENT_TEXT = 29,        ///< host -> client: str text
    /// DDA MP v4.0a-12: overmap minimap block around the session avatar.
    MINIMAP = 30,           ///< host -> client: [w][h][u32 color]*
    /// DDA MP v4.0a-14: inventory listing of the session avatar (text).
    INVENTORY = 31,         ///< host -> client: str text
    /// DDA MP v4.0a-18: an inventory action picked on the client (raw index
    /// into the last INVENTORY row list + action key, e.g. 'w'/'W'/'T').
    INVENTORY_ACTION = 32,  ///< client -> host: i32 node, i32 action
    /// DDA MP v4.0a-27: serialized session NPC for the client's own @ menu.
    CHARACTER_JSON = 33,    ///< host -> client: str json
    /// DDA MP v4.0a-31: pocket settings edited on the client for one row.
    INVENTORY_SETTINGS = 34, ///< client -> host: i32 node, i32 gen, str json
    /// DDA MP v4.0a-43: host-authored aiming panel for a client target tile.
    AIM_INFO_REQ = 35,      ///< client -> host: i32 node, gen, dx, dy
    AIM_INFO = 36,          ///< host -> client: str lines ('\n' separated)
    AIM_PANEL = 37,         ///< host -> client: rendered w_target cells
    /// DDA MP: local-space creature facts for the client aim panel.
    AIM_TARGETS = 38,       ///< host -> client: [n]{i32 dx,dy,...}
    /// DDA MP v4.0a-72: one action id picked in the client's own action menu.
    CLIENT_ACTION = 39,     ///< client -> host: i32 action id
    /// DDA MP v4.0a-88: the client is away; exclude it from turn parity.
    CLIENT_AFK = 40,        ///< client -> host: u8 on/off
    /// DDA MP v4.0a-90: ground items around the avatar for the client's own
    /// pickup selector (inv packet text with dx/dy/can_take fields).
    PICKUP_DATA = 41,       ///< host -> client: str text
    /// DDA MP v4.0a-90: rows chosen in that selector.
    PICKUP_ACTION = 42,     ///< client -> host: i32 gen, u16 n, i32 nodes[n]
    /// DDA MP v4.0a-97: 'g' with items on several adjacent tiles asks which
    /// tile first (the host's own choose_adjacent_highlight does the same).
    PICKUP_TILES = 43,      ///< host -> client: str text (gen + dx/dy rows)
    PICKUP_TILE = 44,       ///< client -> host: i32 gen, i32 dx, i32 dy
    /// DDA MP v4.0a-147: the host's panel_manager layout (json), so the client
    /// draws the same native sidebar and its log starts at the same row.
    PANEL_LAYOUT = 45,      ///< host -> client: str json
    /// DDA MP v4.0a-243: seen overmap tiles around the avatar (stock map UI
    /// and any overmap-based sidebar widget read these).
    OVERMAP = 46,           ///< host -> client: sparse overmap block
    /// DDA MP v4.0a-247: generic prompt bridge. A vanilla host action running
    /// for a session avatar sends its UI request here; the client shows the
    /// same vanilla widget and returns the choice.
    PROMPT_REQ = 47,        ///< host -> client: prompt request
    PROMPT_RES = 48,        ///< client -> host: prompt answer
    /// DDA MP v4.1: a single player intent — what the client wants done.
    /// Payload: [str action][i32 generation][u16 n][i32 args[n]][str data].
    /// The host runs it with the session avatar's own engine code; the old
    /// per-menu frames above are being folded into this channel.
    INTENT = 50,            ///< client -> host
};

/// One client intent: an action id plus action-specific integer arguments.
struct intent_request {
    int sid = 0;
    std::string action;
    int generation = 0;
    std::vector<int> args;
    std::string data;
};

/// One minimap block: overmap tiles around the avatar, colours precomputed by
/// the host's overmap display code.
struct minimap_frame {
    int width = 0;
    int height = 0;
    /// DDA MP v4.0a-229: the avatar's index inside the block. The block is
    /// clamped to the loaded map, so the player is not always at width/2.
    int center_x = -1;
    int center_y = -1;
    std::vector<uint32_t> color;
    /// 1 when the session avatar can see the tile, 0 otherwise (drawn black).
    std::vector<uint8_t> seen;
    /// Critter beacons (host's pixel-minimap diamonds): position relative to
    /// the block centre and a base colour index.
    struct beacon {
        int dx = 0;
        int dy = 0;
        uint32_t color = 0;
        std::string name;
        std::string sym; // display symbol ("@" for characters, monster glyph)
        /// DDA MP: raw creature facts for the client's aiming panel.
        uint8_t is_character = 0;
        int32_t attitude = 0;      // Creature::Attitude
        int32_t npc_attitude = 0;  // npc_attitude
        uint8_t sees_you = 0;
        int32_t hp_pct = 0;
        uint8_t name_color = 8;    // base-16 palette index
        std::string wielded;       // tname with colour tags
    };
    std::vector<beacon> beacons;
};

/// DDA MP v4.0a-243: sparse seen-overmap block around the session avatar.
/// Only tiles the host player has actually seen are listed; everything else
/// stays unseen on the client, exactly like the host's own map screen.
struct overmap_block {
    /// Block bounds in overmap tiles; origin is the top-left corner.
    int width = 0;
    int height = 0;
    int origin_x = 0;   // absolute OMT
    int origin_y = 0;
    int origin_z = 0;
    /// Terrain ids, deduplicated once per block.
    std::vector<std::string> ter_table;
    struct cell {
        uint16_t index = 0;   // y * width + x
        uint16_t ter = 0;     // index into ter_table
        uint8_t vision = 0;   // om_vision_level
        uint8_t vehicle = 0;  // 1 when the host tracks a vehicle here
    };
    std::vector<cell> cells;
    struct note {
        uint16_t index = 0;
        std::string text;
    };
    std::vector<note> notes;
};

/// DDA MP v4.0a-247: one remote UI prompt.
enum class prompt_type : uint8_t {
    query_yn = 0,  ///< text only; answer.retval = 1 yes / 0 no
    uilist = 1     ///< title + entries; answer.retval/index
};
struct prompt_entry {
    std::string txt;
    int retval = 0;
    bool enabled = true;
    int key = -1;      // first input of the entry's hotkey, -1 = none
};
struct prompt_request {
    uint16_t id = 0;
    prompt_type type = prompt_type::query_yn;
    std::string text;
    int selected = 0;
    std::vector<prompt_entry> entries;
};
struct prompt_answer {
    uint16_t id = 0;
    bool canceled = true;
    int retval = 0;
    int index = 0;
};

/// DDA MP: one creature in the session avatar's local frame, raw facts for the
/// aiming panel (dx/dy are relative to the avatar, not overmap tiles).
struct aim_target {
    int dx = 0;
    int dy = 0;
    uint8_t is_character = 0;
    int32_t attitude = 0;
    int32_t npc_attitude = 0;
    uint8_t sees_you = 0;
    int32_t hp_pct = 0;
    /// DDA MP v4.0a-75: target's dodge, so the client's throw dispersion
    /// includes it exactly like the host's own aiming panel does.
    int32_t dodge = 0;
    uint8_t name_color = 8;
    std::string name;
    std::string wielded;
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
    ///        plain item draw is skipped for this cell (drawtop=false),
    /// bit7 = the vpart fields came from the viewer's map memory, not from a
    ///        live vehicle; the native client must not queue a vpart override
    ///        (the renderer draws it dim through map memory instead)
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
    /// DDA MP v4.0a-114: raw field state (fire, smoke, blood, gas...) so the
    /// native renderer draws the same field sprite and intensity as the host.
    /// Empty field id = no field on the cell.
    std::string field;
    uint8_t field_intensity = 0;
    /// DDA MP v4.0a-114: the viewer's map memory for this cell. The client
    /// rebuilds it in its own avatar's map memory, so remembered but unseen
    /// tiles render the same memory sprites as on the host.
    std::string mem_ter;
    std::string mem_dec;
    uint8_t mem_ter_sub = 0;
    uint8_t mem_ter_rot = 0;
    uint8_t mem_dec_sub = 0;
    uint8_t mem_dec_rot = 0;
    uint32_t mem_sym = 0;
    /// DDA MP v4.0a-135: graffiti sprayed on this tile (the native renderer
    /// draws it as the stock draw_graffiti pass does).
    std::string graffiti;
};

/// DDA MP v4.0a-9: one rendered sidebar cell — the text as the curses layer
/// stores it (UTF-8, may be a multi-byte glyph) plus its colour.
struct sidebar_cell {
    std::string text;
    int color = 0;
    int bg = 0;
};

/// A rendered sidebar block (the host's panel layout, drawn for one avatar).
struct sidebar_frame {
    int width = 0;
    int height = 0;
    std::vector<sidebar_cell> cells;
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
    /// DDA MP v4.0a-4: absolute world coordinates of the frame's pivot cell
    /// (the host's map.get_abs(center)). Random-weighted sprite variants seed
    /// off tile position; the client's frame cells are camera-relative, so a
    /// streamed origin keeps the variant stable per world tile (the flickering
    /// ivan-chay flower). false = legacy host (camera-relative seeds).
    bool has_origin = false;
    int origin_x = 0;
    int origin_y = 0;
    int origin_z = 0;
    /// DDA MP v4.0a-232: offset (tiles) of the frame centre from the viewer
    /// avatar. 0,0 = the frame is centred on the avatar. The client's
    /// look/fire cursor scrolls the view, and it needs this to place its
    /// mirror avatar and the reticle exactly.
    int view_dx = 0;
    int view_dy = 0;
    /// DDA MP v4.2: host world clock and weather, so the client's native
    /// lighting/panels read the same time as the host.
    bool has_time = false;
    int turn = 0;
    std::string weather;
    /// FacingDirection of the session avatar (the frame's viewer), streamed
    /// because Character::store/load does not carry `facing`.
    uint8_t avatar_facing = 2;
    /// DDA MP v4.2: raw creature state for the native renderer. kind 0 =
    /// monster, 1 = npc; the JSON is the game's own serialization, position is
    /// absolute so the client can place it without a shared map address.
    struct creature_frame {
        uint8_t kind = 0;
        int ax = 0;
        int ay = 0;
        int az = 0;
        /// FacingDirection (NONE=0, LEFT=1, RIGHT=2). Character::store does not
        /// save it, so without this the mirror always looked right.
        uint8_t facing = 2;
        std::string json;
    };
    std::vector<creature_frame> creatures;
    /// DDA MP v4.0a-239: real item descriptors for the frame's tiles. The
    /// native renderer then draws items, container/item layers, the pile badge
    /// and the zombie revival "!" itself, with the tile's own light - no client
    /// side approximations.
    struct item_desc {
        std::string type;
        std::string variant;
        std::string mtype;   // corpse monster type
        int count = 1;
        int damage = 0;      // raw damage points, like item::damage()
        int seed = 0;        // item::seed (stable layering roll)
    };
    struct item_tile {
        int index = 0;       // cell index in the frame: y * width + x
        std::vector<item_desc> items;
    };
    std::vector<item_tile> item_tiles;
    /// DDA MP v4.0a-248: vehicle parts in the avatar's visible range, not just
    /// inside the streamed frame. The mirror map has no vehicle objects, so the
    /// client draws these through the tileset's vpart override.
    struct vehicle_part_frame {
        int ax = 0;
        int ay = 0;
        int az = 0;
        std::string id;       // "vp_<part>" (no variant)
        std::string variant;
        uint8_t sub = 0;      // 1 = open, 2 = broken, 0 = closed
        uint8_t rot = 0;      // angle_to_dir4 value
        bool memory = false;  // memorized decoration, not a live vehicle
    };
    std::vector<vehicle_part_frame> vehicle_parts;
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

/// Start the host listener; DEFAULT_PORT unless overridden. Idempotent.
bool host_start( int port = DEFAULT_PORT );
/// Stop the host.  Idempotent.
void host_stop();
/// True while host is listening.
bool host_is_running();
/// Port the host is bound to (DEFAULT_PORT when off).
int host_listen_port();
/// Address string of the host, e.g. "192.168.1.10:24224".
std::string host_listen_address();
/// Number of currently connected sessions.
int host_session_count();
/// Display names of the currently connected sessions (copy, stable order).
std::vector<std::string> host_session_names();
/// Display name of one session (empty when the session is gone).
std::string host_session_name( int sid );
/// Per-turn maintenance (accept/expire, flush queues). Safe to call often.
void host_pump();

/**
 * DDA MP: heartbeat for silent sessions. Call it from the host's idle tick.
 * A session that gave no input and is not busy (sleep, activity, helping,
 * long wait) for 30 s gets a PING. If the PING is still unanswered 30 s later
 * the session id is returned: the caller should tell the players and call
 * host_drop_session(). `busy` is supplied by the world glue so the network
 * layer does not have to know about avatars.
 */
std::vector<int> host_heartbeat_tick( const std::function<bool( int )> &busy );
/// DDA MP: unblock a session's reader; it then runs the normal close path.
void host_drop_session( int sid );

/** DDA MP v4.0a-240: true when this npc is a netplay session avatar (a remote
 * player's body). Such a body is driven by the player's own key presses, so
 * NPC-only follower behaviour (closing doors behind itself, ally AI) must not
 * run for it; the stock player walk does none of that. */
bool is_session_avatar( const npc &who );

/// DDA MP v4.0a-226: close a session on purpose and send the client a reason.
/// The client shows the reason and returns to the main menu instead of
/// offering a reconnect. The reader thread still owns the final close.
void host_kick_session( int sid, const std::string &reason );

/// DDA MP v4.0a-226: true when the host kicked this client (read after the
/// client session loop ends).
bool client_kicked();
std::string client_kick_reason();

/// Broadcast a screen frame to every live session (thread-safe; session
/// sockets that fail to receive are dropped).
void host_broadcast_snapshot( const snapshot &snap );

/// Pop one queued client key code (0 = none arrived since last call).
/// Returns false when the queue is empty.
bool host_pop_key( uint32_t &code );
/// DDA MP v4.0a-7: send a short status line to one session (turn pacing:
/// "waiting for other players..."); empty text clears it on the client.
bool host_send_hover_to_sid( int sid, const std::string &text );
/// DDA MP v4.0a-8: send the session avatar's character sheet (plain text).
bool host_send_sheet_to_sid( int sid, const std::string &text );
/// DDA MP v4.0a-27: send the session avatar serialized as JSON.
bool host_send_character_json_to_sid( int sid, const std::string &json );
/// Latest serialized session avatar (client side).
bool client_character_json( std::string &out );
/// DDA MP v4.0a-9: send the host-rendered sidebar (colour cells) to a session.
bool host_send_sidebar_to_sid( int sid, const sidebar_frame &sb );
/// DDA MP v4.0a-147: send the host's panel_manager layout (json) to a session.
bool host_send_panel_layout_to_sid( int sid, const std::string &json );
/// DDA MP v4.0a-134: event type for "your avatar died"; the client logs it and
/// offers to return to the main menu. Values 0..255 fit the wire byte.
constexpr int EVENT_DEATH = 200;
/// DDA MP v4.0a-176: stepping into another player opens the interaction menu.
/// Text payload: "<name>\x1f<activity msgid, empty when idle>"; the client
/// translates the msgid with fork_text().
constexpr int EVENT_PLAYER = 201;
/// DDA MP v4.0a-177: authoritative "you are helping / no longer helping"
/// state for the helper's client. Text payload: "on" or "off". Keeps the
/// client's stop-confirmation flag in sync when the host ends the assistance
/// (target finished, walked away, or the request was refused).
constexpr int EVENT_HELP = 202;
/// DDA MP v4.0a-188: stepping into a common NPC opens an inspection menu on
/// the client. Text payload: "<name>\x1f<dx>\x1f<dy>".
constexpr int EVENT_NPC = 203;
/// DDA MP v4.0a-189: one dialogue step for the client. Payload:
/// "<line>\x1f<index>\x1e<option>\x1f<index>\x1e<option>..." (indices into the
/// host dialogue's response list). An empty option list closes the dialogue.
constexpr int EVENT_DIALOGUE = 204;
/// DDA MP v4.0a-191: NPC mission offers. Payload:
/// "<index>\x1e<mission name>\x1f<index>\x1e<mission name>...".
constexpr int EVENT_MISSION_OFFERS = 205;
/// DDA MP v4.0a-191: session mission journal. Payload:
/// "<name>\x1e<status>\x1e<description>\x1f...".
constexpr int EVENT_MISSION_JOURNAL = 206;
/// DDA MP v4.0a-192: missions ready to turn in at this NPC. Payload:
/// "<uid>\x1e<mission name>\x1f...".
constexpr int EVENT_MISSION_TURNIN = 207;
/// DDA MP v4.0a-193/198: custom barter list. Payload:
/// "<mode 0=buy/1=sell>\x1f<reward credit>\x1f<avatar cash>\x1f<trader cash>"
/// "\x1f<index>\x1e<name>\x1e<price cents>\x1e<count>\x1e<charge>\x1f...".
constexpr int EVENT_TRADE_LIST = 208;
/// DDA MP v4.0a-198: read-only vehicle status. Payload:
/// "<name>\x1e<velocity mph>\x1e<cruise mph>\x1e<engine on>\x1e<battery>"
/// "\x1e<battery capacity>\x1e<parts>\x1e<damaged>\x1f<fuel id>\x1e<charges>...".
constexpr int EVENT_VEHICLE_INFO = 209;
/// DDA MP v4.0a-204: damaged vehicle parts. Payload:
/// "<index>\x1e<vpart id>\x1e<damage level>\x1e<repairable>\x1f...".
constexpr int EVENT_VEHICLE_PARTS = 210;
/// DDA MP v4.0a-207: read-only zone list. Payload:
/// "<name>\x1e<zone type id>\x1e<x>\x1e<y>\x1e<z>\x1f...".
constexpr int EVENT_ZONES = 211;

/// DDA MP v4.0a-169: translate a line that exists only in the netplay fork and
/// therefore never made it into the game's translation catalogs. Falls back to
/// the untranslated string for languages the fork does not ship.
std::string fork_text( const char *msgid );

/// DDA MP v4.0a-174: co-op assistance. A player next to another player who is
/// busy with a long activity can opt in to help; the activity speeds up with
/// the helper's skill and both gain experience. Returns true when a helper was
/// added or removed.
bool host_help_nearby( Character &helper );
/// DDA MP v4.0a-178: host-side player interaction. When the host walks into a
/// session avatar, show the same co-op menu the client gets. Returns false for
/// any other creature (the caller then falls back to the vanilla NPC menu).
bool host_interact_player( npc &target );
/// DDA MP v4.0a-180: progress line shown on the host's screen while the host
/// itself is helping a client. Empty when the host is not helping.
const std::string &host_help_hint();
/// Draw that line on one of the host's top rows. Erases the row when the hint
/// is empty (called once after the assistance ends).
void host_draw_help_hint( int row );
/// DDA MP v4.0a-180: drop all assistance state when hosting stops, so a stale
/// progress line does not survive into single-player.
void host_help_shutdown();
/// True when `helper` is currently assisting `target`.
bool is_helping( const Character *helper, const Character *target );
/// True when `helper` is assisting anyone (used by the round gate: a player
/// who is busy helping has already given their order for this round).
bool is_helping_anyone( const Character *helper );
/// Speed multiplier for `target`'s activity: 1.0 without helpers, higher with
/// a skilled helper (capped).
float help_speed_factor( const Character &target, const skill_id &skill );

/// DDA MP v4.0a-170: when the engine is currently working inside a session's
/// split region, a global add_msg() belongs to that client, not to the host's
/// log. Routes it (with msgid capture, so it stays localized) and reports true.
bool host_route_bubble_message( const std::string &text, int msg_type );

/// DDA MP v4.0a-11: send one event line to the client's message log.
bool host_send_event_to_sid( int sid, const std::string &text, int msg_type = 0 );
/// DDA MP v4.0a-165: send a formatted event line as msgid + arguments. The
/// client formats it with ITS translation catalog, so a Russian client reads
/// the host's messages in Russian even when the host runs in English.
/// An argument may name a data object by type id instead of shipping the
/// host's already-localized text: prefix it with '\x02' and one of
///   'T' terrain id, 'F' furniture id, 'I' item type id, 'M' monster type id.
/// The client resolves those itself.
bool host_send_event_fmt_to_sid( int sid, const std::string &msgid,
                                 const std::vector<std::string> &args, int msg_type = 0 );
/// DDA MP v4.0a-166: a game message reaches the relay already formatted and in
/// the sender's language, because `_()` translates before add_msg runs. Every
/// `_()` call also notes the raw msgid; this checks whether `text` is an
/// instance of that msgid's translated format, recovers the arguments and
/// fills `msgid`/`instance` (the msgid with its arguments, untranslated) so the
/// client can re-create the line in its own language.
bool capture_msgid_for( const std::string &text, std::string &msgid, std::string &instance );
/// DDA MP v4.0a-126: relay a Character message to the client driving that
/// character (session avatar). Returns true when the character belongs to a
/// session and the text was forwarded instead of going to the host log.
/// v4.0a-166: when the message came from a translated template, `msgid` and
/// the msgid-language `instance` are sent as well, so the client formats the
/// line with its own catalog instead of showing the host's sentence.
bool host_relay_character_message( const Character *who, const std::string &text,
                                   int msg_type,
                                   const std::string &msgid = std::string(),
                                   const std::string &instance = std::string() );
/// DDA MP v4.0a-136: relay a third-person "player sees X" message to every
/// session whose avatar can see X (gunshots, NPC actions, world events).
void host_relay_sees_message( const tripoint_bub_ms &target, const std::string &text,
                              int msg_type,
                              const std::string &msgid = std::string(),
                              const std::string &instance = std::string() );
void host_relay_sees_message( const Creature &target, const std::string &text, int msg_type,
                              const std::string &msgid = std::string(),
                              const std::string &instance = std::string() );
/// DDA MP v4.0a-160: forward every message appended since `before_generation`
/// to `sid` and remove it from the host's log (a session avatar's action ran,
/// not the host player's).
void host_forward_new_messages( int sid, std::size_t before_generation );
/// DDA MP v4.0a-139 (multi-region foundation): preserve live monsters when the
/// reality bubble switches away from their area, and bring them back when it
/// returns. Without this, walking far despawns everything left behind.
void host_capture_region_monsters();
void host_restore_region_monsters();
/// DDA MP v4.0a-11: broadcast one event line to every connected session.
void host_broadcast_event( const std::string &text, int msg_type );
/// DDA MP v4.0a-12: send the overmap minimap block to a session.
bool host_send_minimap_to_sid( int sid, const minimap_frame &mm );
/// DDA MP v4.0a-243: send the seen-overmap block to a session.
bool host_send_overmap_to_sid( int sid, const overmap_block &om );
/// DDA MP v4.0a-247: the session whose action is currently executing on the
/// host (0 = none). Set around client action/intent execution and activities.
int host_prompt_owner();
/// Bridge a vanilla host prompt to the owner session. nullopt = no owner/no
/// answer (caller falls back to its normal local prompt).
std::optional<bool> host_prompt_query_yn( const std::string &text );
std::optional<prompt_answer> host_prompt_uilist( const std::string &text, int selected,
                                                 const std::vector<prompt_entry> &entries );
/// DDA MP: send local-frame creature facts for the aim panel.
bool host_send_aim_targets_to_sid( int sid, const std::vector<aim_target> &list );
/// Client side: latest aim target list.
bool client_aim_targets( std::vector<aim_target> &out );
/// DDA MP v4.0a-14: send the inventory listing to a session.
bool host_send_inventory_to_sid( int sid, const std::string &text );
/// DDA MP v4.0a-18: client sends one inventory action for the given row index.
/// `targets` lists source rows (insert); empty means "no second row".
bool client_send_inventory_action( int node, int action, int generation,
                                   const std::vector<int> &targets = {} );
/// Host main thread: pop one queued inventory action.
bool host_pop_inventory_action( int &sid, int &node, int &action, int &generation,
                                std::vector<int> &targets );
/// DDA MP v4.0a-72: client sends one action id chosen in its own action menu.
bool client_send_client_action( int action );
/// Host main thread: pop one queued client action id.
bool host_pop_client_action( int &sid, int &action );
/// DDA MP v4.0a-90: host sends the ground-item listing around the avatar.
bool host_send_pickup_to_sid( int sid, const std::string &text );
/// Client: latest pickup listing (consumed).
bool client_pickup_packet( std::string &out );
/// Client: chosen ground rows for the last listing.
bool client_send_pickup_action( int generation, const std::vector<int> &nodes, int equip = 0 );
/// Host main thread: pop one queued pickup selection.
bool host_pop_pickup_action( int &sid, int &generation, std::vector<int> &nodes, int &equip );
/// DDA MP v4.0a-97: which adjacent tiles can be picked from ('g' asks first
/// when there is more than one, exactly like the host's own highlight).
bool host_send_pickup_tiles_to_sid( int sid, const std::string &text );
/// Client: latest tile-choice request (consumed).
bool client_pickup_tiles( std::string &out );
/// Client: chosen adjacent tile (dx, dy) for the last request.
bool client_send_pickup_tile( int generation, int dx, int dy );
/// Host main thread: pop one queued tile choice.
bool host_pop_pickup_tile( int &sid, int &generation, int &dx, int &dy );
/// DDA MP v4.0a-31: client sends edited pocket settings (JSON array, per pocket).
bool client_send_inventory_settings( int node, int generation, const std::string &json );
/// Host main thread: pop one queued pocket-settings update.
bool host_pop_inventory_settings( int &sid, int &node, int &generation, std::string &json );
/// DDA MP v4.1: send one player intent. The host demultiplexes known action
/// ids into the same typed queues the old per-menu frames used, so every
/// existing host handler keeps working while the wire turns into one channel.
bool client_send_intent( const std::string &action, int generation,
                         const std::vector<int> &args,
                         const std::string &data = std::string() );
/// Host main thread: pop one unhandled intent (unknown action ids).
bool host_pop_intent( intent_request &out );

/// DDA MP v4.1: inventory action addressed by the item's stable mp_uid.
/// data = item uid, then US-separated target uids (container inserts).
bool client_send_inventory_uid( const std::string &uid, int action,
                                const std::vector<std::string> &targets = {},
                                const std::vector<int> &ints = {} );
/// DDA MP v4.1: pocket settings for the item with this uid.
bool client_send_inventory_settings_uid( const std::string &uid, const std::string &json );

/// One uid-addressed inventory action, queued for the host main thread.
struct inventory_uid_action {
    int sid = 0;
    int action = 0;
    /// [0] is the target item; the rest are insert targets (may be empty).
    std::vector<std::string> uids;
    std::string json;   ///< pocket settings payload for the 'v' action.
    std::vector<int> ints; ///< extra integer arguments ('=' invlet, 't' dx/dy).
};
bool host_pop_inventory_uid( inventory_uid_action &out );
/// DDA MP v4.0a-43: ask the host for the aiming panel of one target tile.
bool client_send_aim_info( int node, int generation, int dx, int dy );
/// Host main thread: pop one aiming-panel request.
bool host_pop_aim_request( int &sid, int &node, int &generation, int &dx, int &dy );
/// Host -> client aiming panel text.
bool host_send_aim_info_to_sid( int sid, const std::string &text );
/// Client side: latest aiming panel text.
bool client_aim_info( std::string &out );
/// Host -> client rendered aiming window cells.
bool host_send_aim_panel_to_sid( int sid, const sidebar_frame &sb );
/// Client side: latest rendered aiming window.
bool client_aim_panel( sidebar_frame &out );
/// Pop the pending snapshot request flag (a client asked for the current
/// picture). Reader threads set it; the host main thread consumes it.
bool host_pop_snapshot_request();
/// The view offset (tiles, relative to the session avatar) requested by that
/// client with its last SNAP_REQ. The host builds the streamed frame around
/// avatar + offset, so the client's look/fire cursor can scroll the camera.
point host_snapshot_view( int sid );
/// DDA MP v4.0a-248: vertical look offset requested by the session.
int host_snapshot_view_z( int sid );

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

/// Pump queued inventory actions for session avatars (main thread).
void host_pump_inventory_actions();

/// DDA MP v4.0a-72: pump queued client action-menu choices (main thread).
void host_pump_client_actions();

/// DDA MP v4.1: pump intents that no typed handler claimed. The known ids are
/// demultiplexed by the reader thread into the pumps above; this one drains
/// and reports anything else so a client never silently loses an order.
void host_pump_intents();

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
/// DDA MP v4.0a-7: end-of-world-turn hook — starts a new turn round for the
/// sessions (clears their "acted" marks, lifts waiting hints). No-op when not
/// hosting.
void host_note_world_turn();

/// DDA MP v4.0a-216: a session avatar died; inform its client immediately so
/// the death screen and character creation do not depend on a world turn.
void host_notify_avatar_death( int npc_id );
/// DDA MP v4.0a-7: true while the host must wait for clients to give their
/// orders (strict turn parity, with a short grace for idle clients).
bool host_turn_blocked();
/// DDA MP v4.0a-155: true when the host and every active session have spent
/// their action pool for the current round. The world turn waits until then,
/// but host input is never withheld.
bool host_round_done();
/// Mark that the host player gave input in the current round.
void host_mark_input_done();
/// Whether the host player already gave input in the current round.
bool host_input_done();
/// True when at least one session is bound to an avatar NPC.
bool host_any_avatar_bound();
/// True when at least one bound session can still give orders (not AFK,
/// asleep or inside a long activity). The host's long wait only auto-passes
/// while such a client exists, so the world never free-runs when everybody
/// is excluded.
bool host_any_active_client();
/// DDA MP v4.0a-144: a client finished its order while the host idled. The
/// host turn loop consumes this as an automatic wait, so time advances even
/// when the host player does not press a key. False while the round is still
/// waiting for another client.
bool host_consume_auto_turn();
/// Clear the pending auto-wait at the start of a fresh round.
void host_clear_auto_turn();
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
/// DDA MP v4.0a-7: latest status line from the host (returned once).
bool client_take_hover( std::string &out );
/// DDA MP v4.0a-8: latest character sheet text of this session's avatar
/// (copied on every call; false until the host sent one).
bool client_sheet( std::string &out );
/// DDA MP v4.0a-9: latest rendered sidebar from the host (copied per call).
bool client_sidebar( sidebar_frame &out );
/// DDA MP v4.0a-147: latest host panel layout json (copied per call).
bool client_panel_layout( std::string &out );
/// DDA MP v4.0a-11: pop one event line sent by the host (false when empty).
bool client_take_event( std::string &out, int &msg_type );
/// DDA MP v4.0a-165: turn an event payload into a line in the local language
/// (msgid translated with this client's catalog, typed name arguments
/// resolved from local game data). Plain text passes through unchanged.
std::string format_event_text( const std::string &payload );
/// DDA MP v4.0a-12: latest overmap minimap block (copied per call).
bool client_minimap( minimap_frame &out );
/// DDA MP v4.0a-243: take the latest seen-overmap block (consumed once).
bool client_take_overmap( overmap_block &out );
/// DDA MP v4.0a-247: take a pending host prompt (consumed once).
bool client_take_prompt( prompt_request &out );
/// Answer a host prompt.
bool client_send_prompt_answer( const prompt_answer &ans );
/// DDA MP v4.0a-14: latest inventory listing (copied per call).
bool client_inventory( std::string &out );
/// Show a short status hint on the client's screen (empty clears it).
void client_status_hint( const std::string &text );
/// DDA MP v4.0a-10: terminal cells that actually fit the drawable window.
void drawable_cells( int &cols, int &rows );
/// DDA MP v4.0a-15: ImGui overlay pass above the tiles canvas (pair them).
void overlay_begin();
void overlay_end();
/// Ask the host to send its current picture right away. The host streams
/// snapshots only on its own turn, so a freshly-loaded client needs this to
/// see anything before somebody presses a key. Safe to call often.
void client_request_snapshot( int view_dx = 0, int view_dy = 0, int view_dz = 0 );
/// DDA MP v4.0a-94 test hook: post-connect join flow without the dialog.
void client_bootstrap_session( const std::string &nick );
/// Send an input batch upstream.
void client_send_keys( const std::vector<key_event> &keys );
/// DDA MP v4.0a-9: tell the host this client's terminal size (cols x rows) so
/// the sidebar is rendered to fit instead of being clipped by the client.
void client_send_view( int cols, int rows );
/// Host: last reported terminal size of a session (false if none yet).
bool host_client_view( int sid, int &cols, int &rows );
/// Per-turn maintenance. Safe to call often.
void client_pump();

/* ---- DDA MP v4.0a-86: long wait mode ---- */

/// While long wait is on, the local player passes turns automatically so the
/// other players can move freely. Host and client keep their own flag; the
/// host toggles it from the main menu, the client from its session menu.
void set_long_wait( bool on );
bool long_wait_active();
/// Host: true when a session avatar acted (step/wait) since the last world
/// turn. Drives the host's auto-turn while long wait is on.
/// Client: tell the host this player is away (excluded from turn parity).
bool client_send_afk( bool on );
/// Host: whether a session is in AFK mode.
bool host_session_afk( int sid );
/// Host: draw the long-wait line in the top-of-screen waiting hint block.
void host_draw_long_wait_hint( int row );

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
/// DDA MP v4.0a-122: the host world's active mod ids, announced in WELCOME.
/// The client loads the same mod data even when it has no local copy of the
/// world, so joining needs no save transfer.
std::vector<std::string> client_host_mods();
/// DDA MP v4.0a-123: the host world's options (world_default page), so the
/// virtual client world matches the host's season length and similar rules.
std::vector<std::pair<std::string, std::string>> client_host_world_options();

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
/// DDA MP v4.0a-4: absolute world coordinates of the current snapshot's
/// pivot cell (from the streamed origin). Call before tiles_backend_begin
/// for each frame; has=false keeps the legacy camera-relative seeds.
void tiles_backend_set_origin( void *handle, int ax, int ay, int az, bool has );
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
/// DDA MP v4.2: draw the whole map through the game's own cata_tiles::draw()
/// on the backend's loaded tileset instance (the headless client's global
/// tilecontext may have stale atlas generations). Call inside a begin/end pair.
bool tiles_backend_draw_native( void *handle, int dest_x, int dest_y,
                                int cx, int cy, int cz, int px_w, int px_h );
/// Diagnostics: write the display buffer to netplay_dump_<role>_<tag>.png and
/// log the result to netplay_tiles.log.
void dump_display_buffer( const std::string &tag );
#endif // TILES

} // namespace netplay
