// CataDDA Multiplayer ("DDA MP") — transport layer.
// Windows-first: WinSock2 TCP; length-prefixed frames.
// Public API in "netplay/netplay.h".

#include "netplay.h"
#include "netplay_region.h"
#ifdef _WIN32
#include "worldfactory.h" // world_generator (WELCOME world name)
#include "mod_manager.h"  // mod versions for the data manifest
#include "options.h"      // TILES for the data manifest
#include "version.h"      // VERSION for the data manifest
#endif

/// DDA MP v4.0a-165: the client resolves typed event arguments (terrain,
/// furniture, item type, monster type) from its own data on receipt.
#include "item.h"
#include "mission.h"
#include "item_factory.h"
#include "mapdata.h"
#include "monstergenerator.h"
#include "mtype.h"
#include "netplay/netplay_msgcap.h"
#include "path_info.h"
#include "translation_manager.h"
#include "translations.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#ifdef _WIN32
#include <windows.h> // WideCharToMultiByte (crash diagnostics)
#include <dbghelp.h> // MiniDumpWriteDump (crash diagnostics)
#include <psapi.h>   // PROCESS_MEMORY_COUNTERS (host memory trace)
#endif

#include "zstd/zstd.h"

#ifdef _WIN32
#   ifndef NOMINMAX
#       define NOMINMAX
#   endif
#   include <winsock2.h>
#   include <ws2tcpip.h>
#else
#   include <arpa/inet.h>
#   include <netinet/in.h>
#   include <sys/socket.h>
#   include <unistd.h>
#endif

namespace netplay
{

static_assert( sizeof( socket_t ) == 8, "socket_t width" );

/// DDA MP v4.0a-326: bumped with the data-manifest handshake.
static constexpr const char *NETPLAY_VERSION = "4.0a-334";

std::string data_manifest_hash()
{
    std::string manifest = VERSION;
    manifest += '|';
    manifest += NETPLAY_VERSION;
    if( world_generator != nullptr && world_generator->active_world != nullptr ) {
        for( const mod_id &m : world_generator->active_world->active_mod_order ) {
            manifest += '|';
            manifest += m.str();
            manifest += ':';
            manifest += m->version;
        }
    }
    uint64_t h = 1469598103934665603ULL;
    for( const unsigned char c : manifest ) {
        h ^= c;
        h *= 1099511628211ULL;
    }
    char out[17];
    std::snprintf( out, sizeof( out ), "%016llx", static_cast<unsigned long long>( h ) );
    return std::string( out );
}

/* ------------------------------------------------------------------ */
/* internal axis-agnostic socket helpers                               */
/* ------------------------------------------------------------------ */

static constexpr socket_t BAD_SOCKET = -1;

static void net_init_os()
{
#ifdef _WIN32
    WSADATA wsa;
    ::WSAStartup( MAKEWORD( 2, 2 ), &wsa );
#endif
}
static void net_deinit_os()
{
#ifdef _WIN32
    ::WSACleanup();
#endif
}

static void close_socket( socket_t s )
{
    if( s >= 0 ) {
#ifdef _WIN32
        ::closesocket( static_cast<SOCKET>( s ) );
#else
        ::close( static_cast<int>( s ) );
#endif
    }
}

/* ------------------------------------------------------------------ */
/* wire helpers (little-endian)                                        */
/* ------------------------------------------------------------------ */

static void put_u16( std::vector<char> &buf, uint16_t v )
{
    buf.push_back( static_cast<char>( v & 0xff ) );
    buf.push_back( static_cast<char>( ( v >> 8 ) & 0xff ) );
}
static void put_u32( std::vector<char> &buf, uint32_t v )
{
    for( int i = 0; i < 4; i++ ) {
        buf.push_back( static_cast<char>( ( v >> ( 8 * i ) ) & 0xff ) );
    }
}
static void put_i32( std::vector<char> &buf, int32_t v )
{
    put_u32( buf, static_cast<uint32_t>( v ) );
}
static void put_str( std::vector<char> &buf, const std::string &s )
{
    put_u16( buf, static_cast<uint16_t>( s.size() ) );
    buf.insert( buf.end(), s.begin(), s.end() );
}
static void put_str32( std::vector<char> &buf, const std::string &s )
{
    put_u32( buf, static_cast<uint32_t>( s.size() ) );
    buf.insert( buf.end(), s.begin(), s.end() );
}
static void put_str8( std::vector<char> &buf, const std::string &s )
{
    const size_t n = std::min<size_t>( s.size(), 255 );
    buf.push_back( static_cast<char>( n ) );
    buf.insert( buf.end(), s.begin(), s.begin() + static_cast<std::ptrdiff_t>( n ) );
}

struct reader {
    const char *data = nullptr;
    size_t size = 0;
    size_t off = 0;
    bool ok = true;

    uint16_t get_u16() {
        if( off + 2 > size ) {
            ok = false;
            return 0;
        }
        const uint16_t v = static_cast<uint16_t>( static_cast<uint8_t>( data[off] ) ) |
                           static_cast<uint16_t>( static_cast<uint8_t>( data[off + 1] ) ) << 8;
        off += 2;
        return v;
    }
    uint32_t get_u32() {
        if( off + 4 > size ) {
            ok = false;
            return 0;
        }
        uint32_t v = 0;
        for( int i = 0; i < 4; i++ ) {
            v |= static_cast<uint32_t>( static_cast<uint8_t>( data[off + i] ) ) << ( 8 * i );
        }
        off += 4;
        return v;
    }
    int32_t get_i32()
    {
        return static_cast<int32_t>( get_u32() );
    }
    std::string get_str() {
        const uint16_t len = get_u16();
        if( !ok || off + len > size ) {
            ok = false;
            return {};
        }
        std::string s( data + off, data + off + len );
        off += len;
        return s;
    }
    uint8_t get_u8() {
        if( off + 1 > size ) {
            ok = false;
            return 0;
        }
        return static_cast<uint8_t>( data[off++] );
    }
    std::string get_str32() {
        const uint32_t len = get_u32();
        if( !ok || off + len > size ) {
            ok = false;
            return {};
        }
        std::string s( data + off, data + off + len );
        off += len;
        return s;
    }
    std::string get_str8() {
        const uint8_t len = get_u8();
        if( !ok || off + len > size ) {
            ok = false;
            return {};
        }
        std::string s( data + off, data + off + len );
        off += len;
        return s;
    }
};

/* ------------------------------------------------------------------ */
/* framing                                                             */
/* ------------------------------------------------------------------ */

static std::vector<char> encode_frame( frame op, const std::vector<char> &payload )
{
    std::vector<char> buf;
    put_u16( buf, static_cast<uint16_t>( op ) );
    put_u32( buf, static_cast<uint32_t>( payload.size() ) );
    buf.insert( buf.end(), payload.begin(), payload.end() );
    return buf;
}

static bool send_all( socket_t s, const std::vector<char> &data )
{
    size_t sent = 0;
    while( sent < data.size() ) {
        const size_t chunk = std::min<size_t>( data.size() - sent, 0xFFFF );
#ifdef _WIN32
        const int n = ::send( static_cast<SOCKET>( s ), data.data() + sent,
                              static_cast<int>( chunk ), 0 );
#else
        const int n = ::send( static_cast<int>( s ), data.data() + sent,
                              static_cast<int>( chunk ), 0 );
#endif
        if( n <= 0 ) {
            return false;
        }
        sent += static_cast<size_t>( n );
    }
    return true;
}

static bool recv_all( socket_t s, char *dst, size_t bytes )
{
    size_t got = 0;
    while( got < bytes ) {
#ifdef _WIN32
        const int n = ::recv( static_cast<SOCKET>( s ), dst + got,
                              static_cast<int>( std::min<size_t>( bytes - got, 0xFFFF ) ), 0 );
#else
        const int n = ::recv( static_cast<int>( s ), dst + got,
                              static_cast<int>( std::min<size_t>( bytes - got, 0xFFFF ) ), 0 );
#endif
        if( n <= 0 ) {
            return false;
        }
        got += static_cast<size_t>( n );
    }
    return true;
}

static bool recv_frame( socket_t s, frame &op, std::vector<char> &payload )
{
    char head[6];
    if( !recv_all( s, head, sizeof( head ) ) ) {
        return false;
    }
    reader hdr { head, sizeof( head ) };
    const uint16_t opcode = hdr.get_u16();
    const uint32_t len = hdr.get_u32();
    if( !hdr.ok || len > 32u * 1024u * 1024u ) {
        return false;
    }
    payload.resize( len );
    if( len > 0 && !recv_all( s, payload.data(), len ) ) {
        return false;
    }
    op = static_cast<frame>( opcode );
    return true;
}

/* ------------------------------------------------------------------ */
/* internal state                                                      */
/* ------------------------------------------------------------------ */

namespace internal
{

struct netplay_state {
    netplay_state() {
        net_init_os();
    }
    ~netplay_state() {
        // DDA MP: static state is destroyed by an atexit destructor; any
        // still-joinable std::thread would std::terminate during destruction.
        // stop_* are CAS-guarded, so sweep the members explicitly.
        stop_client();
        stop_host();
        if( accept_thread.joinable() ) {
            accept_thread.join();
        }
        if( client_thread.joinable() ) {
            client_thread.join();
        }
        std::vector<std::thread> leftover;
        {
            std::lock_guard<std::mutex> lk( sessions_mutex );
            for( auto &[id, th] : id2reader ) {
                ( void )id;
                if( th.joinable() ) {
                    leftover.push_back( std::move( th ) );
                }
            }
            id2reader.clear();
        }
        // Join without holding sessions_mutex (threads acquire it).
        for( std::thread &th : leftover ) {
            if( th.joinable() ) {
                th.join();
            }
        }
        net_deinit_os();
    }

    void stop_host();
    void stop_client();

    /* ---- host session ---- */
    socket_t listen_sock = BAD_SOCKET;
    int listen_port = 0;
    std::atomic<bool> running = false;
    std::thread accept_thread;

    std::mutex sessions_mutex;
    int session_watermark = 0;
    std::map<int, socket_t> id2sock;
    std::map<int, std::string> id2name;
    std::map<int, std::thread> id2reader;

    /* queued client keystrokes (wire codes), bounded to avoid hogging */
    std::mutex keys_mutex;
    std::deque<uint32_t> host_keys;
    /* DDA MP v4.0a-2: the same wire with the session id attached, so the
     * host glue can steer each client's own avatar instead of the host
     * player. Routed to the legacy queue by the main thread when the sid
     * has no bound avatar (watchers keep v3.x behavior). One slot per
     * session: a session may hold exactly one action in flight, newer
     * keystrokes are dropped until the host pump executed the previous one
     * (see the reader thread below). */
    std::map<int, sid_key> avatar_keys;

    /* a client asked for the current picture (SNAP_REQ); consumed by the
     * host main thread — reader threads must not touch game state */
    std::atomic<bool> snap_request_pending = false;

    /* ---- client ---- */
    std::atomic<bool> connected = false;
    socket_t client_sock = BAD_SOCKET;
    std::thread client_thread;
    std::string client_address;
    std::string last_address;
    std::string deny_reason;
    /* DDA MP v4.0a-226: set by the reader when the host sends BYE with a
     * reason; the client UI then returns to the main menu, not the reconnect
     * screen. Read after client_run() ends. */
    bool client_kicked_flag = false;
    std::string client_kick_reason_text;
    /* world the host announced in WELCOME (fat client) */
    std::string host_world_name;
    std::string client_last_host_world;
    /* DDA MP v4.0a-122: host world mod ids from WELCOME. */
    std::vector<std::string> client_last_host_mods;
    /* DDA MP v4.0a-326: data manifest from WELCOME. */
    std::string client_last_host_game_version;
    std::string client_last_host_netplay_version;
    std::string client_last_host_manifest_hash;
    std::string client_last_host_tileset;
    std::vector<std::pair<std::string, std::string>> client_last_host_mod_versions;
    /* DDA MP v4.0a-123: host world options from WELCOME. */
    std::vector<std::pair<std::string, std::string>> client_last_host_world_options;
    /* DDA MP v4.0: our session id from WELCOME (avatar binding identity). */
    int client_sid = -1;
    /* DDA MP v4.0 avatar flow, guarded by avatar_mutex; the ready flag is a
     * plain atomic because it flips without payload access. */
    std::mutex avatar_mutex;
    std::vector<avatar_entry> avatar_list;
    std::atomic<int> avatar_bind_state = 0;   /* 0 idle, 1 wait, 2 ok, 3 denied */
    std::atomic<bool> avatar_list_ready = false;
    std::string avatar_deny_reason;
    snapshot latest;
    bool have_latest = false;
    /* DDA MP v4.0a-7: one short host->client status line (turn pacing hints),
     * guarded by hover_mutex; the view loop consumes it. */
    std::mutex hover_mutex;
    std::string hover_text;
    bool have_hover = false;
    /* DDA MP v4.0a-8: latest character sheet text from the host. */
    std::mutex sheet_mutex;
    std::string sheet_text;
    bool have_sheet = false;
    /* DDA MP v4.0a-27: serialized session NPC for the client @ menu. */
    std::mutex char_json_mutex;
    std::string char_json;
    bool have_char_json = false;
    /* DDA MP v4.0a-9: latest host-rendered sidebar cells. */
    std::mutex sidebar_mutex;
    sidebar_frame sidebar;
    bool have_sidebar = false;
    /* DDA MP v4.0a-147: host panel_manager layout json. */
    std::mutex panel_layout_mutex;
    std::string panel_layout;
    bool have_panel_layout = false;
    /* DDA MP v4.0a-11: event lines for the client's own message log. */
    std::mutex event_mutex;
    std::deque<std::pair<int, std::string>> client_events;
    /* DDA MP v4.0a-12: overmap minimap block. */
    std::mutex minimap_mutex;
    minimap_frame minimap;
    bool have_minimap = false;
    /* DDA MP v4.0a-243: seen-overmap block for the stock map UI. */
    std::mutex overmap_mutex;
    overmap_block overmap;
    bool have_overmap = false;
    /* DDA MP v4.0a-247: generic prompt bridge. One host prompt at a time. */
    std::mutex prompt_mutex;
    std::condition_variable prompt_cv;
    bool prompt_has_answer = false;
    prompt_answer prompt_result;
    uint16_t prompt_next_id = 1;
    /* Client side: prompts waiting to be shown, and the last answer id. */
    std::mutex client_prompt_mutex;
    std::deque<prompt_request> client_prompts;
    uint16_t prompt_answer_id = 0;
    /* DDA MP v4.0a-14: inventory listing text. */
    std::mutex inv_mutex;
    std::string inv_text;
    bool have_inv = false;
    /* DDA MP v4.0a-90: ground items for the client's pickup selector. */
    std::mutex pickup_mutex;
    std::string pickup_text;
    bool have_pickup = false;
    /* DDA MP v4.0a-97: adjacent tiles to choose from before that listing. */
    std::mutex pickup_tiles_mutex;
    std::string pickup_tiles_text;
    bool have_pickup_tiles = false;
    /* DDA MP v4.0a-43: latest host aiming panel text. */
    std::mutex aim_mutex;
    std::string aim_text;
    bool have_aim = false;
    /* DDA MP: rendered aiming window for the client. */
    std::mutex aim_panel_mutex;
    sidebar_frame aim_panel;
    bool have_aim_panel = false;
    std::mutex aim_targets_mutex;
    std::vector<aim_target> aim_targets;
    bool have_aim_targets = false;
    /* DDA MP v4.0a-9: terminal size reported by each client (host side). */
    std::mutex view_mutex;
    std::map<int, std::pair<int, int>> client_views;
    /* DDA MP v4.0a-232: view offset (tiles from the avatar) the client wants
     * the streamed frame centred on. Look/fire camera scrolling. */
    std::map<int, std::pair<int, int>> client_view_offsets;
    std::map<int, int> client_view_z;
    std::vector<std::vector<char>> client_out;
    std::mutex client_out_mutex;

    /* DDA MP v4.0: avatar ops queued by session reader threads, consumed on
     * the host main thread (game glue may not run off the main thread). */
    std::mutex avatar_ops_mutex;
    std::deque<avatar_op> avatar_ops;

    /* DDA MP v4.0a-18: inventory actions (row index + action key) queued by
     * reader threads and applied to the session avatar on the main thread. */
    struct inv_action {
        int sid = 0;
        int node = 0;
        int action = 0;
        int generation = 0;
        std::vector<int> targets;
    };
    std::mutex inv_act_mutex;
    std::deque<inv_action> inv_actions;

    /* DDA MP v4.0a-72: action-menu choices queued by reader threads. */
    struct client_action {
        int sid = 0;
        int action = 0;
    };
    std::mutex client_action_mutex;
    std::deque<client_action> client_actions;

    /* DDA MP v4.0a-88: per-session AFK flag (long wait on the client). */
    std::mutex afk_mutex;
    std::map<int, bool> session_afk;

    /* DDA MP v4.0a-223: silent-session heartbeat. last_rx is refreshed by the
     * reader thread on every frame; ping_sent marks an unanswered PING and
     * dropping marks a session already reported, so a stuck reader cannot
     * produce a second disconnect message. Guarded by hb_mutex. */
    std::mutex hb_mutex;
    std::map<int, std::chrono::steady_clock::time_point> session_last_rx;
    std::map<int, std::chrono::steady_clock::time_point> session_ping_sent;
    std::map<int, std::chrono::steady_clock::time_point> session_dropping;

    /* DDA MP v4.0a-223: client frames may be written by the main thread (keys,
     * requests) and by the reader thread (PONG), so writes are serialized. */
    std::mutex client_send_mutex;

    /* DDA MP v4.0a-90: pickup listings (host) and the chosen rows. */
    struct pickup_action {
        int sid = 0;
        int generation = 0;
        std::vector<int> nodes;
        int equip = 0;
        std::string uids;
    };
    std::mutex pickup_act_mutex;
    std::deque<pickup_action> pickup_actions;
    /* DDA MP v4.0a-97: which adjacent tile the client chose to pick from. */
    struct pickup_tile_req {
        int sid = 0;
        int generation = 0;
        int dx = 0;
        int dy = 0;
    };
    std::mutex pickup_tile_mutex;
    std::deque<pickup_tile_req> pickup_tile_reqs;

    /* DDA MP v4.0a-31: pocket-settings payloads (JSON) queued likewise. */
    struct inv_settings {
        int sid = 0;
        int node = 0;
        int generation = 0;
        std::string json;
    };
    std::mutex inv_set_mutex;
    std::deque<inv_settings> inv_settings_q;

    struct aim_req {
        int sid = 0;
        int node = 0;
        int generation = 0;
        int dx = 0;
        int dy = 0;
    };
    std::mutex aim_req_mutex;
    std::deque<aim_req> aim_requests;

    /* DDA MP v4.1: intents whose action id has no host handler yet. Known
     * ids are demultiplexed into the typed queues above by the reader thread. */
    std::mutex intent_mutex;
    std::deque<intent_request> intents;

    /* DDA MP v4.1: uid-addressed inventory actions (mirror model). */
    std::mutex inv_uid_mutex;
    std::deque<inventory_uid_action> inv_uid_q;
};

void netplay_state::stop_host()
{
    bool expect = true;
    if( running.compare_exchange_strong( expect, false ) ) {
        close_socket( listen_sock );
        listen_sock = BAD_SOCKET;
        if( accept_thread.joinable() ) {
            accept_thread.join();
        }
        std::vector<std::thread> readers;
        {
            std::lock_guard<std::mutex> lk( sessions_mutex );
            // Politely let every session know we're leaving before the socket
            // teardown; clients will then show their "disconnected" state.
            for( auto &[id, sock] : id2sock ) {
                ( void )id;
                send_all( sock, encode_frame( frame::BYE, {} ) );
            }
            for( auto &[id, sock] : id2sock ) {
                ( void )id;
                close_socket( sock );
            }
            id2sock.clear();
            id2name.clear();
            for( auto &[id, th] : id2reader ) {
                ( void )id;
                if( th.joinable() ) {
                    readers.push_back( std::move( th ) );
                }
            }
            id2reader.clear();
        }
        // Join without holding sessions_mutex: reader threads acquire it while
        // unregistering themselves.
        for( std::thread &th : readers ) {
            if( th.joinable() ) {
                th.join();
            }
        }
    }
}

void netplay_state::stop_client()
{
    bool expect = true;
    if( connected.compare_exchange_strong( expect, false ) ) {
        close_socket( client_sock );
        client_sock = BAD_SOCKET;
        if( client_thread.joinable() ) {
            client_thread.join();
        }
    }
    have_latest = false;
    latest = snapshot {};
}

} // namespace internal

static internal::netplay_state state;
static std::mutex state_mutex; // guards init/shutdown

/**
 * DDA MP v4.0a-223: every client frame goes through one write lock. The
 * reader thread answers PING with PONG, so two raw send_all() calls from
 * different threads could interleave two frames on the wire.
 */
static bool client_send_on( socket_t sock, frame op, const std::vector<char> &payload )
{
    std::lock_guard<std::mutex> lk( state.client_send_mutex );
    return send_all( sock, encode_frame( op, payload ) );
}


/* ------------------------------------------------------------------ */
/* fat client world data                                                */
/* ------------------------------------------------------------------ */

void host_set_world_name( const std::string &name )
{
    std::lock_guard<std::mutex> lk( state_mutex );
    state.client_last_host_world = name;
}

std::string client_host_world_name()
{
    std::lock_guard<std::mutex> lk( state_mutex );
    return state.client_last_host_world;
}

std::vector<std::string> client_host_mods()
{
    std::lock_guard<std::mutex> lk( state_mutex );
    return state.client_last_host_mods;
}

std::string client_host_game_version()
{
    std::lock_guard<std::mutex> lk( state_mutex );
    return state.client_last_host_game_version;
}

std::string client_host_netplay_version()
{
    std::lock_guard<std::mutex> lk( state_mutex );
    return state.client_last_host_netplay_version;
}

std::string client_host_manifest_hash()
{
    std::lock_guard<std::mutex> lk( state_mutex );
    return state.client_last_host_manifest_hash;
}

std::string client_host_tileset()
{
    std::lock_guard<std::mutex> lk( state_mutex );
    return state.client_last_host_tileset;
}

std::vector<std::pair<std::string, std::string>> client_host_mod_versions()
{
    std::lock_guard<std::mutex> lk( state_mutex );
    return state.client_last_host_mod_versions;
}

void client_set_join_error( const std::string &reason )
{
    std::lock_guard<std::mutex> lk( state_mutex );
    state.client_kicked_flag = true;
    state.client_kick_reason_text = reason;
}

std::vector<std::pair<std::string, std::string>> client_host_world_options()
{
    std::lock_guard<std::mutex> lk( state_mutex );
    return state.client_last_host_world_options;
}

/* ------------------------------------------------------------------ */
/* tiles replay switch                                                 */
/* ------------------------------------------------------------------ */

/// kill-switch for the tileset replay renderer (host mining + client
/// rendering). DDA_MP_TILES=0 disables; used for bisection debugging.
bool tiles_replay_enabled()
{
    static int cached = -1;
    if( cached < 0 ) {
#ifdef _WIN32
        char buf[8];
        size_t sz = 0;
        if( !getenv_s( &sz, buf, sizeof( buf ), "DDA_MP_TILES" ) && sz > 0 ) {
            cached = ( buf[0] == '0' ) ? 0 : 1;
            return cached != 0;
        }
#else
        if( const char *v = getenv( "DDA_MP_TILES" ) ) {
            cached = ( v[0] == '0' ) ? 0 : 1;
            return cached != 0;
        }
#endif
        cached = 1;
    }
    return cached != 0;
}

/* ------------------------------------------------------------------ */
/* built-in diagnostics folder                                          */
/* ------------------------------------------------------------------ */

namespace
{
// Runtime switch from the host menu. DDA_MP_DIAG overrides it: a folder
// forces diagnostics on, 0/off forces them off.
std::atomic<bool> netplay_diag_on{ false };

const std::string &env_diag_dir()
{
    static const std::string dir = []() -> std::string {
        const char *env = std::getenv( "DDA_MP_DIAG" );
        if( env == nullptr ) {
            return std::string();
        }
        const std::string v( env );
        if( v.empty() || v == "0" || v == "off" ) {
            return std::string();
        }
        CreateDirectoryA( v.c_str(), nullptr );
        return v;
    }();
    return dir;
}

bool env_diag_present()
{
    static const bool present = std::getenv( "DDA_MP_DIAG" ) != nullptr;
    return present;
}

const std::string &runtime_diag_dir()
{
    static const std::string dir = []() -> std::string {
        const char *temp = std::getenv( "TEMP" );
        if( temp == nullptr ) {
            temp = std::getenv( "TMP" );
        }
        std::string d = ( temp ? temp : "." );
        d += "\\opencode\\ddadiag";
        CreateDirectoryA( d.c_str(), nullptr );
        return d;
    }();
    return dir;
}

const std::string &active_diag_dir()
{
    static const std::string empty;
    if( env_diag_present() ) {
        return env_diag_dir();
    }
    if( netplay_diag_on.load() ) {
        return runtime_diag_dir();
    }
    return empty;
}
} // namespace

void set_diag_enabled( bool enabled )
{
    netplay_diag_on.store( enabled );
}

bool diag_enabled_by_user()
{
    return netplay_diag_on.load();
}

std::string mp_diag_dir()
{
    return active_diag_dir();
}

bool mp_diag_enabled()
{
    return !active_diag_dir().empty();
}

std::FILE *mp_diag_open( const char *name )
{
    const std::string &dir = active_diag_dir();
    if( dir.empty() ) {
        return nullptr;
    }
    return std::fopen( ( dir + "\\" + name ).c_str(), "a" );
}

/* crash diagnostics black box                                          */
/* ------------------------------------------------------------------ */

static void mp_diag_write( const std::string &s )
{
    std::FILE *f = netplay::mp_diag_open( "netplay_tiles.log" );
    if( f ) {
        std::fputs( s.c_str(), f );
        std::fputs( "\n", f );
        std::fclose( f );
    }
}

/* DDA MP v4.0a-252: always-on crash black box. The old hooks only wrote when
 * DDA_MP_DIAG was set, so a normal launcher start left no trace at all. These
 * write a text signature/stack and a minidump into the userdir config, where
 * the next autosave crash can be symbolicated offline. */
#ifdef _WIN32
static char mp_crash_log_path[MAX_PATH] = {};
static char mp_crash_dump_path[MAX_PATH] = {};

static void mp_crash_init_paths()
{
    if( mp_crash_log_path[0] != '\0' ) {
        return;
    }
    const std::string &cfg = PATH_INFO::config_dir();
    std::snprintf( mp_crash_log_path, sizeof( mp_crash_log_path ), "%snetplay_crash.log",
                   cfg.c_str() );
    std::snprintf( mp_crash_dump_path, sizeof( mp_crash_dump_path ), "%snetplay_crash.dmp",
                   cfg.c_str() );
}

static void mp_crash_write( const char *s )
{
    mp_crash_init_paths();
    if( std::FILE *f = std::fopen( mp_crash_log_path, "a" ) ) {
        std::fputs( s, f );
        std::fputs( "\n", f );
        std::fclose( f );
    }
    mp_diag_write( s );
}

static void mp_crash_stack( const char *tag )
{
    void *frames[64] = {};
    const unsigned short n = CaptureStackBackTrace( 0, 64, frames, nullptr );
    const HMODULE exe = GetModuleHandleA( nullptr );
    char line[512];
    std::snprintf( line, sizeof( line ), "%s STACK n=%u", tag,
                   static_cast<unsigned>( n ) );
    mp_crash_write( line );
    for( unsigned short i = 0; i < n; i++ ) {
        HMODULE mod = nullptr;
        GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>( frames[i] ), &mod );
        const uintptr_t addr = reinterpret_cast<uintptr_t>( frames[i] );
        const uintptr_t base = reinterpret_cast<uintptr_t>( mod );
        const ptrdiff_t off = static_cast<ptrdiff_t>( addr - base );
        if( mod == exe ) {
            std::snprintf( line, sizeof( line ), "  [exe+0x%zx]", static_cast<size_t>( off ) );
        } else if( mod != nullptr ) {
            char name[MAX_PATH] = {};
            GetModuleFileNameA( mod, name, MAX_PATH );
            const char *slash = std::strrchr( name, '\\' );
            std::snprintf( line, sizeof( line ), "  [%s+0x%zx]", slash ? slash + 1 : name,
                           static_cast<size_t>( off ) );
        } else {
            std::snprintf( line, sizeof( line ), "  [%p]", frames[i] );
        }
        mp_crash_write( line );
    }
}

static LONG WINAPI mp_crash_filter( EXCEPTION_POINTERS *ep )
{
    char line[256];
    std::snprintf( line, sizeof( line ), "UNHANDLED EXCEPTION code=0x%08lx addr=%p",
                   static_cast<unsigned long>( ep->ExceptionRecord->ExceptionCode ),
                   ep->ExceptionRecord->ExceptionAddress );
    mp_crash_write( line );
    mp_crash_stack( "EXCEPTION" );
    mp_crash_init_paths();
    if( mp_crash_dump_path[0] != '\0' ) {
        HANDLE h = CreateFileA( mp_crash_dump_path, GENERIC_WRITE, 0, nullptr,
                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr );
        if( h != INVALID_HANDLE_VALUE ) {
            MINIDUMP_EXCEPTION_INFORMATION mei{};
            mei.ThreadId = GetCurrentThreadId();
            mei.ExceptionPointers = ep;
            mei.ClientPointers = FALSE;
            MiniDumpWriteDump( GetCurrentProcess(), GetCurrentProcessId(), h,
                               MiniDumpNormal, &mei, nullptr, nullptr );
            CloseHandle( h );
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// DDA MP: capture stderr (cata_assert prints the failing file:line there).
static void mp_diag_redirect_stderr()
{
    if( std::freopen( "netplay_stderr.log", "w", stderr ) == nullptr ) {
        mp_diag_write( "stderr redirect failed" );
    } else {
        setvbuf( stderr, nullptr, _IONBF, 0 );
    }
}

#ifdef _WIN32
static void mp_diag_invalid_parameter( const wchar_t *expression,
                                       const wchar_t * /*function*/,
                                       const wchar_t *file,
                                       unsigned int line,
                                       uintptr_t /*reserved*/ )
{
    std::string msg = "CRT INVALID PARAMETER";
    try {
        auto to8 = []( const wchar_t *w ) {
            std::string out;
            if( w == nullptr ) {
                return out;
            }
            const int n = WideCharToMultiByte( CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr );
            if( n > 1 ) {
                out.resize( static_cast<size_t>( n ) - 1 );
                WideCharToMultiByte( CP_UTF8, 0, w, -1, out.data(), n, nullptr, nullptr );
            }
            return out;
        };
        if( expression != nullptr && expression[0] != L'\0' ) {
            msg += " expr=" + to8( expression );
        }
        if( file != nullptr && file[0] != L'\0' ) {
            msg += " file=" + to8( file ) + " line=" + std::to_string( line );
        }
    } catch( ... ) {
        msg += " <fmt failed>";
    }
    mp_crash_write( msg.c_str() );
    // returning lets the CRT invoke _invoke_watson (fail-fast) as usual
}
#endif

static void mp_diag_terminate()
{
    mp_crash_write( "TERMINATE (uncaught exception)" );
    try {
        if( std::current_exception() != nullptr ) {
            std::rethrow_exception( std::current_exception() );
        } else {
            mp_crash_write( "  no active exception" );
        }
    } catch( const std::exception &e ) {
        const std::string m = std::string( "  std::exception: " ) + e.what();
        mp_crash_write( m.c_str() );
    } catch( ... ) {
        mp_crash_write( "  non-std exception" );
    }
    mp_crash_stack( "TERMINATE" );
    std::abort();
}

static void mp_diag_sigabrt( int /*sig*/ )
{
    mp_crash_write( "ABORT() invoked" );
    mp_crash_stack( "ABORT" );
}

bool init()
{
    // DDA MP crash "black box": UCRT fail-fasts (invalid parameter, uncaught
    // exception, abort) cannot be caught with try/catch. These hooks write
    // the failure signature into netplay_tiles.log right before death so the
    // origin point is identifiable from the log alone.
    static bool diag_hooks_installed = [] {
        try {
            mp_diag_redirect_stderr();
#ifdef _WIN32
            mp_crash_init_paths();
            SetUnhandledExceptionFilter( mp_crash_filter );
            _set_invalid_parameter_handler( mp_diag_invalid_parameter );
#endif
            std::set_terminate( mp_diag_terminate );
            std::signal( SIGABRT, mp_diag_sigabrt );
            std::signal( SIGABRT, mp_diag_sigabrt );
            mp_diag_write( "diag hooks installed" );
        } catch( ... ) {
            // diagnostics must never break startup
        }
        return true;
    }();

    return true; // constructed the first time `state` is touched
}
void shutdown()
{
    host_region_shutdown();
    std::lock_guard<std::mutex> lk( state_mutex );
    state.stop_client();
    state.stop_host();
}

/* ------------------------------------------------------------------ */
/* host                                                                */
/* ------------------------------------------------------------------ */

/* compression lives below the reader threads; declare the pieces they use */
static std::vector<char> maybe_compress( const std::vector<char> &raw );
static std::vector<char> maybe_decompress( const char *data, size_t size );

static void host_session_reader( int sid, socket_t sock )
{
    while( true ) {
        frame op;
        std::vector<char> payload;
        if( !recv_frame( sock, op, payload ) ) {
            if( mp_diag_enabled() ) {
                if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                    std::fprintf( f, "reader exit sid=%d\n", sid );
                    std::fclose( f );
                }
            }
            break;
        }
        // DDA MP v4.0a-223: any frame, PONG included, proves the client is
        // alive, so the heartbeat timer restarts here.
        {
            std::lock_guard<std::mutex> hb_lk( state.hb_mutex );
            state.session_last_rx[sid] = std::chrono::steady_clock::now();
            state.session_ping_sent.erase( sid );
        }
        if( mp_diag_enabled() ) {
            if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                std::fprintf( f, "rx frame sid=%d op=%u len=%d\n", sid,
                              static_cast<unsigned>( op ),
                              static_cast<int>( payload.size() ) );
                std::fclose( f );
            }
        }
        if( op == frame::KEYS ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const uint32_t n = r.get_u32();
            for( uint32_t i = 0; i < n && r.ok; i++ ) {
                const uint32_t code = r.get_u32();
                r.get_i32(); // mods, unused in v1
                if( !r.ok ) {
                    break;
                }
                if( mp_diag_enabled() ) {
                    if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                        std::fprintf( f, "rx key wait sid=%d code=%u\n", sid, code );
                        std::fclose( f );
                    }
                }
                std::lock_guard<std::mutex> lk( state.keys_mutex );
                // DDA MP v4.0a-2: strictly one action in flight per
                // session. While the host pump has not executed the session's
                // previous key, the slot stays occupied and newer keystrokes
                // (key repeats included) are dropped: a client must not be
                // able to stack a mountain of steps ahead of the world turns
                // they belong to.
                state.avatar_keys[sid] = sid_key{ sid, code };
                if( mp_diag_enabled() ) {
                    if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                        std::fprintf( f, "rx key sid=%d code=%u\n", sid, code );
                        std::fclose( f );
                    }
                }
            }
        } else if( op == frame::SNAP_REQ ) {
            // "send the current picture". Optionally carries the view offset
            // (tiles from the avatar) the client's look/fire camera needs.
            // Consumed on the host main thread (game loop pump).
            if( payload.size() >= 4 ) {
                reader r;
                r.data = payload.data();
                r.size = payload.size();
                const int dx = static_cast<int>( r.get_u16() ) - 32768;
                const int dy = static_cast<int>( r.get_u16() ) - 32768;
                int dz = 0;
                if( payload.size() >= 6 ) {
                    dz = static_cast<int>( r.get_u16() ) - 32768;
                }
                if( r.ok ) {
                    std::lock_guard<std::mutex> lk( state.view_mutex );
                    state.client_view_offsets[sid] = { dx, dy };
                    state.client_view_z[sid] = dz;
                }
            }
            state.snap_request_pending = true;
        } else if( op == frame::CLIENT_VIEW ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const int cols = static_cast<int>( r.get_u16() );
            const int rows = static_cast<int>( r.get_u16() );
            if( r.ok && cols > 0 && rows > 0 ) {
                std::lock_guard<std::mutex> lk( state.view_mutex );
                state.client_views[sid] = { cols, rows };
            }
        } else if( op == frame::PROMPT_RES ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            prompt_answer ans;
            ans.id = r.get_u16();
            ans.canceled = r.get_u8() != 0;
            ans.retval = static_cast<int>( r.get_i32() );
            ans.index = static_cast<int>( r.get_i32() );
            ans.text = r.get_str8();
            if( r.ok ) {
                {
                    std::lock_guard<std::mutex> lk( state.prompt_mutex );
                    state.prompt_result = ans;
                    state.prompt_has_answer = true;
                }
                state.prompt_cv.notify_all();
            }
        } else if( op == frame::AVATAR_LIST_REQ || op == frame::AVATAR_RESUME ||
                   op == frame::AVATAR_PROFILE ) {
            // DDA MP v4.0: queue the avatar op for the host main thread with
            // the session id of the sender. Reader threads never touch the
            // game / world state.
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            avatar_op avop;
            avop.sid = sid;
            avop.kind = op == frame::AVATAR_LIST_REQ ? 1 :
                        ( op == frame::AVATAR_RESUME ? 2 : 3 );
            // [str nick] on every op; AVATAR_PROFILE adds [u32 len][json]
            avop.nick = r.get_str();
            if( op == frame::AVATAR_PROFILE && r.ok ) {
                const uint32_t dlen = r.get_u32();
                if( r.ok && static_cast<size_t>( dlen ) <= r.size - r.off ) {
                    std::vector<char> raw( r.data + r.off, r.data + r.off + dlen );
                    r.off += dlen;
                    if( raw.size() > 512 ) {
                        raw = maybe_decompress( raw.data(), raw.size() );
                    }
                    avop.json.assign( raw.begin(), raw.end() );
                }
            }
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.avatar_ops_mutex );
                if( state.avatar_ops.size() < 8 ) {
                    state.avatar_ops.push_back( std::move( avop ) );
                } else {
                    std::vector<char> d;
                    put_str( d, "Server busy, avatar request dropped. Try again." );
                    // NOTE (v4.0a-226): this send runs on the reader thread
                    // without a per-socket send lock, while the main thread
                    // may be writing a snapshot to the same socket. Rare path
                    // (avatar queue full); accepted for host + 1 client.
                    send_all( sock, encode_frame( frame::AVATAR_DENIED, d ) );
                }
            }
        } else if( op == frame::INVENTORY_ACTION ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const int node = r.get_i32();
            const int action = r.get_i32();
            const int generation = r.get_i32();
            const uint16_t ntargets = r.get_u16();
            std::vector<int> targets;
            for( uint16_t i = 0; i < ntargets && r.ok; i++ ) {
                targets.push_back( r.get_i32() );
            }
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.inv_act_mutex );
                if( state.inv_actions.size() < 8 ) {
                    state.inv_actions.push_back( { sid, node, action, generation,
                                                   std::move( targets ) } );
                }
            }
        } else if( op == frame::INVENTORY_SETTINGS ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const int node = r.get_i32();
            const int generation = r.get_i32();
            const std::string json = r.get_str();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.inv_set_mutex );
                if( state.inv_settings_q.size() < 8 ) {
                    state.inv_settings_q.push_back( { sid, node, generation, json } );
                }
            }
        } else if( op == frame::AIM_INFO_REQ ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const int node = r.get_i32();
            const int generation = r.get_i32();
            const int dx = r.get_i32();
            const int dy = r.get_i32();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.aim_req_mutex );
                if( state.aim_requests.size() < 8 ) {
                    state.aim_requests.push_back( { sid, node, generation, dx, dy } );
                }
            }
        } else if( op == frame::CLIENT_ACTION ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const int action = r.get_i32();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.client_action_mutex );
                if( state.client_actions.size() < 8 ) {
                    state.client_actions.push_back( { sid, action } );
                }
            }
        } else if( op == frame::CLIENT_AFK ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const uint8_t on = r.get_u8();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.afk_mutex );
                state.session_afk[sid] = on != 0;
            }
        } else if( op == frame::PICKUP_ACTION ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const int generation = r.get_i32();
            const uint16_t n = r.get_u16();
            std::vector<int> nodes;
            for( uint16_t i = 0; i < n && r.ok; i++ ) {
                nodes.push_back( r.get_i32() );
            }
            int equip = 0;
            // v4.0a-101: optional trailing byte - take / wield / wear.
            if( r.ok && r.off < r.size ) {
                equip = r.get_u8();
            }
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.pickup_act_mutex );
                if( state.pickup_actions.size() < 8 ) {
                    state.pickup_actions.push_back( { sid, generation, std::move( nodes ), equip } );
                }
            }
        } else if( op == frame::PICKUP_TILE ) {
            // DDA MP v4.0a-97: [i32 gen][i32 dx][i32 dy]
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const int generation = r.get_i32();
            const int dx = r.get_i32();
            const int dy = r.get_i32();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.pickup_tile_mutex );
                if( state.pickup_tile_reqs.size() < 8 ) {
                    state.pickup_tile_reqs.push_back( { sid, generation, dx, dy } );
                }
            }
        } else if( op == frame::INTENT ) {
            // DDA MP v4.1: [str action][i32 gen][u16 n][i32 args[n]][str data].
            // Known actions are demultiplexed into the typed queues the host
            // pumps already consume; unknown ids wait for the main thread in
            // the generic intent queue.
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            intent_request in;
            in.sid = sid;
            in.action = r.get_str();
            in.generation = r.get_i32();
            const uint16_t nargs = r.get_u16();
            for( uint16_t i = 0; i < nargs && r.ok; i++ ) {
                in.args.push_back( r.get_i32() );
            }
            if( r.ok && r.off < r.size ) {
                in.data = r.get_str();
            }
            // Optional trailing command id (v4.0a-290+); absent on legacy
            // senders, which leaves the field at its default 0.
            if( r.ok && r.off + 4 <= r.size ) {
                in.command_id = r.get_u32();
            }
            if( r.ok && !in.action.empty() ) {
                if( in.action == "pickup.tiles" && in.args.size() >= 2 ) {
                    std::lock_guard<std::mutex> lk( state.pickup_tile_mutex );
                    if( state.pickup_tile_reqs.size() < 8 ) {
                        state.pickup_tile_reqs.push_back( { sid, in.generation,
                                                            in.args[0], in.args[1] } );
                    }
                } else if( in.action == "pickup.action" && !in.args.empty() ) {
                    std::lock_guard<std::mutex> lk( state.pickup_act_mutex );
                    if( state.pickup_actions.size() < 8 ) {
                        internal::netplay_state::pickup_action a;
                        a.sid = sid;
                        a.generation = in.generation;
                        a.equip = in.args[0];
                        a.nodes.assign( in.args.begin() + 1, in.args.end() );
                        a.uids = in.data;
                        state.pickup_actions.push_back( std::move( a ) );
                    }
                } else if( in.action == "inv.uid" && !in.args.empty() ) {
                    std::lock_guard<std::mutex> lk( state.inv_uid_mutex );
                    if( state.inv_uid_q.size() < 8 ) {
                        inventory_uid_action a;
                        a.sid = sid;
                        a.action = in.args[0];
                        a.ints.assign( in.args.begin() + 1, in.args.end() );
                        std::size_t pos = 0;
                        while( pos <= in.data.size() ) {
                            const std::size_t q = in.data.find( '\x1f', pos );
                            a.uids.push_back( in.data.substr( pos, q == std::string::npos ?
                                                              std::string::npos : q - pos ) );
                            if( q == std::string::npos ) {
                                break;
                            }
                            pos = q + 1;
                        }
                        if( !a.uids.empty() && !a.uids.front().empty() ) {
                            state.inv_uid_q.push_back( std::move( a ) );
                        }
                    }
                } else if( in.action == "inv.settings.uid" ) {
                    const std::size_t q = in.data.find( '\x1f' );
                    if( q != std::string::npos ) {
                        std::lock_guard<std::mutex> lk( state.inv_uid_mutex );
                        if( state.inv_uid_q.size() < 8 ) {
                            inventory_uid_action a;
                            a.sid = sid;
                            a.action = 'v';
                            a.uids.push_back( in.data.substr( 0, q ) );
                            a.json = in.data.substr( q + 1 );
                            state.inv_uid_q.push_back( std::move( a ) );
                        }
                    }
                } else if( in.action == "inv.action" && in.args.size() >= 2 ) {
                    std::lock_guard<std::mutex> lk( state.inv_act_mutex );
                    if( state.inv_actions.size() < 8 ) {
                        state.inv_actions.push_back( { sid, in.args[0], in.args[1],
                                                       in.generation,
                                                       std::vector<int>( in.args.begin() + 2,
                                                               in.args.end() ) } );
                    }
                } else if( in.action == "inv.settings" && !in.args.empty() ) {
                    std::lock_guard<std::mutex> lk( state.inv_set_mutex );
                    if( state.inv_settings_q.size() < 8 ) {
                        state.inv_settings_q.push_back( { sid, in.args[0], in.generation,
                                                          std::move( in.data ) } );
                    }
                } else if( in.action == "act.menu" && !in.args.empty() ) {
                    std::lock_guard<std::mutex> lk( state.client_action_mutex );
                    if( state.client_actions.size() < 8 ) {
                        state.client_actions.push_back( { sid, in.args[0] } );
                    }
                } else if( in.action == "aim.info" && in.args.size() >= 3 ) {
                    std::lock_guard<std::mutex> lk( state.aim_req_mutex );
                    if( state.aim_requests.size() < 8 ) {
                        state.aim_requests.push_back( { sid, in.args[0], in.generation,
                                                        in.args[1], in.args[2] } );
                    }
                } else {
                    std::lock_guard<std::mutex> lk( state.intent_mutex );
                    if( state.intents.size() < 32 ) {
                        state.intents.push_back( std::move( in ) );
                    }
                }
            }
        } else if( op == frame::PONG ) {
            // DDA MP v4.0a-223: answer to our PING; the timestamp was already
            // refreshed above, this branch only feeds the diagnostics.
            if( mp_diag_enabled() ) {
                if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                    std::fprintf( f, "hb pong sid=%d\n", sid );
                    std::fclose( f );
                }
            }
        } else if( op == frame::BYE ) {
            break;
        }
        // other frames are ignored on the host side for now
    }

    // DDA MP v4.0a-218/223: the session ends with its socket. The reader owns
    // the handle, so it unregisters the session and closes only if it removed
    // the entry itself. stop_host() erases the table first and closes there,
    // which keeps this from closing an already recycled handle.
    bool removed = false;
    {
        std::lock_guard<std::mutex> lk( state.sessions_mutex );
        removed = state.id2sock.erase( sid ) > 0;
        state.id2name.erase( sid );
    }
    {
        std::lock_guard<std::mutex> afk_lk( state.afk_mutex );
        state.session_afk.erase( sid );
    }
    {
        std::lock_guard<std::mutex> hb_lk( state.hb_mutex );
        state.session_last_rx.erase( sid );
        state.session_ping_sent.erase( sid );
        state.session_dropping.erase( sid );
    }
    if( removed ) {
        close_socket( sock );
    }
}

static void host_accept_loop()
{
    while( state.running ) {
        sockaddr_in addr;
        int addr_len = static_cast<int>( sizeof( addr ) );
#ifdef _WIN32
        const socket_t peer = static_cast<socket_t>( ::accept(
                                  static_cast<SOCKET>( state.listen_sock ),
                                  reinterpret_cast<sockaddr *>( &addr ), &addr_len ) );
#else
        const socket_t peer = static_cast<socket_t>( ::accept(
                                  static_cast<int>( state.listen_sock ),
                                  reinterpret_cast<sockaddr *>( &addr ), &addr_len ) );
#endif
        if( peer == static_cast<socket_t>( -1 ) ) {
            break;
        }

        // Read the HELLO frame immediately to gate acceptance.
        frame op;
        std::vector<char> payload;
        if( !recv_frame( peer, op, payload ) || op != frame::HELLO ) {
            close_socket( peer );
            continue;
        }
        reader r;
        r.data = payload.data();
        r.size = payload.size();
        const uint16_t proto_hi = r.get_u16();
        const std::string name = r.get_str();
        if( !r.ok || proto_hi != /* NETPLAY_PROTOCOL */ 0x0004 ) {
            // deny
            std::vector<char> d;
            put_str( d, "Protocol mismatch; DDA MP 0.1 expects protocol 4" );
            send_all( peer, encode_frame( frame::DENY, d ) );
            close_socket( peer );
            continue;
        }
        const std::string client_game = r.get_str();
        const std::string client_netplay = r.get_str();
        if( !r.ok || client_game != VERSION || client_netplay != NETPLAY_VERSION ) {
            std::vector<char> d;
            put_str( d, string_format(
                         "Data version mismatch; host game=%s netplay=%s, client game=%s netplay=%s",
                         VERSION, NETPLAY_VERSION, client_game.c_str(),
                         client_netplay.c_str() ) );
            send_all( peer, encode_frame( frame::DENY, d ) );
            close_socket( peer );
            continue;
        }

        int sid;
        {
            std::lock_guard<std::mutex> lk( state.sessions_mutex );
            sid = ++ state.session_watermark;
            state.id2sock[sid] = peer;
            state.id2name[sid] = name;
        }
        // DDA MP v4.0a-223: a peer that stopped reading must not block the
        // host's main thread forever. Once the send buffer fills, send() fails
        // after this timeout and the heartbeat drops the session.
#ifdef _WIN32
        {
            const DWORD snd_timeout = 5000;
            ::setsockopt( static_cast<SOCKET>( peer ), SOL_SOCKET, SO_SNDTIMEO,
                          reinterpret_cast<const char *>( &snd_timeout ),
                          sizeof( snd_timeout ) );
        }
#else
        {
            timeval snd_timeout{ 5, 0 };
            ::setsockopt( peer, SOL_SOCKET, SO_SNDTIMEO, &snd_timeout,
                          sizeof( snd_timeout ) );
        }
#endif
        std::vector<char> w;
        put_u16( w, /* NETPLAY_PROTOCOL */ 0x0004 );
        put_u32( w, static_cast<uint32_t>( sid ) );
        // DDA MP fat client: announce the host's active world so the client
        // can load the same mod data before rendering snapshots.
        std::string wname;
        std::vector<std::string> wmods;
        std::vector<std::pair<std::string, std::string>> wmod_versions;
#ifdef _WIN32
        if( world_generator != nullptr && world_generator->active_world != nullptr ) {
            wname = world_generator->active_world->world_name;
            for( const mod_id &m : world_generator->active_world->active_mod_order ) {
                wmods.push_back( m.str() );
                wmod_versions.emplace_back( m.str(), m->version );
            }
        }
#endif
        put_str( w, wname );
        // DDA MP v4.0a-122: the world's mod ids. A client without a local
        // copy of the world loads exactly these packs, so joining no longer
        // requires transferring (or manually copying) the save.
        put_u16( w, static_cast<uint16_t>(
                     std::min<std::size_t>( wmods.size(), 0xffff ) ) );
        for( std::size_t i = 0; i < wmods.size() && i < 0xffff; ++i ) {
            put_str( w, wmods[i] );
        }
        // DDA MP v4.0a-123: world_default options (season length, city size,
        // spawn rates...) so a virtual client world matches the host.
        std::vector<std::pair<std::string, std::string>> wopts;
#ifdef _WIN32
        if( world_generator != nullptr && world_generator->active_world != nullptr ) {
            for( const auto &entry : world_generator->active_world->WORLD_OPTIONS ) {
                if( entry.second.getPage() == "world_default" ) {
                    wopts.emplace_back( entry.first, entry.second.getValue() );
                }
            }
        }
#endif
        put_u16( w, static_cast<uint16_t>(
                     std::min<std::size_t>( wopts.size(), 0xffff ) ) );
        for( std::size_t i = 0; i < wopts.size() && i < 0xffff; ++i ) {
            put_str( w, wopts[i].first );
            put_str( w, wopts[i].second );
        }
        // DDA MP v4.0a-326: data manifest. The client validates its loaded mod
        // versions against this and reports its own hash after loading.
        put_str( w, VERSION );
        put_str( w, NETPLAY_VERSION );
        put_u16( w, static_cast<uint16_t>(
                     std::min<std::size_t>( wmod_versions.size(), 0xffff ) ) );
        for( std::size_t i = 0; i < wmod_versions.size() && i < 0xffff; ++i ) {
            put_str( w, wmod_versions[i].first );
            put_str( w, wmod_versions[i].second );
        }
        put_str( w, data_manifest_hash() );
        std::string wtileset;
        if( get_options().has_option( "TILES" ) ) {
            wtileset = get_option<std::string>( "TILES" );
        }
        put_str( w, wtileset );
        send_all( peer, encode_frame( frame::WELCOME, w ) );

        // Every accepted session now has a dedicated reader thread that
        // harvests KEYS frames into the host queue until disconnect.
        {
            std::lock_guard<std::mutex> lk( state.sessions_mutex );
            state.id2reader[sid] = std::thread( [sid, peer] {
                host_session_reader( sid, peer );
            } );
        }
    }
}

bool host_start( int port )
{
    if( state.running ) {
        return true;
    }

    socket_t sock;
#ifdef _WIN32
    sock = static_cast<socket_t>( ::socket( AF_INET, SOCK_STREAM, 0 ) );
#else
    sock = static_cast<socket_t>( ::socket( AF_INET, SOCK_STREAM, 0 ) );
#endif
    if( sock <= 0 ) {
        return false;
    }

    sockaddr_in addr;
    std::memset( &addr, 0, sizeof( addr ) );
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY; // Listen on all interfaces, like MC "open to LAN".
    addr.sin_port = ::htons( static_cast<uint16_t>( port ) );

#ifdef _WIN32
    if( ::bind( static_cast<SOCKET>( sock ), reinterpret_cast<const sockaddr *>( &addr ),
                sizeof( addr ) ) != 0 ) {
        close_socket( sock );
        return false;
    }
    if( ::listen( static_cast<SOCKET>( sock ), 8 ) != 0 ) {
        close_socket( sock );
        return false;
    }
#else
    if( ::bind( static_cast<int>( sock ), reinterpret_cast<const sockaddr *>( &addr ),
                sizeof( addr ) ) != 0 ) {
        close_socket( sock );
        return false;
    }
    if( ::listen( static_cast<int>( sock ), 8 ) != 0 ) {
        close_socket( sock );
        return false;
    }
#endif

    state.listen_sock = sock;
    state.listen_port = port;
    state.running = true;
    state.accept_thread = std::thread( [] { host_accept_loop(); } );

    return true;
}

void host_stop()
{
    set_long_wait( false );
    // DDA MP v4.0a-180: no assistance survives the session.
    host_help_shutdown();
    // DDA MP v4.0a-142: split regions own extra maps and creature stores.
    // Fold them back into the host world before the transport goes away.
    host_region_shutdown();
    state.stop_host();
}

bool host_is_running()
{
    return state.running;
}

int host_listen_port()
{
    return state.listen_port;
}

int host_session_count()
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    return static_cast<int>( state.id2sock.size() );
}

std::vector<std::string> host_session_names()
{
    std::vector<std::string> names;
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    for( const auto &[id, name] : state.id2name ) {
        ( void )id;
        names.push_back( name );
    }
    return names;
}

std::string host_session_name( int sid )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2name.find( sid );
    return it != state.id2name.end() ? it->second : std::string();
}

/* DDA MP v4.0a-223: silent-session heartbeat. Production waits 30 s for
 * input before a PING and another 30 s for the answer; the automated tests
 * shorten both with DDA_MP_TEST_HEARTBEAT_MS. */
static std::chrono::milliseconds hb_interval()
{
    static const std::chrono::milliseconds cached = []() {
        if( const char *ms = std::getenv( "DDA_MP_TEST_HEARTBEAT_MS" ) ) {
            const long long v = std::atoll( ms );
            if( v >= 100 && v <= 600000 ) {
                return std::chrono::milliseconds( v );
            }
        }
        return std::chrono::milliseconds( 30000 );
    }();
    return cached;
}

static bool host_send_ping_to_sid( int sid )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    return send_all( it->second, encode_frame( frame::PING, {} ) );
}

void host_drop_session( int sid )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return;
    }
    // The reader thread owns the handle and unregisters the session itself.
    // shutdown() only makes its blocking recv return, which runs the usual
    // close path; erasing here would make the reader skip close_socket().
#ifdef _WIN32
    ::shutdown( static_cast<SOCKET>( it->second ), SD_BOTH );
#else
    ::shutdown( it->second, SHUT_RDWR );
#endif
}

void host_kick_session( int sid, const std::string &reason )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return;
    }
    std::vector<char> payload;
    put_str( payload, reason );
    // Best effort: the client may already be gone. The reader thread still
    // owns the close and unregisters the session.
    send_all( it->second, encode_frame( frame::BYE, payload ) );
#ifdef _WIN32
    ::shutdown( static_cast<SOCKET>( it->second ), SD_BOTH );
#else
    ::shutdown( it->second, SHUT_RDWR );
#endif
}

bool client_kicked()
{
    return state.client_kicked_flag;
}

std::string client_kick_reason()
{
    return state.client_kick_reason_text;
}

std::vector<int> host_heartbeat_tick( const std::function<bool( int )> &busy )
{
    std::vector<int> dead;
    if( !host_is_running() ) {
        return dead;
    }
    const auto now = std::chrono::steady_clock::now();
    const auto interval = hb_interval();
    for( int sid : host_session_ids() ) {
        const bool is_busy = busy && busy( sid );
        bool send_ping = false;
        bool give_up = false;
        {
            std::lock_guard<std::mutex> lk( state.hb_mutex );
            if( state.session_dropping.count( sid ) > 0 ) {
                continue; // already reported, the reader is closing
            }
            const auto it = state.session_last_rx.find( sid );
            if( it == state.session_last_rx.end() ) {
                state.session_last_rx[sid] = now;
                continue;
            }
            if( is_busy ) {
                // Sleep, activity, helping or long wait: no input expected,
                // so restart the idle timer instead of pinging.
                it->second = now;
                state.session_ping_sent.erase( sid );
                continue;
            }
            if( now - it->second < interval ) {
                continue;
            }
            const auto ping_it = state.session_ping_sent.find( sid );
            if( ping_it == state.session_ping_sent.end() ) {
                state.session_ping_sent[sid] = now;
                send_ping = true;
            } else if( now - ping_it->second >= interval ) {
                state.session_ping_sent.erase( sid );
                state.session_last_rx.erase( it );
                state.session_dropping[sid] = now;
                give_up = true;
            }
        }
        if( send_ping ) {
            host_send_ping_to_sid( sid );
            if( mp_diag_enabled() ) {
                if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                    std::fprintf( f, "hb ping sid=%d\n", sid );
                    std::fclose( f );
                }
            }
        } else if( give_up ) {
            if( mp_diag_enabled() ) {
                if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                    std::fprintf( f, "hb timeout sid=%d\n", sid );
                    std::fclose( f );
                }
            }
            dead.push_back( sid );
        }
    }
    return dead;
}

std::string host_listen_address()
{
    if( !state.running ) {
        return {};
    }
    sockaddr_in addr;
    socklen_t len = static_cast<socklen_t>( sizeof( addr ) );
    std::memset( &addr, 0, sizeof( addr ) );
#ifdef _WIN32
    if( ::getsockname( static_cast<SOCKET>( state.listen_sock ),
                       reinterpret_cast<sockaddr *>( &addr ), &len ) != 0 ) {
        return {};
    }
#else
    if( ::getsockname( static_cast<int>( state.listen_sock ),
                       reinterpret_cast<sockaddr *>( &addr ), &len ) != 0 ) {
        return {};
    }
#endif
    char ip[64] = {};
    if( ::inet_ntop( AF_INET, &addr.sin_addr, ip, sizeof( ip ) ) == nullptr ) {
        return {};
    }
    return std::string( ip ) + ":" + std::to_string( ::ntohs( addr.sin_port ) );
}

void host_pump()
{
    // No-op here: sessions accept on their own thread. The authoritative
    // gameplay glue (avatar registration, snapshots, input routing) hooks in via
    // the game-side glue (see netplay/hostgame hooks).
}

/* ------------------------------------------------------------------ */
/* payload compression (zstd)                                          */
/* ------------------------------------------------------------------ */

static constexpr size_t SNAP_MAX_UNCOMPRESSED = 64u * 1024u * 1024u;

/// Wrap a payload into: [1 byte tag][body]. tag 0 = raw body, tag 1 = zstd.
static std::vector<char> maybe_compress( const std::vector<char> &raw )
{
    std::vector<char> out( 1 + raw.size() );
    const size_t bound = raw.empty() ? 0 : ZSTD_compressBound( raw.size() );
    bool compressed = false;
    if( !raw.empty() ) {
        out.resize( 1 + bound );
        const size_t n = ZSTD_compress( out.data() + 1, bound, raw.data(), raw.size(), 3 );
        if( !ZSTD_isError( n ) && 1 + n < raw.size() ) {
            out[0] = 1;
            out.resize( 1 + n );
            compressed = true;
        }
    }
    if( !compressed ) {
        out[0] = 0;
        std::copy( raw.begin(), raw.end(), out.begin() + 1 );
        out.resize( 1 + raw.size() );
    }
    return out;
}

/// Inverse of maybe_compress; empty result means "unusable payload".
static std::vector<char> maybe_decompress( const char *data, size_t size )
{
    if( size < 1 ) {
        return {};
    }
    const uint8_t tag = static_cast<uint8_t>( data[0] );
    if( tag == 0 ) {
        return std::vector<char>( data + 1, data + size );
    }
    if( tag != 1 ) {
        return {};
    }
    const size_t size_hint = ZSTD_getFrameContentSize( data + 1, size - 1 );
    if( size_hint == ZSTD_CONTENTSIZE_ERROR ||
        size_hint == ZSTD_CONTENTSIZE_UNKNOWN || size_hint > SNAP_MAX_UNCOMPRESSED ) {
        return {};
    }
    std::vector<char> out( size_hint );
    const size_t got = ZSTD_decompress( out.data(), out.size(), data + 1, size - 1 );
    if( ZSTD_isError( got ) ) {
        return {};
    }
    out.resize( got );
    return out;
}

static std::vector<char> encode_snapshot_payload_raw( const snapshot &snap )
{
    std::vector<char> p;
    put_u16( p, static_cast<uint16_t>( snap.width ) );
    put_u16( p, static_cast<uint16_t>( snap.height ) );
    put_str32( p, snap.status_line );
    // DDA MP v4.2: host clock/weather for the client's native renderer.
    p.push_back( static_cast<char>( snap.has_time ? 1 : 0 ) );
    put_i32( p, snap.turn );
    put_str32( p, snap.weather );
    put_str32( p, snap.vehicle_json );
    put_i32( p, snap.veh_ax );
    put_i32( p, snap.veh_ay );
    put_i32( p, snap.veh_az );
    p.push_back( static_cast<char>( snap.has_vehicle_info ? 1 : 0 ) );
    p.push_back( static_cast<char>( snap.avatar_facing ) );
    p.push_back( static_cast<char>( snap.avatar_controlling ? 1 : 0 ) );
    put_i32( p, snap.oxygen );
    put_i32( p, snap.oxygen_max );
    p.push_back( static_cast<char>( snap.has_vehicle_status ? 1 : 0 ) );
    if( snap.has_vehicle_status ) {
        put_str8( p, snap.veh_azimuth );
        put_str8( p, snap.veh_cruise );
        put_str8( p, snap.veh_fuel );
        put_str8( p, snap.veh_battery );
        p.push_back( static_cast<char>( snap.veh_cruise_color ) );
        p.push_back( static_cast<char>( snap.veh_fuel_color ) );
        p.push_back( static_cast<char>( snap.veh_battery_color ) );
        p.push_back( static_cast<char>( snap.veh_tracking_on ? 1 : 0 ) );
        p.push_back( static_cast<char>( snap.veh_has_horn ? 1 : 0 ) );
        p.push_back( static_cast<char>( snap.veh_has_indicator ? 1 : 0 ) );
        if( snap.veh_has_indicator ) {
            put_i32( p, snap.veh_indicator_next_dx );
            put_i32( p, snap.veh_indicator_next_dy );
            put_i32( p, snap.veh_indicator_face_dx );
            put_i32( p, snap.veh_indicator_face_dy );
        }
    }
    put_u16( p, static_cast<uint16_t>( snap.creatures.size() ) );
    for( const snapshot::creature_frame &cf : snap.creatures ) {
        p.push_back( static_cast<char>( cf.kind ) );
        put_i32( p, cf.ax );
        put_i32( p, cf.ay );
        put_i32( p, cf.az );
        p.push_back( static_cast<char>( cf.facing ) );
        put_str32( p, cf.json );
    }
    for( const cell &c : snap.cells ) {
        put_u32( p, static_cast<uint32_t>( c.glyph ) );
        put_i32( p, static_cast<int32_t>( c.color ) );
    }
    // DDA MP v4.0a-239: item-descriptor section, placed before the optional
    // sprite payload so the existing exact-fit tile ladder still works.
    if( !snap.item_tiles.empty() ) {
        put_u32( p, 0x4D504954u ); // "MPIT"
        put_u16( p, static_cast<uint16_t>( std::min<size_t>( snap.item_tiles.size(), 65535 ) ) );
        for( const snapshot::item_tile &it : snap.item_tiles ) {
            put_u32( p, static_cast<uint32_t>( it.index ) );
            p.push_back( static_cast<char>( std::min<size_t>( it.items.size(), 255 ) ) );
            for( const snapshot::item_desc &d : it.items ) {
                put_str8( p, d.type );
                put_str8( p, d.variant );
                put_str8( p, d.mtype );
                put_u16( p, static_cast<uint16_t>( std::max( 0, std::min( 65535, d.count ) ) ) );
                put_u32( p, static_cast<uint32_t>( std::max( 0, std::min( 1000000, d.damage ) ) ) );
                put_u32( p, static_cast<uint32_t>( std::max( 0, d.seed ) ) );
                p.push_back( static_cast<char>( d.filthy ? 1 : 0 ) );
                p.push_back( static_cast<char>( d.corpse_flags ) );
            }
        }
    }
    // DDA MP v4.0a-248: vehicle parts in the visible range (magic "MPVP").
    if( !snap.vehicle_parts.empty() ) {
        put_u32( p, 0x5056504Du ); // "MPVP" in LE byte order
        put_u16( p, static_cast<uint16_t>( std::min<size_t>( snap.vehicle_parts.size(), 65535 ) ) );
        for( const snapshot::vehicle_part_frame &vp : snap.vehicle_parts ) {
            put_i32( p, vp.ax );
            put_i32( p, vp.ay );
            put_i32( p, vp.az );
            put_str8( p, vp.id );
            put_str8( p, vp.variant );
            p.push_back( static_cast<char>( vp.sub ) );
            p.push_back( static_cast<char>( vp.rot ) );
            p.push_back( static_cast<char>( vp.memory ? 1 : 0 ) );
        }
    }
    // Optional tileset replay payload. Append only when it matches 1:1 with
    // the ascii cells; legacy (shorter) payloads end here, and the reader
    // detects the extension by remaining bytes (backwards compatible).
    if( snap.tiles.size() == snap.cells.size() && !snap.tiles.empty() ) {
        put_u16( p, static_cast<uint16_t>( snap.tiles.size() ) );
        for( const tile_cell &t : snap.tiles ) {
            p.push_back( static_cast<char>( t.light ) );
            put_str8( p, t.ter );
            put_str8( p, t.furn );
            put_str8( p, t.trap );
            put_str8( p, t.critter );
            put_str8( p, t.item );
            put_str8( p, t.vpart );
            // v3.2(+3): per-cell multi-tile appearance values (6 bytes),
            // v3.5: flags byte (bit3 barrier, bit4 light color valid) and
            // the smooth-lighting light texel color (3 bytes where
            // flags & 16), v3.8: critter appearance (facing flip byte +
            // attitude badge byte + the ';' separated overlay draw id list,
            // 16-bit length; variable length strings keep the reader
            // self-describing). The reader detects the extension by trying
            // to consume the whole section with the longest extras first,
            // then falls back (backwards compatible both directions).
            p.push_back( static_cast<char>( t.ter_sub ) );
            p.push_back( static_cast<char>( t.ter_rot ) );
            p.push_back( static_cast<char>( t.furn_sub ) );
            p.push_back( static_cast<char>( t.furn_rot ) );
            p.push_back( static_cast<char>( t.vpart_sub ) );
            p.push_back( static_cast<char>( t.vpart_rot ) );
            p.push_back( static_cast<char>( t.flags ) );
            p.push_back( static_cast<char>( t.light_r ) );
            p.push_back( static_cast<char>( t.light_g ) );
            p.push_back( static_cast<char>( t.light_b ) );
            p.push_back( static_cast<char>( t.critter_rot ) );
            p.push_back( static_cast<char>( t.critter_badge ) );
            put_str32( p, t.critter_overlays );
            // v3.9.1: item itype variant (draw_item draws items with
            // itype_variant().id; tilesets resolve "id_var_<variant>").
            put_str8( p, t.item_variant );
            // v3.9.1: item-layer sprites the tileset draws on this cell
            // ("layering.json" contexts; the skip-top-item fact rides on
            // flags bit 6 with the other flags).
            p.push_back( static_cast<char>(
                             t.item_layers.size() < 255 ? t.item_layers.size() : 255 ) );
            for( const tile_item_layer &l : t.item_layers ) {
                put_str8( p, l.sprite );
                put_str8( p, l.variant );
                p.push_back( static_cast<char>( l.dx ) );
                p.push_back( static_cast<char>( l.dy ) );
            }
            // v4.0a-2: the avatar's own heard-sound marker on this cell
            // ("footstep" / "footstep_above" / "footstep_below" overlay id
            // on the client; the ascii cells carry the same glyph anyway).
            p.push_back( static_cast<char>( t.marker ) );
            // v4.0a-114: raw field (sprite + intensity) and the viewer's map
            // memory for this cell. Fields were previously folded into the
            // ascii cells only, so the native renderer never drew fire/smoke;
            // memory content was streamed as live terrain, which the native
            // path ignores for out-of-sight cells (it reads map memory).
            put_str8( p, t.field );
            p.push_back( static_cast<char>( t.field_intensity ) );
            put_str8( p, t.mem_ter );
            put_str8( p, t.mem_dec );
            p.push_back( static_cast<char>( t.mem_ter_sub ) );
            p.push_back( static_cast<char>( t.mem_ter_rot ) );
            p.push_back( static_cast<char>( t.mem_dec_sub ) );
            p.push_back( static_cast<char>( t.mem_dec_rot ) );
            put_u32( p, t.mem_sym );
            // v4.0a-135: graffiti text.
            put_str32( p, t.graffiti );
        }
        // DDA MP v4.0a-4: absolute world origin of the frame's pivot cell,
        // written after the cell loop. The reader's exact-fit ladder decodes
        // this 12-byte trailer first; legacy payloads fall through as before.
        if( snap.has_origin ) {
            put_i32( p, snap.origin_x );
            put_i32( p, snap.origin_y );
            put_i32( p, snap.origin_z );
            // DDA MP v4.0a-232: view offset from the viewer avatar, so the
            // client can scroll the camera in look/fire mode and place the
            // reticle exactly. Decoded by the exact-fit ladder in the reader.
            put_u16( p, static_cast<uint16_t>( snap.view_dx + 32768 ) );
            put_u16( p, static_cast<uint16_t>( snap.view_dy + 32768 ) );
        }
    }
    return p;
}

/* DDA MP v4.0a-240: record what a snapshot really costs. Estimates were only
 * guesses; the diagnostic log shows the raw payload and the framed size the
 * socket actually carries, per frame and per second. */
static void mp_log_snapshot_bytes( const char *who, const std::vector<char> &raw,
                                   const std::vector<char> &framed, size_t peers )
{
    if( !mp_diag_enabled() ) {
        return;
    }
    std::FILE *f = mp_diag_open( "netplay_snapshot.log" );
    if( f == nullptr ) {
        return;
    }
    static long long mp_snap_bucket = 0;
    static size_t mp_snap_noted = 0;
    static size_t mp_snap_sum_raw = 0;
    static size_t mp_snap_sum_framed = 0;
    const long long wms = std::chrono::duration_cast<std::chrono::milliseconds>(
                              std::chrono::system_clock::now().time_since_epoch() ).count();
    mp_snap_noted++;
    mp_snap_sum_raw += raw.size();
    mp_snap_sum_framed += framed.size();
    const long long bucket = wms / 1000;
    if( bucket != mp_snap_bucket ) {
        if( mp_snap_bucket != 0 && mp_snap_noted > 0 ) {
            std::fprintf( f, "snapshot sec=%lld frames=%zu raw_avg=%zu framed_avg=%zu peers=%zu\n",
                          mp_snap_bucket, mp_snap_noted,
                          mp_snap_sum_raw / mp_snap_noted,
                          mp_snap_sum_framed / mp_snap_noted, peers );
        }
        mp_snap_bucket = bucket;
        mp_snap_noted = 0;
        mp_snap_sum_raw = 0;
        mp_snap_sum_framed = 0;
    }
    std::fprintf( f, "%s bytes=%zu framed=%zu peers=%zu\n", who, raw.size(), framed.size(), peers );
    std::fclose( f );
}

void host_broadcast_snapshot( const snapshot &snap )
{
    const std::vector<char> mp_raw = encode_snapshot_payload_raw( snap );
    std::vector<char> f = encode_frame( frame::SNAPSHOT, maybe_compress( mp_raw ) );
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    mp_log_snapshot_bytes( "broadcast", mp_raw, f, state.id2sock.size() );
    auto it = state.id2sock.begin();
    while( it != state.id2sock.end() ) {
        if( send_all( it->second, f ) ) {
            ++it;
        } else {
            // failed session: drop it; its reader thread will notice too
            close_socket( it->second );
            it = state.id2sock.erase( it );
        }
    }
}

/* DDA MP v4.0a-2 ------------------------------------------------- */

bool host_pop_sid_key( sid_key &out )
{
    std::lock_guard<std::mutex> lk( state.keys_mutex );
    if( state.avatar_keys.empty() ) {
        return false;
    }
    // lowest session id first: each session owns at most one slot, so this
    // is just a stable drain order
    out = state.avatar_keys.begin()->second;
    state.avatar_keys.erase( state.avatar_keys.begin() );
    if( mp_diag_enabled() ) {
        const long long wms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch() ).count();
        if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
            std::fprintf( f, "srv pop wms=%lld sid=%d code=%u\n", wms, out.sid, out.code );
            std::fclose( f );
        }
    }
    return true;
}

/* DDA MP v4.0a-2: return a key the pump inspected but did not execute (the
 * session's move pool was exhausted). Drops the inspect step, does not
 * regenerate it. */
void host_put_back_sid_key( const sid_key &in )
{
    std::lock_guard<std::mutex> lk( state.keys_mutex );
    state.avatar_keys.emplace( in.sid, in );
}

/* DDA MP v4.0a-2: whether the reader thread still holds avatar steps. The
 * host keeps the reality bubble on the acting avatar while a burst is in
 * progress and hands it back to the host player only between bursts. */
bool host_avatar_keys_pending()
{
    std::lock_guard<std::mutex> lk( state.keys_mutex );
    return !state.avatar_keys.empty();
}

void host_push_legacy_key( uint32_t code )
{
    std::lock_guard<std::mutex> lk( state.keys_mutex );
    if( state.host_keys.size() < 256 ) {
        state.host_keys.push_back( code );
    }
}

std::vector<int> host_session_ids()
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    std::vector<int> out;
    out.reserve( state.id2sock.size() );
    for( const auto &[id, sock] : state.id2sock ) {
        ( void )sock;
        out.push_back( id );
    }
    return out;
}

static void mp_mem_probe( int sid, const snapshot &snap );

bool host_send_snapshot_to_sid( int sid, const snapshot &snap )
{
    // NOTE (v4.0a-226): sends run under sessions_mutex. With the 5 s
    // SO_SNDTIMEO a stuck peer can delay every other send by up to 5 s.
    // Accepted for host + 1 client; move sends to a per-session writer before
    // scaling.
    // NOTE (v4.0a-226): a send failure drops the session silently. Planned
    // fix: route it through a shared host_end_session() so the chat notice,
    // heartbeat bookkeeping and reader-owned close are not skipped.
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    mp_mem_probe( sid, snap );
    const std::vector<char> mp_raw = encode_snapshot_payload_raw( snap );
    std::vector<char> f = encode_frame( frame::SNAPSHOT, maybe_compress( mp_raw ) );
    {
        char mp_who[64];
        std::snprintf( mp_who, sizeof( mp_who ), "direct sid=%d", sid );
        mp_log_snapshot_bytes( mp_who, mp_raw, f, 1 );
    }
    if( !send_all( it->second, f ) ) {
        // failed session: drop it; its reader thread will notice too
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool host_send_hover_to_sid( int sid, const std::string &text )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_str( p, text );
    if( !send_all( it->second, encode_frame( frame::HOVER, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

/* DDA MP v4.0a-202: fork-only UI strings are not in the game catalogs. The
 * host records every fork_text pair it emits so a plain message can be turned
 * back into its English msgid + arguments before it goes over the wire. */
struct fork_text_entry {
    std::string msgid;
    std::string text;
};
static std::vector<fork_text_entry> fork_text_entries;
static std::mutex fork_text_mutex;

static void fork_text_register( const std::string &msgid, const std::string &text )
{
    if( msgid.empty() || text.empty() ) {
        return;
    }
    std::lock_guard<std::mutex> lk( fork_text_mutex );
    for( fork_text_entry &entry : fork_text_entries ) {
        if( entry.msgid == msgid ) {
            entry.text = text;
            return;
        }
    }
    fork_text_entries.push_back( { msgid, text } );
}

/* DDA MP v4.0a-256: host memory black box for the pulping/butchery leak.
 * Writes a line every 5 s while snapshots are flowing: process working set,
 * fork-text table size and the item/vehicle/creature volume of the frame.
 * If the working set grows while the snapshot counters stay flat, the leak is
 * not the item stream. */
static std::size_t mp_process_working_set()
{
#ifdef _WIN32
    using get_mem_t = BOOL ( WINAPI * )( HANDLE, PROCESS_MEMORY_COUNTERS *, DWORD );
    static get_mem_t get_mem = reinterpret_cast<get_mem_t>(
                                   GetProcAddress( GetModuleHandleA( "kernel32.dll" ),
                                           "K32GetProcessMemoryInfo" ) );
    if( get_mem != nullptr ) {
        PROCESS_MEMORY_COUNTERS pmc{};
        if( get_mem( GetCurrentProcess(), &pmc, sizeof( pmc ) ) ) {
            return static_cast<std::size_t>( pmc.WorkingSetSize );
        }
    }
#endif
    return 0;
}

static void mp_mem_probe( int sid, const snapshot &snap )
{
    static auto last = std::chrono::steady_clock::now() - std::chrono::seconds( 30 );
    const auto now = std::chrono::steady_clock::now();
    if( now - last < std::chrono::seconds( 5 ) ) {
        return;
    }
    last = now;
    std::size_t fork_count = 0;
    {
        std::lock_guard<std::mutex> lk( fork_text_mutex );
        fork_count = fork_text_entries.size();
    }
    std::size_t items = 0;
    for( const snapshot::item_tile &it : snap.item_tiles ) {
        items += it.items.size();
    }
    std::FILE *f = std::fopen( ( PATH_INFO::config_dir() + "netplay_mem.log" ).c_str(), "a" );
    if( f == nullptr ) {
        return;
    }
    std::fprintf( f,
                  "%lld ws=%zu fork=%zu sid=%d items=%zu itemtiles=%zu veh=%zu creatures=%zu\n",
                  static_cast<long long>( std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch() ).count() ),
                  mp_process_working_set(), fork_count, sid, items,
                  snap.item_tiles.size(), snap.vehicle_parts.size(), snap.creatures.size() );
    std::fclose( f );
}

static bool capture_fork_message( const std::string &text, std::string &msgid,
                                  std::vector<std::string> &args );

std::string fork_text( const char *msgid )
{
    if( msgid == nullptr ) {
        return std::string();
    }
    const std::string lang = TranslationManager::GetInstance().GetCurrentLanguage();
    if( lang.rfind( "ru", 0 ) == 0 ) {
        // The fork's own UI strings, kept here because the game's catalogs are
        // generated from the upstream sources and have no entries for them.
        static const std::unordered_map<std::string, std::string> ru = {
            { "%s connected.", "Игрок %s подключился." },
            { "%s lost connection.", "Игрок %s потерял связь." },
            { "%s gives you the %s.", "%s даёт вам %s." },
            { "< <color_green>Aiming</color> >", "< <color_green>Прицеливание</color> >" },
            { "< <color_green>Throwing</color> >", "< <color_green>Бросок</color> >" },
            { "<ENTER>: start listener on port 24224", "<ENTER>: запустить сервер на порту 24224" },
            { "<ENTER>: stop listener", "<ENTER>: остановить сервер" },
            { "<UP>/<DOWN>: select   <ENTER>: connect   <ESC>: cancel", "<ВВЕРХ>/<ВНИЗ>: выбор   <ENTER>: подключиться   <ESC>: отмена" },
            { "Bad avatar profile: %s", "Некорректный профиль персонажа: %s" },
            { "Cata server", "Сервер Cata" },
            { "Chat message", "Сообщение в чат" },
            { "Connection refused: %s", "В подключении отказано: %s" },
            { "Connection refused: host unreachable.", "В подключении отказано: хост недоступен." },
            { "Could not connect: the host rejected the connection.", "Не удалось подключиться: хост отклонил соединение." },
            { "Could not reconnect: the host is unreachable.", "Не удалось переподключиться: хост недоступен." },
            { "Craft something first.", "Сначала что-нибудь создайте." },
            { "Disassembly is not available for netplay clients yet.", "Разбор предметов пока недоступен клиентам сетевой игры." },
            { "Disconnect from server", "Отключиться от сервера" },
            { "The host removed this client: bind a character before sending game input.",
              "Хост отключил клиента: создайте или выберите персонажа перед отправкой игровых команд." },
            { "Netplay diagnostics for this host session",
              "Диагностика netplay для этого хостинга" },
            { "Start without diagnostics (recommended)",
              "Запустить без диагностики (рекомендуется)" },
            { "Start with debug logging",
              "Запустить с отладочными логами" },
            { "Failed to bind the listener. Port %d may be in use.", "Не удалось занять порт. Возможно, порт %d уже используется." },
            { "Firing mode: %s", "Режим стрельбы: %s" },
            { "HP: %d/%d | %s | %s", "ОЗ: %d/%d | %s | %s" },
            { "Host is not running a game yet.", "Хост ещё не запустил игру." },
            { "Invalid nickname.", "Недопустимое имя." },
            { "Invalid target.", "Недопустимая цель." },
            { "Item hotkeys assigned: %d/%d", "Назначено горячих клавиш: %d/%d" },
            { "Items: %d", "Предметов: %d" },
            { "Long wait mode OFF", "Режим долгого ожидания выключен" },
            { "Long wait mode ON", "Режим долгого ожидания включён" },
            { "Long wait mode: OFF (pass turns for other players)", "Режим долгого ожидания: ВЫКЛ (пропускать ходы за других игроков)" },
            { "Long wait mode: ON (pass turns for other players)", "Режим долгого ожидания: ВКЛ (пропускать ходы за других игроков)" },
            { "Longest side", "Длинная сторона" },
            { "Network: open world for co-op (port 24224)", "Сеть: открыть мир для совместной игры (порт 24224)" },
            { "No free tile near the host avatar to spawn in.", "Рядом с персонажем хоста нет свободной клетки для появления." },
            { "Perception", "Восприятие" },
            { "Players (%d):", "Игроки (%d):" },
            { "Protocol: DDA MP 0.1  Port: %d", "Протокол: DDA MP 0.1  Порт: %d" },
            { "That item moved. Reopen the inventory and try again.", "Этот предмет переместился. Откройте инвентарь заново и повторите." },
            { "The host address can not be empty.", "Адрес хоста не может быть пустым." },
            { "The inventory changed. Reopen it and try again.", "Инвентарь изменился. Откройте его заново и повторите." },
            { "The way is blocked.", "Путь заблокирован." },
            { "There is no one close enough to give it to.", "Рядом нет никого, кому это можно передать." },
            { "There's nothing to smash here.", "Здесь нечего ломать." },
            { "This vehicle has no working controls.", "У этого транспорта нет работающего управления." },
            { "Unknown recipe.", "Неизвестный рецепт." },
            { "Volume", "Объём" },
            { "Waiting for other players...", "Ожидание других игроков…" },
            { "You are in a vehicle. Use the vehicle controls to get out.", "Вы в транспорте. Чтобы выйти, используйте управление транспортом." },
            { "You are not wielding a gun.", "Вы не держите огнестрельное оружие." },
            { "You are not wielding a multi-mode gun.", "Вы не держите оружие с несколькими режимами стрельбы." },
            { "You can't carry anything more.", "Вы больше не можете ничего нести." },
            { "You can't drive here.", "Здесь нельзя ехать." },
            { "You can't read that.", "Вы не можете это читать." },
            { "You can't throw there.", "Туда нельзя бросить." },
            { "You can't wield that.", "Вы не можете это взять в руки." },
            { "You get out of the vehicle.", "Вы выходите из транспорта." },
            { "You go down the stairs.", "Вы спускаетесь по лестнице." },
            { "You go up the stairs.", "Вы поднимаетесь по лестнице." },
            { "You have died. Disconnect and join again to create a new character.", "Вы погибли. Отключитесь и подключитесь снова, чтобы создать нового персонажа." },
            { "You have no compatible ammo for that.", "У вас нет подходящих патронов." },
            { "[%d] Listening on %d", "[%d] Приём подключений на порту %d" },
            { "[Esc] close   [Up/Down] select   [Tab] column   [PgUp/PgDn] page", "[Esc] закрыть   [Вверх/Вниз] выбор   [Tab] столбец   [PgUp/PgDn] страница" },
            { "[Esc] close", "[Esc] закрыть" },
            { "Map", "Карта" },
            { "No map data yet.", "Данных для карты пока нет." },
            { "[x] Not listening", "[x] Сервер не запущен" },
            { "Construct what?", "Что построить?" },
            { "Construct where?", "Где строить?" },
            { "Construction", "Строительство" },
            { "You can't construct there.", "Здесь нельзя строить." },
            { "You can't construct that here.", "Здесь это построить нельзя." },
            { "There's already a construction in progress here.",
              "Здесь уже идёт строительство." },
            { "Unknown construction \"%s\".", "Неизвестная конструкция \"%s\"." },
            { "Help the busy player", "Помочь занятому игроку" },
            { "Stop helping?", "Прекратить помощь?" },
            { "Player: %s", "Игрок: %s" },
            { "NPC: %s", "НПС: %s" },
            { "Talk", "Поговорить" },
            { "Missions", "Задания" },
            { "Available missions", "Доступные задания" },
            { "Mission journal", "Журнал заданий" },
            { "No missions available.", "Доступных заданий нет." },
            { "No active missions.", "Активных заданий нет." },
            { "Mission accepted: %s", "Задание принято: %s." },
            { "Turn in", "Сдать задание" },
            { "No missions to turn in.", "Нет заданий, готовых к сдаче." },
            { "Mission completed: %s", "Задание выполнено: %s." },
            { "Reward credit: %s", "Награда: %s" },
            { "Money: %s   Trader: %s", "Деньги: %s   У торговца: %s" },
            { "Vehicle", "Транспорт" },
            { "Vehicle status", "Состояние транспорта" },
            { "Refuel vehicle", "Заправить транспорт" },
            { "Vehicle parts", "Детали транспорта" },
            { "Zones", "Зоны" },
            { "No zones.", "Зон нет." },
            { "Grab", "Захват" },
            { "Release", "Отпустить" },
            { "North", "Север" },
            { "South", "Юг" },
            { "East", "Восток" },
            { "West", "Запад" },
            { "Northeast", "Северо-восток" },
            { "Northwest", "Северо-запад" },
            { "Southeast", "Юго-восток" },
            { "Southwest", "Юго-запад" },
            { "Movement mode", "Режим передвижения" },
            { "Walk", "Шагом" },
            { "Run", "Бегом" },
            { "Crouch", "Пригнувшись" },
            { "Prone", "Ползком" },
            { "Drop where?", "Куда выбросить?" },
            { "You can't drop there.", "Туда нельзя выбросить." },
            { "Suicide", "Самоубийство" },
            { "Abandon this character?", "Покинуть этого персонажа?" },
            { "This will kill your character. Continue?", "Это убьёт вашего персонажа. Продолжить?" },
            { "Spells", "Заклинания" },
            { "No spells.", "Заклинаний нет." },
            { "< <color_green>Casting</color> >", "< <color_green>Колдовство</color> >" },
            { "You are incapable of attacking!", "Вы не способны атаковать!" },
            { "No hostile creature in reach.  Waiting a turn.", "В пределах досягаемости нет врагов.  Ожидание хода." },
            { "You can't use reach attacks while forcing yourself to fight unarmed.", "Нельзя бить на расстоянии, сражаясь без оружия." },
            { "Select a damaged part to repair.", "Выберите повреждённую деталь для ремонта." },
            { "%s (damage %d)", "%s (повреждение %d)" },
            { "No damaged parts.", "Нет повреждённых деталей." },
            { "That part can't be repaired.", "Эту деталь нельзя отремонтировать." },
            { "You can't refuel a moving vehicle.", "Нельзя заправлять движущийся транспорт." },
            { "You have no fuel for this vehicle.", "У вас нет топлива для этого транспорта." },
            { "You are not in a vehicle.", "Вы не в транспорте." },
            { "Speed: %d mph   Cruise: %d mph", "Скорость: %d миль/ч   Крейсерская: %d миль/ч" },
            { "Engine: on", "Двигатель: включён" },
            { "Engine: off", "Двигатель: выключен" },
            { "Battery: %d/%d kJ", "Аккумулятор: %d/%d кДж" },
            { "Parts: %d   Damaged: %d", "Деталей: %d   Повреждено: %d" },
            { "Fuel: %s %d", "Топливо: %s %d" },
            { "Workout", "Тренировка" },
            { "Choose training intensity:", "Выберите интенсивность тренировки:" },
            { "Light", "Лёгкая" },
            { "Moderate", "Умеренная" },
            { "Active", "Активная" },
            { "High", "Высокая" },
            { "Train for how long (minutes): ", "Сколько тренироваться (минут): " },
            { "Trade", "Торговля" },
            { "Trade: buy", "Торговля: покупка" },
            { "Trade: sell", "Торговля: продажа" },
            { "Buy", "Купить" },
            { "Sell", "Продать" },
            { "Nothing to buy.", "Нечего купить." },
            { "Nothing to sell.", "Нечего продать." },
            { "You don't have enough money.", "У вас недостаточно денег." },
            { "The trader doesn't have enough money.", "У торговца недостаточно денег." },
            { "You buy %s for %s.", "Вы покупаете %s за %s." },
            { "You sell %s for %s.", "Вы продаёте %s за %s." },
            { "Quantity", "Количество" },
            { "That item moved. Reopen the trade.", "Предмет переместился. Откройте торговлю заново." },
            { "Conversation", "Разговор" },
            { "Conversation ended.", "Разговор окончен." },
            { "Look", "Осмотреть" },
            { "Help: %s", "Помочь: %s" },
            { "Close", "Закрыть" },
            { "Bionics", "Бионика" },
            { "Mutations", "Мутации" },
            { "Bionic power: %s", "Запас энергии бионики: %s" },
            { "No bionics installed.", "Установленной бионики нет." },
            { "No mutations.", "Мутаций нет." },
            { "passive", "пассивно" },
            { "active", "активно" },
            { "ready", "готово" },
            { "Hauling", "Волочение" },
            { "Haul everything here", "Волочить всё на этой клетке" },
            { "Stop hauling", "Прекратить волочить" },
            { "Enable autohaul", "Включить автоволочение" },
            { "Disable autohaul", "Выключить автоволочение" },
            { "Lost %d HP", "Потеряно %d HP" },
            { "Regained %d HP", "Восстановлено %d HP" },
            { "Hunger: %d", "Голод: %d" },
            { "Thirst: %d", "Жажда: %d" },
            { "Pain: %d", "Боль: %d" },
            { "Condition changed", "Состояние изменилось" },
            { "Read-only view: treatment is performed on the host.",
              "Только просмотр: лечение выполняется на хосте." },
            { "No body data available.", "Данные о теле ещё не получены." },
            { "butchering", "разделка туши" },
            { "crafting", "изготовление" },
            { "disassembly", "разбор" },
            { "construction", "строительство" },
            { "You help: %s.", "Вы помогаете: %s." },
            { "%s helps you.", "%s помогает вам." },
            { "You stop helping.", "Вы больше не помогаете." },
            { "%s stops helping you.", "%s больше вам не помогает." },
            { "There is no one nearby busy with a task you could help with.",
              "Рядом нет никого, кто занят делом, которому можно помочь." },
        };
        const auto it = ru.find( msgid );
        if( it != ru.end() ) {
            fork_text_register( msgid, it->second );
            return it->second;
        }
    }
    fork_text_register( msgid, msgid );
    return msgid;
}

bool host_send_event_to_sid( int sid, const std::string &text, int msg_type )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::string out_text = text;
    // A plain fork message is still a host-language sentence. Turn it back
    // into msgid + arguments so the receiving client can use its own language.
    if( !out_text.empty() &&
        static_cast<unsigned char>( out_text[0] ) >= 0x20 &&
        out_text.find( '\x1f' ) == std::string::npos &&
        out_text.find( '\x1e' ) == std::string::npos ) {
        std::string msgid;
        std::vector<std::string> args;
        const bool captured = capture_fork_message( out_text, msgid, args );
        if( captured ) {
            if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                std::fprintf( f, "event encode sid=%d msgid=%s\n", sid, msgid.c_str() );
                std::fclose( f );
            }
            out_text.clear();
            out_text += '\x01';
            out_text += msgid;
            for( const std::string &arg : args ) {
                out_text += '\x1f';
                out_text += arg;
            }
        }
    }
    std::vector<char> p;
    p.push_back( static_cast<char>( msg_type & 0xff ) );
    put_str( p, out_text );
    if( !send_all( it->second, encode_frame( frame::EVENT_TEXT, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

/* DDA MP v4.0a-165: formatted events. The host sends the untranslated msgid
 * plus its arguments; the client translates the msgid with its own catalog
 * and resolves typed name arguments from local game data. This keeps a
 * Russian client's log in Russian even when the host runs in English, without
 * shipping a host-localized (and therefore host-language) sentence. */

/* DDA MP v4.0a-167: arguments of engine messages arrive exactly as the sender
 * rendered them. Item and monster names are translation objects carrying a
 * JSON context ("stone", ...), so a context-less to_translation() would pick a
 * wrong word. The client keeps a lazily built index from the untranslated name
 * (what an English sender writes) to the name in the client's language, keyed
 * by both the singular and the plural form so stacks ("2 rocks") translate as
 * well. */
static const std::unordered_map<std::string, const translation *> &mp_name_index()
{
    static std::unordered_map<std::string, const translation *> index;
    static bool built = false;
    if( built ) {
        return index;
    }
    built = true;
    const auto add = []( const translation &tr ) {
        const std::string &raw = tr.msgid();
        if( raw.empty() || tr.translated( 1 ) == raw ) {
            // No translation for this name in the client's language.
            return;
        }
        index.emplace( raw, &tr );
        const std::string &raw_pl = tr.msgid_plural();
        if( raw_pl != raw ) {
            index.emplace( raw_pl, &tr );
        }
    };
    if( item_controller ) {
        for( const itype *t : item_controller->all() ) {
            if( t != nullptr ) {
                add( t->raw_name() );
            }
        }
    }
    for( const mtype &m : MonsterGenerator::generator().get_all_mtypes() ) {
        add( m.raw_name() );
    }
    return index;
}

static std::string mp_translate_name( const std::string &name, int count )
{
    const auto &index = mp_name_index();
    const auto it = index.find( name );
    if( it == index.end() ) {
        return name;
    }
    return it->second->translated( count > 1 ? count : 1 );
}

static std::string mp_translate_name_arg( const std::string &arg )
{
    if( arg.empty() ) {
        return arg;
    }
    // A stack is rendered as "<count> <name>"; keep the number and translate
    // the name with that count so the plural form is right.
    std::size_t digits = 0;
    while( digits < arg.size() && std::isdigit( static_cast<unsigned char>( arg[digits] ) ) ) {
        ++digits;
    }
    if( digits > 0 && digits < arg.size() && arg[digits] == ' ' ) {
        const int count = std::atoi( arg.substr( 0, digits ).c_str() );
        const std::string rest = arg.substr( digits + 1 );
        const std::string translated = mp_translate_name( rest, count );
        if( translated != rest ) {
            return arg.substr( 0, digits + 1 ) + translated;
        }
        return arg;
    }
    bool numeric = true;
    for( const char c : arg ) {
        if( !std::isdigit( static_cast<unsigned char>( c ) ) &&
            c != '.' && c != '-' && c != ',' && c != ' ' ) {
            numeric = false;
            break;
        }
    }
    if( numeric ) {
        return arg;
    }
    return mp_translate_name( arg, 1 );
}

static std::string mp_event_arg( const std::string &raw )
{
    if( raw.size() < 3 || raw[0] != '\x02' ) {
        return mp_translate_name_arg( raw );
    }
    const std::string id = raw.substr( 2 );
    switch( raw[1] ) {
        case 'T': {
            const ter_str_id ter( id );
            return ter.is_valid() ? ter->name() : id;
        }
        case 'F': {
            const furn_str_id furn( id );
            return furn.is_valid() ? furn->name() : id;
        }
        case 'I':
            return item::nname( itype_id( id ) );
        case 'M': {
            const mtype_id mon( id );
            return mon.is_valid() ? mon->nname() : id;
        }
        case 'N': {
            const mission_type_id mid( id );
            const mission_type *type = mid.is_valid() ? mission_type::get( mid ) : nullptr;
            return type != nullptr ? type->tname() : id;
        }
        case 'D': {
            const mission_type_id mid( id );
            const mission_type *type = mid.is_valid() ? mission_type::get( mid ) : nullptr;
            return type != nullptr ? type->description.translated() : id;
        }
        default:
            return raw;
    }
}

/// Substitute the arguments into a translated format string. Accepts both
/// plain ("%s") and positional ("%1$s") conversions, as the translation
/// catalogs use both.
static std::string mp_fill_args( const std::string &fmt,
                                 const std::vector<std::string> &args )
{
    std::string out;
    std::size_t next = 0;
    for( std::size_t i = 0; i < fmt.size(); ++i ) {
        if( fmt[i] != '%' || i + 1 >= fmt.size() ) {
            out += fmt[i];
            continue;
        }
        if( fmt[i + 1] == '%' ) {
            out += '%';
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        std::size_t pos = 0;
        bool has_pos = false;
        while( j < fmt.size() && std::isdigit( static_cast<unsigned char>( fmt[j] ) ) ) {
            pos = pos * 10 + static_cast<std::size_t>( fmt[j] - '0' );
            has_pos = true;
            ++j;
        }
        if( has_pos && j < fmt.size() && fmt[j] == '$' ) {
            ++j;
        } else {
            has_pos = false;
            j = i + 1;
        }
        // flags, width and precision
        while( j < fmt.size() &&
               ( std::strchr( "-+ #0.", fmt[j] ) != nullptr ||
                 std::isdigit( static_cast<unsigned char>( fmt[j] ) ) ) ) {
            ++j;
        }
        if( j < fmt.size() && std::strchr( "diuxXofsScgeE", fmt[j] ) != nullptr ) {
            const std::size_t idx = has_pos ? ( pos == 0 ? 0 : pos - 1 ) : next++;
            if( idx < args.size() ) {
                out += args[idx];
            }
            i = j;
            continue;
        }
        out += fmt[i];
    }
    return out;
}

/// Split a printf-style format string into the literal chunks between its
/// conversions and record which argument each conversion takes. Accepts both
/// plain ("%s") and positional ("%1$s") forms, as the catalogs use both.
static void mp_split_format( const std::string &fmt, std::vector<std::string> &chunks,
                             std::vector<std::size_t> &slots )
{
    chunks.clear();
    slots.clear();
    std::string cur;
    std::size_t next = 0;
    for( std::size_t i = 0; i < fmt.size(); ++i ) {
        if( fmt[i] != '%' || i + 1 >= fmt.size() ) {
            cur += fmt[i];
            continue;
        }
        if( fmt[i + 1] == '%' ) {
            cur += '%';
            ++i;
            continue;
        }
        std::size_t j = i + 1;
        std::size_t pos = 0;
        bool has_pos = false;
        while( j < fmt.size() && std::isdigit( static_cast<unsigned char>( fmt[j] ) ) ) {
            pos = pos * 10 + static_cast<std::size_t>( fmt[j] - '0' );
            has_pos = true;
            ++j;
        }
        if( has_pos && j < fmt.size() && fmt[j] == '$' ) {
            ++j;
        } else {
            has_pos = false;
            j = i + 1;
        }
        while( j < fmt.size() &&
               ( std::strchr( "-+ #0.", fmt[j] ) != nullptr ||
                 std::isdigit( static_cast<unsigned char>( fmt[j] ) ) ) ) {
            ++j;
        }
        if( j < fmt.size() && std::strchr( "diuxXofsScgeE", fmt[j] ) != nullptr ) {
            chunks.push_back( cur );
            cur.clear();
            slots.push_back( has_pos && pos > 0 ? pos - 1 : next );
            ++next;
            i = j;
            continue;
        }
        cur += fmt[i];
    }
    chunks.push_back( cur );
}

/// Try to match the remaining literal chunks from `c` onward. Backtracking is
/// needed because an argument may itself contain the text of a later chunk.
static bool mp_match_from( const std::vector<std::string> &chunks, std::size_t c,
                           const std::string &instance, std::size_t pos,
                           std::vector<std::string> &args )
{
    const std::string &lit = chunks[c];
    if( c + 1 == chunks.size() ) {
        // The trailing literal must close the instance; whatever is left in
        // between is the last argument.
        if( instance.size() < pos + lit.size() ) {
            return false;
        }
        const std::size_t tail = instance.size() - lit.size();
        if( instance.compare( tail, lit.size(), lit ) != 0 ) {
            return false;
        }
        args.push_back( instance.substr( pos, tail - pos ) );
        return true;
    }
    if( lit.empty() ) {
        // Two conversions next to each other: the first one takes nothing.
        args.push_back( std::string() );
        if( mp_match_from( chunks, c + 1, instance, pos, args ) ) {
            return true;
        }
        args.pop_back();
        return false;
    }
    std::size_t at = pos;
    while( ( at = instance.find( lit, at ) ) != std::string::npos ) {
        const std::size_t next_pos = at + lit.size();
        args.push_back( instance.substr( pos, at - pos ) );
        if( mp_match_from( chunks, c + 1, instance, next_pos, args ) ) {
            return true;
        }
        args.pop_back();
        at = next_pos;
    }
    return false;
}

/// Recover the arguments of a msgid-language instance by walking the literal
/// chunks of its format string. Returns false when the instance does not fit
/// the pattern (the caller then shows the sender's text unchanged).
static bool mp_match_format( const std::vector<std::string> &chunks, const std::string &instance,
                             std::vector<std::string> &args )
{
    args.clear();
    if( chunks.empty() ) {
        return false;
    }
    std::size_t pos = 0;
    if( !chunks.front().empty() ) {
        if( instance.compare( 0, chunks.front().size(), chunks.front() ) != 0 ) {
            return false;
        }
        pos = chunks.front().size();
    }
    return mp_match_from( chunks, 1, instance, pos, args );
}

/// Rebuild a message the host formatted in its own language, using this
/// client's catalog: recover the arguments from the msgid-language instance,
/// translate the msgid and fill the arguments back in. Names of items and
/// creatures are msgids themselves, so they are translated too.
static std::string mp_rebuild_message( const std::string &msgid, const std::string &instance )
{
    if( msgid.empty() ) {
        return instance;
    }
    std::vector<std::string> chunks;
    std::vector<std::size_t> slots;
    mp_split_format( msgid, chunks, slots );
    if( slots.empty() ) {
        // No arguments: the translated msgid is the whole line.
        std::string out = to_translation( msgid ).translated();
        if( out == msgid ) {
            out = fork_text( msgid.c_str() );
        }
        return out;
    }
    std::vector<std::string> raw_args;
    if( !mp_match_format( chunks, instance, raw_args ) ) {
        return instance;
    }
    std::size_t arg_count = 0;
    for( std::size_t idx : slots ) {
        arg_count = std::max( arg_count, idx + 1 );
    }
    std::vector<std::string> args( arg_count );
    for( std::size_t k = 0; k < slots.size() && k < raw_args.size(); ++k ) {
        // Numbers pass through; item/monster names are mapped to the client's
        // language through the name index.
        args[slots[k]] = mp_translate_name_arg( raw_args[k] );
    }
    std::string fmt = to_translation( msgid ).translated();
    if( fmt == msgid ) {
        fmt = fork_text( msgid.c_str() );
    }
    return mp_fill_args( fmt, args );
}

static bool capture_fork_message( const std::string &text, std::string &msgid,
                                  std::vector<std::string> &args )
{
    std::vector<fork_text_entry> entries;
    {
        std::lock_guard<std::mutex> lk( fork_text_mutex );
        entries = fork_text_entries;
    }
    for( const fork_text_entry &entry : entries ) {
        std::vector<std::string> chunks;
        std::vector<std::size_t> slots;
        mp_split_format( entry.text, chunks, slots );
        if( slots.empty() ) {
            if( text == entry.text ) {
                msgid = entry.msgid;
                args.clear();
                return true;
            }
            continue;
        }
        std::vector<std::string> raw_args;
        if( !mp_match_format( chunks, text, raw_args ) ) {
            continue;
        }
        std::size_t arg_count = 0;
        for( std::size_t idx : slots ) {
            arg_count = std::max( arg_count, idx + 1 );
        }
        std::vector<std::string> ordered( arg_count );
        for( std::size_t k = 0; k < slots.size() && k < raw_args.size(); ++k ) {
            ordered[slots[k]] = raw_args[k];
        }
        msgid = entry.msgid;
        args = std::move( ordered );
        return true;
    }
    return false;
}

bool capture_msgid_for( const std::string &text, std::string &msgid, std::string &instance )
{
    std::vector<std::string> fork_args;
    if( capture_fork_message( text, msgid, fork_args ) ) {
        instance = mp_fill_args( msgid, fork_args );
        return true;
    }
    if( netplay_msgcap::noted_empty() ) {
        return false;
    }
    const char *tr = netplay_msgcap::noted_translated();
    const std::string raw = netplay_msgcap::noted_raw();
    if( raw.empty() || tr == nullptr ) {
        return false;
    }
    const std::string translated_fmt( tr );
    std::vector<std::string> chunks;
    std::vector<std::size_t> slots;
    mp_split_format( translated_fmt, chunks, slots );
    if( slots.empty() ) {
        // No arguments: the line must be exactly the translated msgid.
        if( text != translated_fmt ) {
            return false;
        }
        msgid = raw;
        instance = raw;
        return true;
    }
    std::vector<std::string> args;
    if( !mp_match_format( chunks, text, args ) ) {
        return false;
    }
    std::size_t arg_count = 0;
    for( std::size_t idx : slots ) {
        arg_count = std::max( arg_count, idx + 1 );
    }
    std::vector<std::string> ordered( arg_count );
    for( std::size_t k = 0; k < slots.size() && k < args.size(); ++k ) {
        ordered[slots[k]] = args[k];
    }
    msgid = raw;
    instance = mp_fill_args( raw, ordered );
    return true;
}

bool host_send_event_fmt_to_sid( int sid, const std::string &msgid,
                                 const std::vector<std::string> &args, int msg_type )
{
    std::string payload;
    payload.reserve( 64 );
    payload += '\x01';
    payload += msgid;
    for( const std::string &a : args ) {
        payload += '\x1f';
        payload += a;
    }
    return host_send_event_to_sid( sid, payload, msg_type );
}

std::string format_event_text( const std::string &payload )
{
    if( payload.empty() ) {
        return payload;
    }
    if( payload[0] == '\x03' ) {
        // DDA MP v4.0a-166: game message relayed as msgid + msgid-language
        // instance (see host_relay_character_message).
        const std::size_t sep = payload.find( '\x1f', 1 );
        if( sep == std::string::npos ) {
            return payload.substr( 1 );
        }
        return mp_rebuild_message( payload.substr( 1, sep - 1 ), payload.substr( sep + 1 ) );
    }
    if( payload[0] != '\x01' ) {
        return payload; // plain, already localized by the sender
    }
    std::vector<std::string> parts;
    std::size_t start = 1;
    while( true ) {
        const std::size_t sep = payload.find( '\x1f', start );
        if( sep == std::string::npos ) {
            parts.push_back( payload.substr( start ) );
            break;
        }
        parts.push_back( payload.substr( start, sep - start ) );
        start = sep + 1;
    }
    std::vector<std::string> args;
    args.reserve( parts.size() - 1 );
    for( std::size_t i = 1; i < parts.size(); ++i ) {
        args.push_back( mp_event_arg( parts[i] ) );
    }
    std::string fmt = to_translation( parts[0] ).translated();
    if( fmt == parts[0] ) {
        fmt = fork_text( parts[0].c_str() );
    }
    return mp_fill_args( fmt, args );
}

/* DDA MP v4.0a-243: seen-overmap block. Only seen tiles are listed, so the
 * client reproduces the host's own map screen (and any overmap-based sidebar
 * widget) without a whole-overmap transfer. */
bool host_send_overmap_to_sid( int sid, const overmap_block &ob )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_u16( p, static_cast<uint16_t>( std::clamp( ob.width, 0, 121 ) ) );
    put_u16( p, static_cast<uint16_t>( std::clamp( ob.height, 0, 121 ) ) );
    put_i32( p, ob.origin_x );
    put_i32( p, ob.origin_y );
    put_i32( p, ob.origin_z );
    put_u16( p, static_cast<uint16_t>( std::min<size_t>( ob.ter_table.size(), 65535 ) ) );
    for( const std::string &t : ob.ter_table ) {
        put_str8( p, t );
    }
    put_u16( p, static_cast<uint16_t>( std::min<size_t>( ob.cells.size(), 65535 ) ) );
    for( const overmap_block::cell &c : ob.cells ) {
        put_u16( p, c.index );
        put_u16( p, c.ter );
        p.push_back( static_cast<char>( c.vision ) );
        p.push_back( static_cast<char>( c.vehicle ) );
    }
    put_u16( p, static_cast<uint16_t>( std::min<size_t>( ob.notes.size(), 65535 ) ) );
    for( const overmap_block::note &n : ob.notes ) {
        put_u16( p, n.index );
        put_str8( p, n.text );
    }
    if( !send_all( it->second, encode_frame( frame::OVERMAP, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    if( mp_diag_enabled() ) {
        static int mp_om_logged = 0;
        if( mp_om_logged < 3 ) {
            ++mp_om_logged;
            if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                std::fprintf( f, "overmap tx sid=%d %dx%d seen=%d notes=%d bytes=%d\n",
                              sid, ob.width, ob.height, static_cast<int>( ob.cells.size() ),
                              static_cast<int>( ob.notes.size() ),
                              static_cast<int>( p.size() ) );
                std::fclose( f );
            }
        }
    }
    return true;
}

/* DDA MP v4.0a-247: generic prompt bridge — host side. */
static bool mp_send_prompt_req( int sid, const prompt_request &pr )
{
    std::vector<char> p;
    put_u16( p, pr.id );
    p.push_back( static_cast<char>( pr.type ) );
    put_str8( p, pr.text );
    put_i32( p, pr.selected );
    put_str8( p, pr.value );
    put_i32( p, pr.max_length );
    put_u16( p, static_cast<uint16_t>( std::min<size_t>( pr.entries.size(), 65535 ) ) );
    for( const prompt_entry &e : pr.entries ) {
        put_str8( p, e.txt );
        put_i32( p, e.retval );
        p.push_back( static_cast<char>( e.enabled ? 1 : 0 ) );
        put_i32( p, e.key );
    }
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    return send_all( it->second, encode_frame( frame::PROMPT_REQ, p ) );
}

static std::optional<prompt_answer> mp_wait_prompt_answer( const prompt_request &pr )
{
    if( !mp_send_prompt_req( host_prompt_owner(), pr ) ) {
        return std::nullopt;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds( 60 );
    std::unique_lock<std::mutex> lk( state.prompt_mutex );
    while( std::chrono::steady_clock::now() < deadline ) {
        if( state.prompt_cv.wait_for( lk, std::chrono::milliseconds( 500 ), [&] {
        return state.prompt_has_answer && state.prompt_result.id == pr.id;
        } ) ) {
            return state.prompt_result;
        }
        if( host_session_count() == 0 ) {
            return std::nullopt;
        }
    }
    return std::nullopt;
}

std::optional<bool> host_prompt_query_yn( const std::string &text )
{
    if( host_prompt_owner() == 0 || !host_is_running() ) {
        return std::nullopt;
    }
    prompt_request pr;
    pr.type = prompt_type::query_yn;
    pr.text = text;
    {
        std::lock_guard<std::mutex> lk( state.prompt_mutex );
        pr.id = state.prompt_next_id++;
        state.prompt_has_answer = false;
    }
    const auto ans = mp_wait_prompt_answer( pr );
    if( !ans ) {
        return std::nullopt;
    }
    return !ans->canceled && ans->retval != 0;
}

std::optional<prompt_answer> host_prompt_uilist( const std::string &text, int selected,
        const std::vector<prompt_entry> &entries )
{
    if( host_prompt_owner() == 0 || !host_is_running() ) {
        return std::nullopt;
    }
    prompt_request pr;
    pr.type = prompt_type::uilist;
    pr.text = text;
    pr.selected = selected;
    pr.entries = entries;
    {
        std::lock_guard<std::mutex> lk( state.prompt_mutex );
        pr.id = state.prompt_next_id++;
        state.prompt_has_answer = false;
    }
    return mp_wait_prompt_answer( pr );
}

std::optional<std::string> host_prompt_string( const std::string &title,
        const std::string &value, int max_length )
{
    if( host_prompt_owner() == 0 || !host_is_running() ) {
        return std::nullopt;
    }
    prompt_request pr;
    pr.type = prompt_type::string_input;
    pr.text = title;
    pr.value = value;
    pr.max_length = max_length;
    {
        std::lock_guard<std::mutex> lk( state.prompt_mutex );
        pr.id = state.prompt_next_id++;
        state.prompt_has_answer = false;
    }
    const auto ans = mp_wait_prompt_answer( pr );
    if( !ans || ans->canceled ) {
        return std::nullopt;
    }
    return ans->text;
}

std::optional<int> host_prompt_int( const std::string &text, int value, bool show_default )
{
    if( host_prompt_owner() == 0 || !host_is_running() ) {
        return std::nullopt;
    }
    prompt_request pr;
    pr.type = prompt_type::query_int;
    pr.text = text;
    pr.value = show_default ? std::to_string( value ) : std::string();
    pr.max_length = 20;
    {
        std::lock_guard<std::mutex> lk( state.prompt_mutex );
        pr.id = state.prompt_next_id++;
        state.prompt_has_answer = false;
    }
    const auto ans = mp_wait_prompt_answer( pr );
    if( !ans || ans->canceled || ans->text.empty() ) {
        return std::nullopt;
    }
    try {
        return std::stoi( ans->text );
    } catch( ... ) {
        return std::nullopt;
    }
}

std::optional<tripoint_rel_ms> host_prompt_direction( const std::string &message,
        bool allow_vertical )
{
    if( host_prompt_owner() == 0 || !host_is_running() ) {
        return std::nullopt;
    }
    prompt_request pr;
    pr.type = prompt_type::direction;
    pr.text = message;
    pr.max_length = allow_vertical ? 1 : 0;
    {
        std::lock_guard<std::mutex> lk( state.prompt_mutex );
        pr.id = state.prompt_next_id++;
        state.prompt_has_answer = false;
    }
    const auto ans = mp_wait_prompt_answer( pr );
    if( !ans || ans->canceled || ans->text.empty() ) {
        return std::nullopt;
    }
    int dx = 0;
    int dy = 0;
    int dz = 0;
    if( std::sscanf( ans->text.c_str(), "%d,%d,%d", &dx, &dy, &dz ) < 2 ) {
        return std::nullopt;
    }
    if( dx < -1 || dx > 1 || dy < -1 || dy > 1 ||
        ( !allow_vertical && dz != 0 ) || dz < -1 || dz > 1 ) {
        return std::nullopt;
    }
    return tripoint_rel_ms( dx, dy, dz );
}

bool host_send_minimap_to_sid( int sid, const minimap_frame &mm )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    if( mp_diag_enabled() ) {
        static int mp_mm_logged = 0;
        if( mp_mm_logged < 3 ) {
            ++mp_mm_logged;
            if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                std::fprintf( f, "minimap tx sid=%d %dx%d c0=%u c1=%u\n", sid, mm.width, mm.height,
                              mm.color.empty() ? 0u : mm.color[0],
                              mm.color.size() > 1 ? mm.color[1] : 0u );
                std::fclose( f );
            }
        }
    }
    std::vector<char> p;
    put_u16( p, static_cast<uint16_t>( mm.width ) );
    put_u16( p, static_cast<uint16_t>( mm.height ) );
    put_u16( p, static_cast<uint16_t>( std::max( 0, mm.center_x ) ) );
    put_u16( p, static_cast<uint16_t>( std::max( 0, mm.center_y ) ) );
    for( uint32_t c : mm.color ) {
        put_u32( p, c );
    }
    for( uint8_t s : mm.seen ) {
        p.push_back( static_cast<char>( s ) );
    }
    put_u16( p, static_cast<uint16_t>( mm.beacons.size() ) );
    for( const minimap_frame::beacon &b : mm.beacons ) {
        put_i32( p, b.dx );
        put_i32( p, b.dy );
        put_u32( p, b.color );
        put_str8( p, b.name );
        put_str8( p, b.sym );
        p.push_back( static_cast<char>( b.is_character ) );
        p.push_back( static_cast<char>( b.sees_you ) );
        p.push_back( static_cast<char>( b.name_color ) );
        put_i32( p, b.attitude );
        put_i32( p, b.npc_attitude );
        put_i32( p, b.hp_pct );
        put_str8( p, b.wielded );
    }
    if( !send_all( it->second, encode_frame( frame::MINIMAP, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool host_send_inventory_to_sid( int sid, const std::string &text )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_str( p, text );
    if( !send_all( it->second, encode_frame( frame::INVENTORY, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool client_send_inventory_action( int node, int action, int generation,
                                   const std::vector<int> &targets )
{
    if( !state.connected ) {
        return false;
    }
    std::vector<int> args;
    args.push_back( node );
    args.push_back( action );
    args.insert( args.end(), targets.begin(), targets.end() );
    return client_send_intent( "inv.action", generation, args );
}

bool host_pop_inventory_action( int &sid, int &node, int &action, int &generation,
                                std::vector<int> &targets )
{
    std::lock_guard<std::mutex> lk( state.inv_act_mutex );
    if( state.inv_actions.empty() ) {
        return false;
    }
    auto a = std::move( state.inv_actions.front() );
    state.inv_actions.pop_front();
    sid = a.sid;
    node = a.node;
    action = a.action;
    generation = a.generation;
    targets = std::move( a.targets );
    return true;
}

bool client_send_client_action( int action )
{
    if( !state.connected ) {
        return false;
    }
    if( std::FILE *f = mp_diag_open( "netplay_actions.log" ) ) {
        std::fprintf( f, "cli action=%d\n", action );
        std::fclose( f );
    }
    return client_send_intent( "act.menu", 0, { action } );
}

bool host_pop_client_action( int &sid, int &action )
{
    std::lock_guard<std::mutex> lk( state.client_action_mutex );
    if( state.client_actions.empty() ) {
        return false;
    }
    const auto a = state.client_actions.front();
    state.client_actions.pop_front();
    sid = a.sid;
    action = a.action;
    return true;
}

bool host_send_pickup_to_sid( int sid, const std::string &text )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_str( p, text );
    if( !send_all( it->second, encode_frame( frame::PICKUP_DATA, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool client_pickup_packet( std::string &out )
{
    std::lock_guard<std::mutex> lk( state.pickup_mutex );
    if( !state.have_pickup ) {
        return false;
    }
    out = state.pickup_text;
    state.have_pickup = false;
    return true;
}

bool client_send_pickup_action( int generation, const std::vector<int> &nodes, int equip,
                                const std::vector<std::string> &uids )
{
    if( !state.connected ) {
        return false;
    }
    std::vector<int> args;
    args.push_back( equip );
    args.insert( args.end(), nodes.begin(), nodes.end() );
    std::string uid_data;
    for( size_t i = 0; i < uids.size(); ++i ) {
        if( i > 0 ) {
            uid_data += '\x1f';
        }
        uid_data += uids[i];
    }
    return client_send_intent( "pickup.action", generation, args, uid_data );
}

bool host_pop_pickup_action( int &sid, int &generation, std::vector<int> &nodes, int &equip,
                             std::string &uids )
{
    std::lock_guard<std::mutex> lk( state.pickup_act_mutex );
    if( state.pickup_actions.empty() ) {
        return false;
    }
    internal::netplay_state::pickup_action a = std::move( state.pickup_actions.front() );
    state.pickup_actions.pop_front();
    sid = a.sid;
    generation = a.generation;
    nodes = std::move( a.nodes );
    equip = a.equip;
    uids = std::move( a.uids );
    return true;
}

bool host_send_pickup_tiles_to_sid( int sid, const std::string &text )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_str( p, text );
    if( !send_all( it->second, encode_frame( frame::PICKUP_TILES, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool client_pickup_tiles( std::string &out )
{
    std::lock_guard<std::mutex> lk( state.pickup_tiles_mutex );
    if( !state.have_pickup_tiles ) {
        return false;
    }
    out = state.pickup_tiles_text;
    state.have_pickup_tiles = false;
    return true;
}

bool client_send_pickup_tile( int generation, int dx, int dy )
{
    if( !state.connected ) {
        return false;
    }
    return client_send_intent( "pickup.tiles", generation, { dx, dy } );
}

bool host_pop_pickup_tile( int &sid, int &generation, int &dx, int &dy )
{
    std::lock_guard<std::mutex> lk( state.pickup_tile_mutex );
    if( state.pickup_tile_reqs.empty() ) {
        return false;
    }
    internal::netplay_state::pickup_tile_req a = std::move( state.pickup_tile_reqs.front() );
    state.pickup_tile_reqs.pop_front();
    sid = a.sid;
    generation = a.generation;
    dx = a.dx;
    dy = a.dy;
    return true;
}

bool host_pop_inventory_settings( int &sid, int &node, int &generation, std::string &json )
{
    std::lock_guard<std::mutex> lk( state.inv_set_mutex );
    if( state.inv_settings_q.empty() ) {
        return false;
    }
    auto s = std::move( state.inv_settings_q.front() );
    state.inv_settings_q.pop_front();
    sid = s.sid;
    node = s.node;
    generation = s.generation;
    json = std::move( s.json );
    return true;
}

bool client_send_intent( const std::string &action, int generation,
                         const std::vector<int> &args, const std::string &data )
{
    if( !state.connected ) {
        return false;
    }
    if( std::FILE *f = mp_diag_open( "netplay_actions.log" ) ) {
        std::fprintf( f, "intent tx %s gen=%d args=%d\n", action.c_str(), generation,
                      static_cast<int>( args.size() ) );
        std::fclose( f );
    }
    std::vector<char> p;
    put_str( p, action );
    put_i32( p, generation );
    put_u16( p, static_cast<uint16_t>( args.size() ) );
    for( const int a : args ) {
        put_i32( p, a );
    }
    put_str( p, data );
    // DDA MP v4.0a-290: optional trailing id so the host can drop exact
    // duplicates; legacy hosts simply ignore the extra four bytes.
    static std::atomic<uint32_t> mp_next_command_id{ 1 };
    put_u32( p, mp_next_command_id.fetch_add( 1 ) );
    return client_send_on( state.client_sock, frame::INTENT, p );
}

bool host_pop_intent( intent_request &out )
{
    std::lock_guard<std::mutex> lk( state.intent_mutex );
    if( state.intents.empty() ) {
        return false;
    }
    out = std::move( state.intents.front() );
    state.intents.pop_front();
    return true;
}

bool host_pop_inventory_uid( inventory_uid_action &out )
{
    std::lock_guard<std::mutex> lk( state.inv_uid_mutex );
    if( state.inv_uid_q.empty() ) {
        return false;
    }
    out = std::move( state.inv_uid_q.front() );
    state.inv_uid_q.pop_front();
    return true;
}

bool client_send_inventory_uid( const std::string &uid, int action,
                                const std::vector<std::string> &targets,
                                const std::vector<int> &ints )
{
    if( !state.connected || uid.empty() ) {
        return false;
    }
    std::string data = uid;
    for( const std::string &t : targets ) {
        data += '\x1f';
        data += t;
    }
    std::vector<int> args;
    args.push_back( action );
    args.insert( args.end(), ints.begin(), ints.end() );
    return client_send_intent( "inv.uid", 0, args, data );
}

bool client_send_inventory_settings_uid( const std::string &uid, const std::string &json )
{
    if( !state.connected || uid.empty() ) {
        return false;
    }
    return client_send_intent( "inv.settings.uid", 0, {}, uid + '\x1f' + json );
}
bool client_send_inventory_settings( int node, int generation, const std::string &json )
{
    if( !state.connected ) {
        return false;
    }
    return client_send_intent( "inv.settings", generation, { node }, json );
}

bool host_pop_aim_request( int &sid, int &node, int &generation, int &dx, int &dy )
{
    std::lock_guard<std::mutex> lk( state.aim_req_mutex );
    if( state.aim_requests.empty() ) {
        return false;
    }
    const auto r = state.aim_requests.front();
    state.aim_requests.pop_front();
    sid = r.sid;
    node = r.node;
    generation = r.generation;
    dx = r.dx;
    dy = r.dy;
    return true;
}

bool client_send_aim_info( int node, int generation, int dx, int dy )
{
    if( !state.connected ) {
        return false;
    }
    return client_send_intent( "aim.info", generation, { node, dx, dy } );
}

bool host_send_aim_info_to_sid( int sid, const std::string &text )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_str( p, text );
    if( !send_all( it->second, encode_frame( frame::AIM_INFO, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool client_aim_info( std::string &out )
{
    std::lock_guard<std::mutex> lk( state.aim_mutex );
    if( !state.have_aim ) {
        return false;
    }
    out = state.aim_text;
    return true;
}

bool host_send_aim_panel_to_sid( int sid, const sidebar_frame &sb )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_u16( p, static_cast<uint16_t>( sb.width ) );
    put_u16( p, static_cast<uint16_t>( sb.height ) );
    for( const sidebar_cell &c : sb.cells ) {
        put_str8( p, c.text );
        put_i32( p, static_cast<int32_t>( c.color ) );
    }
    if( !send_all( it->second, encode_frame( frame::AIM_PANEL, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool host_send_aim_targets_to_sid( int sid, const std::vector<aim_target> &list )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_u16( p, static_cast<uint16_t>( list.size() ) );
    for( const aim_target &t : list ) {
        put_i32( p, t.dx );
        put_i32( p, t.dy );
        p.push_back( static_cast<char>( t.is_character ) );
        p.push_back( static_cast<char>( t.sees_you ) );
        p.push_back( static_cast<char>( t.name_color ) );
        put_i32( p, t.attitude );
        put_i32( p, t.npc_attitude );
        put_i32( p, t.hp_pct );
        put_i32( p, t.dodge );
        put_str8( p, t.name );
        put_str8( p, t.wielded );
    }
    if( !send_all( it->second, encode_frame( frame::AIM_TARGETS, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool client_aim_targets( std::vector<aim_target> &out )
{
    std::lock_guard<std::mutex> lk( state.aim_targets_mutex );
    if( !state.have_aim_targets ) {
        return false;
    }
    out = state.aim_targets;
    return true;
}

void host_broadcast_event( const std::string &text, int msg_type )
{
    std::vector<int> sids;
    {
        std::lock_guard<std::mutex> lk( state.sessions_mutex );
        sids.reserve( state.id2sock.size() );
        for( const auto &entry : state.id2sock ) {
            sids.push_back( entry.first );
        }
    }
    for( int sid : sids ) {
        host_send_event_to_sid( sid, text, msg_type );
    }
}

bool host_send_sidebar_to_sid( int sid, const sidebar_frame &sb )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_u16( p, static_cast<uint16_t>( sb.width ) );
    put_u16( p, static_cast<uint16_t>( sb.height ) );
    for( const sidebar_cell &c : sb.cells ) {
        put_str8( p, c.text );
        put_i32( p, static_cast<int32_t>( c.color ) );
    }
    if( !send_all( it->second, encode_frame( frame::SIDEBAR, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool host_send_panel_layout_to_sid( int sid, const std::string &json )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    // The layout json is several KB; put_str8 truncates at 255 bytes.
    put_str( p, json );
    if( !send_all( it->second, encode_frame( frame::PANEL_LAYOUT, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool host_send_sheet_to_sid( int sid, const std::string &text )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_str( p, text );
    if( !send_all( it->second, encode_frame( frame::CHARACTER_SHEET, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool host_send_character_json_to_sid( int sid, const std::string &json )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    put_str( p, json );
    if( !send_all( it->second, encode_frame( frame::CHARACTER_JSON, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
}

bool host_pop_key( uint32_t &code ){    std::lock_guard<std::mutex> lk( state.keys_mutex );
    if( state.host_keys.empty() ) {
        return false;
    }
    code = state.host_keys.front();
    state.host_keys.pop_front();
    return true;
}

bool host_pop_snapshot_request()
{
    return state.snap_request_pending.exchange( false );
}

point host_snapshot_view( int sid )
{
    std::lock_guard<std::mutex> lk( state.view_mutex );
    const auto it = state.client_view_offsets.find( sid );
    if( it == state.client_view_offsets.end() ) {
        return point::zero;
    }
    return point( it->second.first, it->second.second );
}

int host_snapshot_view_z( int sid )
{
    std::lock_guard<std::mutex> lk( state.view_mutex );
    const auto it = state.client_view_z.find( sid );
    return it == state.client_view_z.end() ? 0 : it->second;
}

/* ------------------------------------------------------------------ */
/* client                                                              */
/* ------------------------------------------------------------------ */

/* DDA MP v3.9: diagnostics master switch (rx snapshot size logs through
 * netplay_tiles.log). Off by default; the host menu can switch it on,
 * DDA_MP_DIAG overrides. */
static bool mp_netplay_diag_on()
{
    return mp_diag_enabled();
}

static void client_run( socket_t sock )
{
    std::string local_deny;
    while( state.connected ) {
        frame op;
        std::vector<char> payload;
        if( !recv_frame( sock, op, payload ) ) {
            break;
        }
        if( op == frame::SNAPSHOT ) {
            const std::vector<char> body = maybe_decompress( payload.data(), payload.size() );
            if( body.empty() ) {
                // broken or unsupported payload: drop and keep waiting
                continue;
            }
            reader r;
            r.data = body.data();
            r.size = body.size();
            snapshot snap;
            snap.width = static_cast<int>( r.get_u16() );
            snap.height = static_cast<int>( r.get_u16() );
            snap.status_line = r.get_str32();
            snap.has_time = r.get_u8() != 0;
            snap.turn = r.get_i32();
            snap.weather = r.get_str32();
            snap.vehicle_json = r.get_str32();
            snap.veh_ax = r.get_i32();
            snap.veh_ay = r.get_i32();
            snap.veh_az = r.get_i32();
            snap.has_vehicle_info = r.get_u8() != 0;
            // The host may broadcast a watcher frame right after the session
            // frame. Keep the last authoritative vehicle payload so that a
            // broadcast cannot blank the client mirror before it is applied.
            static std::string mp_veh_json_cache;
            static int mp_veh_ax_cache = 0;
            static int mp_veh_ay_cache = 0;
            static int mp_veh_az_cache = 0;
            static int mp_veh_ox_cache = 0;
            static int mp_veh_oy_cache = 0;
            static int mp_veh_oz_cache = 0;
            if( !snap.vehicle_json.empty() ) {
                // Authoritative session payload (or a merged cache frame):
                // remember the session origin that belongs to this JSON.
                mp_veh_json_cache = snap.vehicle_json;
                mp_veh_ax_cache = snap.veh_ax;
                mp_veh_ay_cache = snap.veh_ay;
                mp_veh_az_cache = snap.veh_az;
                mp_veh_ox_cache = snap.origin_x;
                mp_veh_oy_cache = snap.origin_y;
                mp_veh_oz_cache = snap.origin_z;
                snap.veh_ox = snap.origin_x;
                snap.veh_oy = snap.origin_y;
                snap.veh_oz = snap.origin_z;
                snap.has_vehicle_info = true;
            } else if( snap.has_vehicle_info ) {
                mp_veh_json_cache.clear();
                snap.veh_ox = snap.origin_x;
                snap.veh_oy = snap.origin_y;
                snap.veh_oz = snap.origin_z;
            } else if( !mp_veh_json_cache.empty() ) {
                snap.vehicle_json = mp_veh_json_cache;
                snap.veh_ax = mp_veh_ax_cache;
                snap.veh_ay = mp_veh_ay_cache;
                snap.veh_az = mp_veh_az_cache;
                snap.veh_ox = mp_veh_ox_cache;
                snap.veh_oy = mp_veh_oy_cache;
                snap.veh_oz = mp_veh_oz_cache;
                snap.has_vehicle_info = true;
            }
            snap.avatar_facing = r.get_u8();
            snap.avatar_controlling = r.get_u8() != 0;
            snap.oxygen = r.get_i32();
            snap.oxygen_max = r.get_i32();
            snap.has_vehicle_status = r.get_u8() != 0;
            if( snap.has_vehicle_status ) {
                snap.veh_azimuth = r.get_str8();
                snap.veh_cruise = r.get_str8();
                snap.veh_fuel = r.get_str8();
                snap.veh_battery = r.get_str8();
                snap.veh_cruise_color = static_cast<uint8_t>( r.get_u8() );
                snap.veh_fuel_color = static_cast<uint8_t>( r.get_u8() );
                snap.veh_battery_color = static_cast<uint8_t>( r.get_u8() );
                snap.veh_tracking_on = r.get_u8() != 0;
                snap.veh_has_horn = r.get_u8() != 0;
                snap.veh_has_indicator = r.get_u8() != 0;
                if( snap.veh_has_indicator ) {
                    snap.veh_indicator_next_dx = r.get_i32();
                    snap.veh_indicator_next_dy = r.get_i32();
                    snap.veh_indicator_face_dx = r.get_i32();
                    snap.veh_indicator_face_dy = r.get_i32();
                }
            }
            const uint16_t mp_ncre = r.get_u16();
            for( uint16_t i = 0; i < mp_ncre && r.ok; i++ ) {
                snapshot::creature_frame cf;
                cf.kind = static_cast<uint8_t>( r.get_u8() );
                cf.ax = r.get_i32();
                cf.ay = r.get_i32();
                cf.az = r.get_i32();
                cf.facing = static_cast<uint8_t>( r.get_u8() );
                cf.json = r.get_str32();
                snap.creatures.push_back( std::move( cf ) );
            }
            // DDA MP v4.0a-82: validate dimensions in size_t; the u16 fields
            // could overflow the old int product and let a malformed frame
            // drive the cell reader past the payload.
            constexpr int mp_snap_max_dim = 400;
            const int mp_sw = snap.width;
            const int mp_sh = snap.height;
            if( r.ok && mp_sw > 0 && mp_sh > 0
                && mp_sw <= mp_snap_max_dim && mp_sh <= mp_snap_max_dim
                && r.off <= body.size()
                && static_cast<size_t>( mp_sw ) * static_cast<size_t>( mp_sh ) * 8u
                <= body.size() - r.off ) {
                const size_t cells = static_cast<size_t>( mp_sw ) * mp_sh;
                snap.cells.reserve( cells );
                for( size_t i = 0; i < cells; i++ ) {
                    cell c;
                    c.glyph = static_cast<int>( r.get_u32() );
                    c.color = r.get_i32();
                    snap.cells.push_back( c );
                }
                // DDA MP v4.0a-239: item descriptors sit between the cells and
                // the optional sprite payload. put_u32 writes little-endian, so
                // 0x4D504954 ("MPIT") lands as T,I,P,M.
                if( r.ok && r.off + 6 <= body.size() &&
                    body[r.off + 0] == 'T' && body[r.off + 1] == 'I' &&
                    body[r.off + 2] == 'P' && body[r.off + 3] == 'M' ) {
                    reader ir;
                    ir.data = body.data();
                    ir.size = body.size();
                    ir.off = r.off + 4;
                    const uint16_t mp_nit = ir.get_u16();
                    for( uint16_t i = 0; i < mp_nit && ir.ok; i++ ) {
                        snapshot::item_tile it;
                        it.index = static_cast<int>( ir.get_u32() );
                        const uint8_t cnt = ir.get_u8();
                        for( uint8_t k = 0; k < cnt && ir.ok; k++ ) {
                            snapshot::item_desc d;
                            d.type = ir.get_str8();
                            d.variant = ir.get_str8();
                            d.mtype = ir.get_str8();
                            d.count = static_cast<int>( ir.get_u16() );
                            d.damage = static_cast<int>( ir.get_u32() );
                            d.seed = static_cast<int>( ir.get_u32() );
                            d.filthy = ir.get_u8() != 0;
                            d.corpse_flags = static_cast<uint8_t>( ir.get_u8() );
                            it.items.push_back( std::move( d ) );
                        }
                        snap.item_tiles.push_back( std::move( it ) );
                    }
                    if( ir.ok ) {
                        r.off = ir.off;
                    }
                }
                // DDA MP v4.0a-248: wide vehicle-part section, magic "MPVP".
                if( r.ok && r.off + 6 <= body.size() &&
                    body[r.off + 0] == 'M' && body[r.off + 1] == 'P' &&
                    body[r.off + 2] == 'V' && body[r.off + 3] == 'P' ) {
                    reader vr;
                    vr.data = body.data();
                    vr.size = body.size();
                    vr.off = r.off + 4;
                    const uint16_t mp_nvp = vr.get_u16();
                    for( uint16_t i = 0; i < mp_nvp && vr.ok; i++ ) {
                        snapshot::vehicle_part_frame vp;
                        vp.ax = static_cast<int>( vr.get_i32() );
                        vp.ay = static_cast<int>( vr.get_i32() );
                        vp.az = static_cast<int>( vr.get_i32() );
                        vp.id = vr.get_str8();
                        vp.variant = vr.get_str8();
                        vp.sub = vr.get_u8();
                        vp.rot = vr.get_u8();
                        vp.memory = vr.get_u8() != 0;
                        snap.vehicle_parts.push_back( std::move( vp ) );
                    }
                    if( vr.ok ) {
                        r.off = vr.off;
                    }
                }
                if( r.ok ) {
                    // Optional tileset replay extension: present when bytes
                    // remain after the ascii cells (legacy payloads end
                    // here). v3.2 packs 4 extra appearance bytes per cell
                    // (ter/furn subtile+rotation); decode tries that exact
                    // layout first, then the legacy one.
                    if( r.ok && r.off + 2 <= body.size() ) {
                        reader probe;
                        probe.data = r.data;
                        probe.size = r.size;
                        probe.off = r.off;
                        const uint16_t tc = probe.get_u16();
                        // sanity: each tile cell costs >= 6 bytes
                        if( probe.ok && tc > 0
                            && tc <= ( body.size() - probe.off ) / 6 ) {
                            const size_t tiles_off = r.off;
                            bool got_origin = false;
                            int o_x = 0;
                            int o_y = 0;
                            int o_z = 0;
                            int v_dx = 0;
                            int v_dy = 0;
                            auto try_decode = [&]( size_t extras, bool with_origin,
                                                   bool with_view = false ) {
                                reader rr;
                                rr.data = probe.data;
                                rr.size = probe.size;
                                rr.off = tiles_off;
                                rr.get_u16();
                                std::vector<tile_cell> out;
                                out.reserve( tc );
                                for( uint16_t i = 0; i < tc && rr.ok; i++ ) {
                                    tile_cell t;
                                    t.light = rr.get_u8();
                                    t.ter = rr.get_str8();
                                    t.furn = rr.get_str8();
                                    t.trap = rr.get_str8();
                                    t.critter = rr.get_str8();
                                    t.item = rr.get_str8();
                                    t.vpart = rr.get_str8();
                                    if( extras >= 6 ) {
                                        t.ter_sub = rr.get_u8();
                                        t.ter_rot = rr.get_u8();
                                        t.furn_sub = rr.get_u8();
                                        t.furn_rot = rr.get_u8();
                                        t.vpart_sub = rr.get_u8();
                                        t.vpart_rot = rr.get_u8();
                                    }
                                    if( extras >= 7 ) {
                                        t.flags = rr.get_u8();
                                    }
                                    if( extras >= 10 ) {
                                        t.light_r = rr.get_u8();
                                        t.light_g = rr.get_u8();
                                        t.light_b = rr.get_u8();
                                    }
                                    if( extras >= 13 ) {
                                        t.critter_rot = static_cast<int8_t>( rr.get_u8() );
                                        t.critter_badge = rr.get_u8();
                                        t.critter_overlays = rr.get_str32();
                                    }
                                    if( extras >= 14 ) {
                                        t.item_variant = rr.get_str8();
                                    }
                                    if( extras >= 15 ) {
                                        const uint8_t nl = rr.get_u8();
                                        t.item_layers.reserve( nl );
                                        for( uint8_t li = 0; li < nl && rr.ok; ++li ) {
                                            tile_item_layer l;
                                            l.sprite = rr.get_str8();
                                            l.variant = rr.get_str8();
                                            l.dx = static_cast<int8_t>( rr.get_u8() );
                                            l.dy = static_cast<int8_t>( rr.get_u8() );
                                            t.item_layers.push_back( std::move( l ) );
                                        }
                                        t.item_skip_top = ( t.flags & 64 ) != 0;
                                    }
                                    if( extras >= 16 ) {
                                        t.marker = rr.get_u8();
                                    }
                                    if( extras >= 17 ) {
                                        t.field = rr.get_str8();
                                        t.field_intensity = rr.get_u8();
                                        t.mem_ter = rr.get_str8();
                                        t.mem_dec = rr.get_str8();
                                        t.mem_ter_sub = rr.get_u8();
                                        t.mem_ter_rot = rr.get_u8();
                                        t.mem_dec_sub = rr.get_u8();
                                        t.mem_dec_rot = rr.get_u8();
                                        t.mem_sym = rr.get_u32();
                                    }
                                    if( extras >= 18 ) {
                                        t.graffiti = rr.get_str32();
                                    }
                                    out.push_back( std::move( t ) );
                                }
                                if( with_origin && rr.ok ) {
                                    o_x = rr.get_i32();
                                    o_y = rr.get_i32();
                                    o_z = rr.get_i32();
                                }
                                if( with_view && rr.ok ) {
                                    v_dx = static_cast<int>( rr.get_u16() ) - 32768;
                                    v_dy = static_cast<int>( rr.get_u16() ) - 32768;
                                }
                                // exact fit decides which layout ran
                                if( !rr.ok || rr.off != body.size() ) {
                                    out.clear();
                                    return out;
                                }
                                if( with_origin ) {
                                    got_origin = true;
                                }
                                return out;
                            };
                            std::vector<tile_cell> parsed = try_decode( 18, true, true );
                            if( parsed.empty() ) {
                                parsed = try_decode( 18, true );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 18, false );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 17, true );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 17, false );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 16, true );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 15, false );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 14, false );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 13, false );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 10, false );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 7, false );
                            }
                            if( parsed.empty() ) {
                                parsed = try_decode( 0, false );
                            }
                            if( !parsed.empty() ) {
                                snap.tiles = std::move( parsed );
                                snap.has_origin = got_origin;
                                snap.origin_x = o_x;
                                snap.origin_y = o_y;
                                snap.origin_z = o_z;
                                snap.view_dx = v_dx;
                                snap.view_dy = v_dy;
                            }
                        }
                    }
                }
                if( r.ok ) {
                    state.latest = snap;
                    state.have_latest = true;
                    // DDA MP debug: what the client actually parsed from the
                    // snapshot payload (sizes + how many tiles carry the
                    // light-valid flag), to compare with what the replay
                    // loop sees later.
                    {
                        static int mp_rx_log = 0;
                        if( mp_netplay_diag_on() && mp_rx_log < 120 ) {
                            ++mp_rx_log;
                            std::size_t lit = 0;
                            for( const tile_cell &t : snap.tiles ) {
                                if( t.flags & 16 ) {
                                    ++lit;
                                }
                            }
                            try {
                                std::ofstream f( netplay::mp_diag_dir() + "\\netplay_tiles.log", std::ios::app );
                                if( f ) {
                                    f << "rx snap #" << mp_rx_log << " w=" << snap.width
                                      << " h=" << snap.height
                                      << " tiles=" << snap.tiles.size()
                                      << " bit4cnt=" << lit << "\n";
                                }
                            } catch( ... ) { }
                        }
                    }
                }
            }
        } else if( op == frame::WELCOME ) {
            // DDA MP fat client: [u16 proto][u32 sid][str world name]
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            r.get_u16();
            const uint32_t sid = r.get_u32();
            if( r.ok ) {
                state.client_last_host_world = r.get_str();
                state.client_sid = static_cast<int>( sid );
                state.client_last_host_mods.clear();
                // v4.0a-122: optional trailing mod list; older hosts end here.
                if( r.ok && r.off < r.size ) {
                    const uint16_t nmods = r.get_u16();
                    for( uint16_t i = 0; i < nmods && r.ok; ++i ) {
                        state.client_last_host_mods.push_back( r.get_str() );
                    }
                }
                state.client_last_host_world_options.clear();
                if( r.ok && r.off < r.size ) {
                    const uint16_t nopts = r.get_u16();
                    for( uint16_t i = 0; i < nopts && r.ok; ++i ) {
                        const std::string name = r.get_str();
                        const std::string value = r.get_str();
                        if( r.ok ) {
                            state.client_last_host_world_options.emplace_back( name, value );
                        }
                    }
                }
                // DDA MP v4.0a-326: host data manifest.
                state.client_last_host_game_version.clear();
                state.client_last_host_netplay_version.clear();
                state.client_last_host_manifest_hash.clear();
                state.client_last_host_tileset.clear();
                state.client_last_host_mod_versions.clear();
                if( r.ok && r.off < r.size ) {
                    state.client_last_host_game_version = r.get_str();
                    state.client_last_host_netplay_version = r.get_str();
                    const uint16_t nvers = r.get_u16();
                    for( uint16_t i = 0; i < nvers && r.ok; ++i ) {
                        const std::string id = r.get_str();
                        const std::string version = r.get_str();
                        if( r.ok ) {
                            state.client_last_host_mod_versions.emplace_back( id, version );
                        }
                    }
                    if( r.ok && r.off < r.size ) {
                        state.client_last_host_manifest_hash = r.get_str();
                    }
                    if( r.ok && r.off < r.size ) {
                        state.client_last_host_tileset = r.get_str();
                    }
                }
            }
        } else if( op == frame::AVATAR_LIST ) {
            // DDA MP v4.0: [u16 n][str name + u8 alive + u8 in_session]*
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const uint16_t n = r.get_u16();
            std::vector<avatar_entry> list;
            for( uint16_t i = 0; i < n && r.ok; ++i ) {
                avatar_entry e;
                e.name = r.get_str();
                e.alive = r.get_u8() != 0;
                e.in_session = r.get_u8() != 0;
                if( r.ok ) {
                    list.push_back( std::move( e ) );
                }
            }
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.avatar_mutex );
                state.avatar_list = std::move( list );
                state.avatar_bind_state = 1; // list ready / waiting for the bind result
                state.avatar_list_ready = true;
            }
        } else if( op == frame::AVATAR_OK ) {
            state.avatar_bind_state = 2;
        } else if( op == frame::AVATAR_DENIED ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            {
                std::lock_guard<std::mutex> lk( state.avatar_mutex );
                state.avatar_deny_reason = r.get_str();
            }
            state.avatar_bind_state = 3;
        } else if( op == frame::DENY ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            local_deny = r.get_str();
        } else if( op == frame::HOVER ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const std::string text = r.get_str();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.hover_mutex );
                state.hover_text = text;
                state.have_hover = true;
            }
        } else if( op == frame::CHARACTER_SHEET ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const std::string text = r.get_str();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.sheet_mutex );
                state.sheet_text = text;
                state.have_sheet = true;
            }
        } else if( op == frame::CHARACTER_JSON ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const std::string text = r.get_str();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.char_json_mutex );
                state.char_json = text;
                state.have_char_json = true;
            }
        } else if( op == frame::AIM_INFO ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const std::string text = r.get_str();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.aim_mutex );
                state.aim_text = text;
                state.have_aim = true;
            }
        } else if( op == frame::AIM_PANEL ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            sidebar_frame sb;
            sb.width = static_cast<int>( r.get_u16() );
            sb.height = static_cast<int>( r.get_u16() );
            if( r.ok && sb.width > 0 && sb.height > 0 && sb.width <= 200 && sb.height <= 200 ) {
                sb.cells.resize( static_cast<size_t>( sb.width ) * sb.height );
                for( sidebar_cell &c : sb.cells ) {
                    c.text = r.get_str8();
                    c.color = r.get_i32();
                }
                if( r.ok ) {
                    std::lock_guard<std::mutex> lk( state.aim_panel_mutex );
                    state.aim_panel = std::move( sb );
                    state.have_aim_panel = true;
                }
            }
        } else if( op == frame::AIM_TARGETS ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const uint16_t n = r.get_u16();
            std::vector<aim_target> list;
            for( uint16_t i = 0; i < n && r.ok; i++ ) {
                aim_target t;
                t.dx = r.get_i32();
                t.dy = r.get_i32();
                t.is_character = static_cast<uint8_t>( r.get_u8() );
                t.sees_you = static_cast<uint8_t>( r.get_u8() );
                t.name_color = static_cast<uint8_t>( r.get_u8() );
                t.attitude = r.get_i32();
                t.npc_attitude = r.get_i32();
                t.hp_pct = r.get_i32();
                t.dodge = r.get_i32();
                t.name = r.get_str8();
                t.wielded = r.get_str8();
                list.push_back( std::move( t ) );
            }
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.aim_targets_mutex );
                state.aim_targets = std::move( list );
                state.have_aim_targets = true;
            }
        } else if( op == frame::SIDEBAR ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            sidebar_frame sb;
            sb.width = static_cast<int>( r.get_u16() );
            sb.height = static_cast<int>( r.get_u16() );
            if( r.ok && sb.width > 0 && sb.height > 0 && sb.width <= 200 && sb.height <= 200 ) {
                const size_t cells = static_cast<size_t>( sb.width ) * sb.height;
                sb.cells.reserve( cells );
                for( size_t i = 0; i < cells && r.ok; i++ ) {
                    sidebar_cell c;
                    c.text = r.get_str8();
                    c.color = r.get_i32();
                    sb.cells.push_back( std::move( c ) );
                }
                if( r.ok ) {
                    std::lock_guard<std::mutex> lk( state.sidebar_mutex );
                    state.sidebar = std::move( sb );
                    state.have_sidebar = true;
                }
            }
        } else if( op == frame::PANEL_LAYOUT ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const std::string json = r.get_str();
            if( r.ok && !json.empty() ) {
                std::lock_guard<std::mutex> lk( state.panel_layout_mutex );
                state.panel_layout = json;
                state.have_panel_layout = true;
            }
        } else if( op == frame::EVENT_TEXT ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const int msg_type = static_cast<int>( r.get_u8() );
            const std::string text = r.get_str();
            if( r.ok && !text.empty() ) {
                std::lock_guard<std::mutex> lk( state.event_mutex );
                state.client_events.emplace_back( msg_type, text );
                if( state.client_events.size() > 200 ) {
                    state.client_events.pop_front();
                }
            }
        } else if( op == frame::MINIMAP ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            minimap_frame mm;
            mm.width = static_cast<int>( r.get_u16() );
            mm.height = static_cast<int>( r.get_u16() );
            mm.center_x = static_cast<int>( r.get_u16() );
            mm.center_y = static_cast<int>( r.get_u16() );
            if( r.ok && mm.width > 0 && mm.height > 0 && mm.width <= 128 && mm.height <= 128 ) {
                const size_t n = static_cast<size_t>( mm.width ) * mm.height;
                mm.color.reserve( n );
                for( size_t i = 0; i < n && r.ok; i++ ) {
                    mm.color.push_back( r.get_u32() );
                }
                mm.seen.reserve( n );
                for( size_t i = 0; i < n && r.ok; i++ ) {
                    mm.seen.push_back( r.get_u8() );
                }
                const uint16_t nb = static_cast<uint16_t>( r.get_u16() );
                for( uint16_t i = 0; i < nb && r.ok; i++ ) {
                    minimap_frame::beacon b;
                    b.dx = static_cast<int>( r.get_i32() );
                    b.dy = static_cast<int>( r.get_i32() );
                    b.color = r.get_u32();
                    b.name = r.get_str8();
                    b.sym = r.get_str8();
                    b.is_character = static_cast<uint8_t>( r.get_u8() );
                    b.sees_you = static_cast<uint8_t>( r.get_u8() );
                    b.name_color = static_cast<uint8_t>( r.get_u8() );
                    b.attitude = r.get_i32();
                    b.npc_attitude = r.get_i32();
                    b.hp_pct = r.get_i32();
                    b.wielded = r.get_str8();
                    mm.beacons.push_back( b );
                }
                if( r.ok ) {
                    std::lock_guard<std::mutex> lk( state.minimap_mutex );
                    state.minimap = std::move( mm );
                    state.have_minimap = true;
                    if( mp_diag_enabled() ) {                        static int mp_mm_rx = 0;
                        if( mp_mm_rx < 3 ) {
                            ++mp_mm_rx;
                            if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                                std::fprintf( f, "minimap rx %dx%d c0=%u c1=%u\n",
                                              state.minimap.width, state.minimap.height,
                                              state.minimap.color.empty() ? 0u :
                                              state.minimap.color[0],
                                              state.minimap.color.size() > 1 ?
                                              state.minimap.color[1] : 0u );
                                std::fclose( f );
                            }
                        }
                    }
                }
            }
        } else if( op == frame::PROMPT_REQ ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            prompt_request pr;
            pr.id = r.get_u16();
            pr.type = static_cast<prompt_type>( r.get_u8() );
            pr.text = r.get_str8();
            pr.selected = static_cast<int>( r.get_i32() );
            pr.value = r.get_str8();
            pr.max_length = static_cast<int>( r.get_i32() );
            const uint16_t n = r.get_u16();
            for( uint16_t i = 0; i < n && r.ok; i++ ) {
                prompt_entry e;
                e.txt = r.get_str8();
                e.retval = static_cast<int>( r.get_i32() );
                e.enabled = r.get_u8() != 0;
                e.key = static_cast<int>( r.get_i32() );
                pr.entries.push_back( std::move( e ) );
            }
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.client_prompt_mutex );
                if( state.client_prompts.size() < 8 ) {
                    state.client_prompts.push_back( std::move( pr ) );
                }
            }
        } else if( op == frame::OVERMAP ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            overmap_block ob;
            ob.width = static_cast<int>( r.get_u16() );
            ob.height = static_cast<int>( r.get_u16() );
            ob.origin_x = static_cast<int>( r.get_i32() );
            ob.origin_y = static_cast<int>( r.get_i32() );
            ob.origin_z = static_cast<int>( r.get_i32() );
            if( r.ok && ob.width > 0 && ob.height > 0 &&
                ob.width <= 121 && ob.height <= 121 ) {
                const uint16_t nter = r.get_u16();
                for( uint16_t i = 0; i < nter && r.ok; i++ ) {
                    ob.ter_table.push_back( r.get_str8() );
                }
                const uint16_t ncells = r.get_u16();
                for( uint16_t i = 0; i < ncells && r.ok; i++ ) {
                    overmap_block::cell c;
                    c.index = r.get_u16();
                    c.ter = r.get_u16();
                    c.vision = r.get_u8();
                    c.vehicle = r.get_u8();
                    if( c.ter < ob.ter_table.size() &&
                        c.index < static_cast<uint16_t>( ob.width ) *
                        static_cast<uint16_t>( ob.height ) ) {
                        ob.cells.push_back( c );
                    }
                }
                const uint16_t nnotes = r.get_u16();
                for( uint16_t i = 0; i < nnotes && r.ok; i++ ) {
                    overmap_block::note n;
                    n.index = r.get_u16();
                    n.text = r.get_str8();
                    if( n.index < static_cast<uint16_t>( ob.width ) *
                        static_cast<uint16_t>( ob.height ) ) {
                        ob.notes.push_back( std::move( n ) );
                    }
                }
                if( r.ok ) {
                    std::lock_guard<std::mutex> lk( state.overmap_mutex );
                    state.overmap = std::move( ob );
                    state.have_overmap = true;
                }
            }
        } else if( op == frame::INVENTORY ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const std::string text = r.get_str();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.inv_mutex );
                state.inv_text = text;
                state.have_inv = true;
            }
        } else if( op == frame::PICKUP_DATA ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const std::string text = r.get_str();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.pickup_mutex );
                state.pickup_text = text;
                state.have_pickup = true;
            }
        } else if( op == frame::PICKUP_TILES ) {
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            const std::string text = r.get_str();
            if( r.ok ) {
                std::lock_guard<std::mutex> lk( state.pickup_tiles_mutex );
                state.pickup_tiles_text = text;
                state.have_pickup_tiles = true;
            }
        } else if( op == frame::PING ) {
            // DDA MP v4.0a-223: answer from the reader thread. The main thread
            // can sit in a blocking keypress for minutes, so it cannot be the
            // one that keeps the session alive.
            client_send_on( sock, frame::PONG, {} );
        } else if( op == frame::BYE ) {
            // DDA MP v4.0a-226: a BYE payload means the host removed this
            // client on purpose. Remember the reason so the UI can show it and
            // return to the main menu instead of offering a reconnect.
            reader r;
            r.data = payload.data();
            r.size = payload.size();
            if( payload.size() >= 2 ) {
                const std::string reason = r.get_str();
                if( r.ok && !reason.empty() ) {
                    state.client_kicked_flag = true;
                    state.client_kick_reason_text = reason;
                }
            }
            break;
        }
    }
    state.deny_reason = local_deny;
    close_socket( sock );
    state.connected = false;
}

bool client_connect( const std::string &address_, int port )
{
    if( state.connected ) {
        return true;
    }

    // parse "a.b.c.d:port" shortcuts honored
    std::string host = address_;
    if( host.find( ':' ) != std::string::npos ) {
        try {
            const size_t colon = host.rfind( ':' );
            const int host_port = std::stoi( host.substr( colon + 1 ) );
            if( host_port > 0 && port == DEFAULT_PORT ) {
                port = host_port;
            }
            host.resize( colon );
        } catch( ... ) {
            // ignore, format as given
        }
    }

    addrinfo hints;
    std::memset( &hints, 0, sizeof( hints ) );
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *res = nullptr;
    char portbuf[8];
    std::snprintf( portbuf, sizeof( portbuf ), "%d", port );
    if( ::getaddrinfo( host.c_str(), portbuf, &hints, &res ) != 0 || res == nullptr ) {
        return false;
    }

    socket_t sock = BAD_SOCKET;
    for( addrinfo *ai = res; ai; ai = ai->ai_next ) {
#ifdef _WIN32
        sock = static_cast<socket_t>( ::socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol ) );
#else
        sock = static_cast<socket_t>( ::socket( ai->ai_family, ai->ai_socktype, ai->ai_protocol ) );
#endif
        if( sock <= 0 ) {
            continue;
        }
#ifdef _WIN32
        if( ::connect( static_cast<SOCKET>( sock ), ai->ai_addr,
                       static_cast<int>( ai->ai_addrlen ) ) != 0 ) {
            close_socket( sock );
            sock = BAD_SOCKET;
            continue;
        }
#else
        if( ::connect( static_cast<int>( sock ), ai->ai_addr, ai->ai_addrlen ) != 0 ) {
            close_socket( sock );
            sock = BAD_SOCKET;
            continue;
        }
#endif
        break;
    }
    ::freeaddrinfo( res );

    if( sock == BAD_SOCKET ) {
        return false;
    }

    state.deny_reason.clear();
    state.client_kicked_flag = false;
    state.client_kick_reason_text.clear();
    state.client_address = string_format( "%s:%d", host, port );
    state.last_address = state.client_address;
    state.connected = true;
    state.client_sock = sock;
    // NOTE (v4.0a-226): client sends have no SO_SNDTIMEO, unlike accepted host
    // sockets. A frozen host can block a send on the UI thread. Accepted for
    // host + 1 client; add a send timeout or a writer thread before scaling.
    state.client_thread = std::thread( [] { client_run( state.client_sock ); } );

    // HELLO
    std::vector<char> hello;
    put_u16( hello, /* NETPLAY_PROTOCOL */ 0x0004 );
    put_str( hello, "player" ); // v1: replace with a configured player name
    put_str( hello, VERSION );
    put_str( hello, NETPLAY_VERSION );
    if( !client_send_on( sock, frame::HELLO, hello ) ) {
        state.stop_client();
        return false;
    }

    return true;
}

void client_disconnect()
{
    if( state.connected ) {
        client_send_on( state.client_sock, frame::BYE, {} );
    }
    set_long_wait( false );
    state.stop_client();
}

bool client_is_connected()
{
    return state.connected;
}

std::string client_deny_reason()
{
    return state.deny_reason;
}

std::string client_last_address()
{
    return state.last_address;
}

bool client_receive( snapshot &out )
{
    if( !state.have_latest ) {
        return false;
    }
    out = state.latest;
    return true;
}

#if !defined(TILES)
/* Curses-only builds: no window title to update, the hint stays silent. */
void client_status_hint( const std::string &text )
{
    ( void )text;
}
#endif

static vehicle_sidebar_view client_vehicle_sidebar_data;

void client_set_vehicle_sidebar( const snapshot &snap )
{
    client_vehicle_sidebar_data = vehicle_sidebar_view{};
    if( !snap.has_vehicle_status ) {
        return;
    }
    client_vehicle_sidebar_data.valid = true;
    client_vehicle_sidebar_data.azimuth = snap.veh_azimuth;
    client_vehicle_sidebar_data.cruise = snap.veh_cruise;
    client_vehicle_sidebar_data.fuel = snap.veh_fuel;
    client_vehicle_sidebar_data.battery = snap.veh_battery;
    client_vehicle_sidebar_data.cruise_color = snap.veh_cruise_color;
    client_vehicle_sidebar_data.fuel_color = snap.veh_fuel_color;
    client_vehicle_sidebar_data.battery_color = snap.veh_battery_color;
    client_vehicle_sidebar_data.tracking_on = snap.veh_tracking_on;
    client_vehicle_sidebar_data.has_horn = snap.veh_has_horn;
    client_vehicle_sidebar_data.has_indicator = snap.veh_has_indicator;
    client_vehicle_sidebar_data.indicator_next_dx = snap.veh_indicator_next_dx;
    client_vehicle_sidebar_data.indicator_next_dy = snap.veh_indicator_next_dy;
    client_vehicle_sidebar_data.indicator_face_dx = snap.veh_indicator_face_dx;
    client_vehicle_sidebar_data.indicator_face_dy = snap.veh_indicator_face_dy;
}

bool client_vehicle_dir_indicator( bool next, tripoint_rel_ms &out )
{
    if( !client_vehicle_sidebar_data.valid || !client_vehicle_sidebar_data.has_indicator ) {
        return false;
    }
    out = tripoint_rel_ms( next ? client_vehicle_sidebar_data.indicator_next_dx :
                            client_vehicle_sidebar_data.indicator_face_dx,
                            next ? client_vehicle_sidebar_data.indicator_next_dy :
                            client_vehicle_sidebar_data.indicator_face_dy, 0 );
    return true;
}

bool client_vehicle_sidebar( vehicle_sidebar_view &out )
{
    if( !client_vehicle_sidebar_data.valid ) {
        return false;
    }
    out = client_vehicle_sidebar_data;
    return true;
}

bool client_inventory( std::string &out )
{    std::lock_guard<std::mutex> lk( state.inv_mutex );
    if( !state.have_inv ) {
        return false;
    }
    out = state.inv_text;
    return true;
}

bool client_minimap( minimap_frame &out )
{
    std::lock_guard<std::mutex> lk( state.minimap_mutex );
    if( !state.have_minimap ) {
        return false;
    }
    out = state.minimap;
    return true;
}

bool client_take_overmap( overmap_block &out )
{
    std::lock_guard<std::mutex> lk( state.overmap_mutex );
    if( !state.have_overmap ) {
        return false;
    }
    out = std::move( state.overmap );
    state.overmap = overmap_block();
    state.have_overmap = false;
    return true;
}

/* DDA MP v4.0a-247: generic prompt bridge — client side. */
static bool client_send_on( socket_t sock, frame op, const std::vector<char> &payload );

bool client_take_prompt( prompt_request &out )
{
    std::lock_guard<std::mutex> lk( state.client_prompt_mutex );
    if( state.client_prompts.empty() ) {
        return false;
    }
    out = std::move( state.client_prompts.front() );
    state.client_prompts.pop_front();
    return true;
}

bool client_send_prompt_answer( const prompt_answer &ans )
{
    std::vector<char> p;
    put_u16( p, ans.id );
    p.push_back( static_cast<char>( ans.canceled ? 1 : 0 ) );
    put_i32( p, ans.retval );
    put_i32( p, ans.index );
    put_str8( p, ans.text );
    return client_send_on( state.client_sock, frame::PROMPT_RES, p );
}

bool client_take_event( std::string &out, int &msg_type )
{
    std::lock_guard<std::mutex> lk( state.event_mutex );
    if( state.client_events.empty() ) {
        return false;
    }
    msg_type = state.client_events.front().first;
    out = std::move( state.client_events.front().second );
    state.client_events.pop_front();
    return true;
}

bool client_sidebar( sidebar_frame &out )
{
    std::lock_guard<std::mutex> lk( state.sidebar_mutex );
    if( !state.have_sidebar ) {
        return false;
    }
    out = state.sidebar;
    return true;
}

bool client_panel_layout( std::string &out )
{
    std::lock_guard<std::mutex> lk( state.panel_layout_mutex );
    if( !state.have_panel_layout ) {
        return false;
    }
    out = state.panel_layout;
    return true;
}

bool client_character_json( std::string &out )
{
    std::lock_guard<std::mutex> lk( state.char_json_mutex );
    if( !state.have_char_json ) {
        return false;
    }
    out = state.char_json;
    return true;
}

bool client_sheet( std::string &out )
{
    std::lock_guard<std::mutex> lk( state.sheet_mutex );
    if( !state.have_sheet ) {
        return false;
    }
    out = state.sheet_text;
    return true;
}

#if !defined(TILES)
/* Curses-only builds: no window metrics to consult; the caller falls back. */
void drawable_cells( int &cols, int &rows )
{
    cols = 0;
    rows = 0;
}
#endif

bool client_take_hover( std::string &out )
{
    std::lock_guard<std::mutex> lk( state.hover_mutex );
    if( !state.have_hover ) {
        return false;
    }
    out = state.hover_text;
    state.have_hover = false;
    return true;
}

void client_send_view( int cols, int rows )
{
    if( !state.connected ) {
        return;
    }
    std::vector<char> p;
    put_u16( p, static_cast<uint16_t>( std::max( 1, cols ) ) );
    put_u16( p, static_cast<uint16_t>( std::max( 1, rows ) ) );
    if( !client_send_on( state.client_sock, frame::CLIENT_VIEW, p ) ) {
        state.stop_client();
    }
}

bool host_client_view( int sid, int &cols, int &rows )
{
    std::lock_guard<std::mutex> lk( state.view_mutex );
    const auto it = state.client_views.find( sid );
    if( it == state.client_views.end() ) {
        return false;
    }
    cols = it->second.first;
    rows = it->second.second;
    return true;
}

/* DDA MP v4.0a-86: long wait mode. Host and client are separate processes, so
 * each keeps its own flag; the host uses it to pass its own turns, the client
 * to auto-send WAIT orders. */
static std::atomic<bool> long_wait_on( false );

void set_long_wait( bool on )
{
    long_wait_on = on;
}

bool long_wait_active()
{
    return long_wait_on.load();
}

bool client_send_afk( bool on )
{
    if( !state.connected ) {
        return false;
    }
    std::vector<char> p;
    p.push_back( on ? 1 : 0 );
    return client_send_on( state.client_sock, frame::CLIENT_AFK, p );
}

bool host_session_afk( int sid )
{
    std::lock_guard<std::mutex> lk( state.afk_mutex );
    const auto it = state.session_afk.find( sid );
    return it != state.session_afk.end() && it->second;
}

void client_send_keys( const std::vector<key_event> &keys )
{
    if( !state.connected ) {
        return;
    }
    if( mp_diag_enabled() ) {
        const long long wms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                  std::chrono::system_clock::now().time_since_epoch() ).count();
        for( const key_event &k : keys ) {
            if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
                std::fprintf( f, "cli key wms=%lld code=%u\n", wms, k.code );
                std::fclose( f );
            }
        }
    }
    std::vector<char> p;
    put_u32( p, static_cast<uint32_t>( keys.size() ) );
    for( const key_event &k : keys ) {
        put_u32( p, k.code );
        put_i32( p, k.mods );
    }
    if( !client_send_on( state.client_sock, frame::KEYS, p ) ) {
        // Sending failed: the session is dead. Closing the socket unblocks the
        // receiver thread; the view loop will surface the reconnect prompt.
        state.stop_client();
    } else if( mp_diag_enabled() ) {
        if( std::FILE *f = mp_diag_open( "netplay_tiles.log" ) ) {
            std::fprintf( f, "cli key sent n=%d\n", static_cast<int>( keys.size() ) );
            std::fclose( f );
        }
    }
}

void client_request_snapshot( int view_dx, int view_dy, int view_dz )
{
    if( !state.connected ) {
        return;
    }
    // DDA MP v4.0a-232/248: carry the wanted view offset (tiles from the
    // avatar) so the host can centre the frame on the look/fire cursor,
    // including one z-level up/down while looking.
    std::vector<char> p;
    put_u16( p, static_cast<uint16_t>( std::max( -30000, std::min( 30000, view_dx ) ) + 32768 ) );
    put_u16( p, static_cast<uint16_t>( std::max( -30000, std::min( 30000, view_dy ) ) + 32768 ) );
    put_u16( p, static_cast<uint16_t>( std::max( -300, std::min( 300, view_dz ) ) + 32768 ) );
    if( !client_send_on( state.client_sock, frame::SNAP_REQ, p ) ) {
        // Sending failed: the session is dead (same policy as KEYS).
        state.stop_client();
    }
}

void client_pump()
{
    // No-op: receiving happens on the background thread.
}

/* ------------------------------------------------------------------ */
/* DDA MP v4.0 avatar protocol                                         */
/* ------------------------------------------------------------------ */

/* payload encoders shared with the game glue (netplay_world.cpp), which
 * has no access to the transport-local put_* helpers. */
std::vector<char> encode_avatar_list( const std::vector<avatar_entry> &list )
{
    std::vector<char> p;
    const size_t take = std::min<size_t>( list.size(), 0xFFFF );
    put_u16( p, static_cast<uint16_t>( take ) );
    for( size_t i = 0; i < take; ++i ) {
        const avatar_entry &e = list[i];
        put_str( p, e.name );
        p.push_back( static_cast<char>( e.alive ? 1 : 0 ) );
        p.push_back( static_cast<char>( e.in_session ? 1 : 0 ) );
    }
    return p;
}

std::vector<char> encode_avatar_text( const std::string &text )
{
    std::vector<char> p;
    put_str( p, text );
    return p;
}

/* client side: the join flow sends one request and polls bind_state until
 * the host answers (2 = bound, 3 = denied). The bind_state re-arms to 1
 * ("waiting") in every sender so stale results from a previous exchange
 * cannot be mistaken for answers. */
void client_send_avatar_list_req( const std::string &nick )
{
    if( !state.connected ) {
        return;
    }
    state.avatar_bind_state = 1;
    state.avatar_list_ready = false;
    {
        std::lock_guard<std::mutex> alk( state.avatar_mutex );
        state.avatar_list.clear();
        state.avatar_deny_reason.clear();
    }
    std::vector<char> p;
    put_str( p, nick );
    if( !client_send_on( state.client_sock, frame::AVATAR_LIST_REQ, p ) ) {
        state.stop_client();
        state.avatar_bind_state = 0;
    }
}

void client_send_avatar_resume( const std::string &nick )
{
    if( !state.connected ) {
        return;
    }
    state.avatar_bind_state = 1;
    std::vector<char> p;
    put_str( p, nick );
    if( !client_send_on( state.client_sock, frame::AVATAR_RESUME, p ) ) {
        state.stop_client();
        state.avatar_bind_state = 0;
    }
}

void client_send_avatar_profile( const std::string &nick,
                                 const std::string &profile_json )
{
    if( !state.connected ) {
        return;
    }
    state.avatar_bind_state = 1;
    std::vector<char> p;
    put_str( p, nick );
    // [u32 len][json bytes]; possibly zstd-compressed (the host reverses it)
    if( profile_json.size() > 512 ) {
        const std::vector<char> rawv( profile_json.begin(), profile_json.end() );
        const std::vector<char> zip = maybe_compress( rawv );
        put_u32( p, static_cast<uint32_t>( zip.size() ) );
        p.insert( p.end(), zip.begin(), zip.end() );
    } else {
        put_u32( p, static_cast<uint32_t>( profile_json.size() ) );
        p.insert( p.end(), profile_json.begin(), profile_json.end() );
    }
    if( !client_send_on( state.client_sock, frame::AVATAR_PROFILE, p ) ) {
        state.stop_client();
        state.avatar_bind_state = 0;
    }
}

int client_avatar_bind_state()
{
    return state.avatar_bind_state;
}

std::vector<avatar_entry> client_avatar_list()
{
    std::lock_guard<std::mutex> lk( state.avatar_mutex );
    return state.avatar_list;
}

bool client_avatar_list_ready()
{
    return state.avatar_list_ready;
}

std::string client_avatar_error()
{
    std::lock_guard<std::mutex> lk( state.avatar_mutex );
    return state.avatar_deny_reason;
}

/* host side: one-session reply + main-thread op consumption. The game glue
 * (netplay_world.cpp) runs on the host main thread, so ops must be queued
 * by session readers and popped here. */
void host_send_avatar_reply( int sid, frame op, const std::vector<char> &payload )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return;
    }
    if( !send_all( it->second, encode_frame( op, payload ) ) ) {
        // dead session: drop its socket like the broadcast path does
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
    }
}

bool host_pop_avatar_op( avatar_op &out )
{
    std::lock_guard<std::mutex> lk( state.avatar_ops_mutex );
    if( state.avatar_ops.empty() ) {
        return false;
    }
    out = std::move( state.avatar_ops.front() );
    state.avatar_ops.pop_front();
    return true;
}

} // namespace netplay
