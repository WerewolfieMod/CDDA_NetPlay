// CataDDA Multiplayer ("DDA MP") — transport layer.
// Windows-first: WinSock2 TCP; length-prefixed frames.
// Public API in "netplay/netplay.h".

#include "netplay.h"
#ifdef _WIN32
#include "worldfactory.h" // world_generator (WELCOME world name)
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <csignal>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>
#ifdef _WIN32
#include <windows.h> // WideCharToMultiByte (crash diagnostics)
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
    /* world the host announced in WELCOME (fat client) */
    std::string host_world_name;
    std::string client_last_host_world;
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
    /* DDA MP v4.0a-11: event lines for the client's own message log. */
    std::mutex event_mutex;
    std::deque<std::pair<int, std::string>> client_events;
    /* DDA MP v4.0a-12: overmap minimap block. */
    std::mutex minimap_mutex;
    minimap_frame minimap;
    bool have_minimap = false;
    /* DDA MP v4.0a-14: inventory listing text. */
    std::mutex inv_mutex;
    std::string inv_text;
    bool have_inv = false;
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
/* built-in diagnostics folder (no env / powershell required)            */
/* ------------------------------------------------------------------ */

const std::string &mp_diag_dir()
{
    static const std::string dir = []() -> std::string {
        const char *env = std::getenv( "DDA_MP_DIAG" );
        if( env ) {
            const std::string v( env );
            if( v.empty() || v == "0" || v == "off" ) {
                return std::string();
            }
            CreateDirectoryA( v.c_str(), nullptr );
            return v;
        }
        const char *temp = std::getenv( "TEMP" );
        if( !temp ) {
            temp = std::getenv( "TMP" );
        }
        std::string d = ( temp ? temp : "." );
        d += "\\opencode\\ddadiag";
        CreateDirectoryA( d.c_str(), nullptr );
        return d;
    }();
    return dir;
}

std::FILE *mp_diag_open( const char *name )
{
    const std::string &dir = mp_diag_dir();
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
    mp_diag_write( msg );
    // returning lets the CRT invoke _invoke_watson (fail-fast) as usual
}
#endif

static void mp_diag_terminate()
{
    mp_diag_write( "TERMINATE (uncaught exception)" );
    try {
        if( std::current_exception() != nullptr ) {
            std::rethrow_exception( std::current_exception() );
        } else {
            mp_diag_write( "  no active exception" );
        }
    } catch( const std::exception &e ) {
        mp_diag_write( std::string( "  std::exception: " ) + e.what() );
    } catch( ... ) {
        mp_diag_write( "  non-std exception" );
    }
    std::abort();
}

static void mp_diag_sigabrt( int /*sig*/ )
{
    mp_diag_write( "ABORT() invoked" );
    // Log module-relative addresses for offline symbolization (symmap).
    void *frames[32] = {};
    const unsigned short n = CaptureStackBackTrace( 0, 32, frames, nullptr );
    const HMODULE exe = GetModuleHandleA( nullptr );
    std::string out = "ABORT STACK:";
    char tmp[80];
    for( unsigned short i = 0; i < n; i++ ) {
        HMODULE mod = nullptr;
        GetModuleHandleExA( GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCSTR>( frames[i] ), &mod );
        const uintptr_t addr = reinterpret_cast<uintptr_t>( frames[i] );
        const uintptr_t base = reinterpret_cast<uintptr_t>( mod );
        ptrdiff_t off = static_cast<ptrdiff_t>( addr - base );
        if( mod == exe ) {
            std::snprintf( tmp, sizeof( tmp ), " [exe+0x%zx]", static_cast<size_t>( off ) );
        } else if( mod != nullptr ) {
            char name[MAX_PATH] = {};
            GetModuleFileNameA( mod, name, MAX_PATH );
            const char *slash = std::strrchr( name, '\\' );
            std::snprintf( tmp, sizeof( tmp ), " [%s+0x%zx]",
                           slash ? slash + 1 : name, static_cast<size_t>( off ) );
        } else {
            std::snprintf( tmp, sizeof( tmp ), " [%p]", frames[i] );
        }
        out += tmp;
    }
    mp_diag_write( out );
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
            break;
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
                std::lock_guard<std::mutex> lk( state.keys_mutex );
                // DDA MP v4.0a-2: strictly one action in flight per
                // session. While the host pump has not executed the session's
                // previous key, the slot stays occupied and newer keystrokes
                // (key repeats included) are dropped: a client must not be
                // able to stack a mountain of steps ahead of the world turns
                // they belong to.
                state.avatar_keys[sid] = sid_key{ sid, code };
            }
        } else if( op == frame::SNAP_REQ ) {
            // Empty payload: "the client finished loading, send the current
            // picture". Consumed on the host main thread (game loop pump).
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
        } else if( op == frame::BYE ) {
            break;
        }
        // other frames are ignored on the host side for now
    }

    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    state.id2sock.erase( sid );
    state.id2name.erase( sid );
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
        if( !r.ok || proto_hi != /* NETPLAY_PROTOCOL */ 0x0001 ) {
            // deny
            std::vector<char> d;
            put_str( d, "Protocol mismatch; DDA MP 0.1 expects protocol 1" );
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
        std::vector<char> w;
        put_u16( w, /* NETPLAY_PROTOCOL */ 0x0001 );
        put_u32( w, static_cast<uint32_t>( sid ) );
        // DDA MP fat client: announce the host's active world so the client
        // can load the same mod data before rendering snapshots.
        std::string wname;
#ifdef _WIN32
        if( world_generator != nullptr && world_generator->active_world != nullptr ) {
            wname = world_generator->active_world->world_name;
        }
#endif
        put_str( w, wname );
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
    put_str( p, snap.status_line );
    for( const cell &c : snap.cells ) {
        put_u32( p, static_cast<uint32_t>( c.glyph ) );
        put_i32( p, static_cast<int32_t>( c.color ) );
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
            put_str( p, t.critter_overlays );
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
        }
        // DDA MP v4.0a-4: absolute world origin of the frame's pivot cell,
        // written after the cell loop. The reader's exact-fit ladder decodes
        // this 12-byte trailer first; legacy payloads fall through as before.
        if( snap.has_origin ) {
            put_i32( p, snap.origin_x );
            put_i32( p, snap.origin_y );
            put_i32( p, snap.origin_z );
        }
    }
    return p;
}

void host_broadcast_snapshot( const snapshot &snap )
{
    std::vector<char> f = encode_frame( frame::SNAPSHOT, maybe_compress( encode_snapshot_payload_raw( snap ) ) );
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
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

bool host_send_snapshot_to_sid( int sid, const snapshot &snap )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> f = encode_frame( frame::SNAPSHOT,
                                        maybe_compress( encode_snapshot_payload_raw( snap ) ) );
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

bool host_send_event_to_sid( int sid, const std::string &text, int msg_type )
{
    std::lock_guard<std::mutex> lk( state.sessions_mutex );
    const auto it = state.id2sock.find( sid );
    if( it == state.id2sock.end() ) {
        return false;
    }
    std::vector<char> p;
    p.push_back( static_cast<char>( msg_type & 0xff ) );
    put_str( p, text );
    if( !send_all( it->second, encode_frame( frame::EVENT_TEXT, p ) ) ) {
        close_socket( it->second );
        state.id2sock.erase( it );
        state.id2name.erase( sid );
        return false;
    }
    return true;
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
    std::vector<char> p;
    put_i32( p, node );
    put_i32( p, action );
    put_i32( p, generation );
    put_u16( p, static_cast<uint16_t>( targets.size() ) );
    for( const int t : targets ) {
        put_i32( p, t );
    }
    return send_all( state.client_sock, encode_frame( frame::INVENTORY_ACTION, p ) );
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
    std::vector<char> p;
    put_i32( p, action );
    return send_all( state.client_sock, encode_frame( frame::CLIENT_ACTION, p ) );
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

bool client_send_inventory_settings( int node, int generation, const std::string &json )
{
    if( !state.connected ) {
        return false;
    }
    std::vector<char> p;
    put_i32( p, node );
    put_i32( p, generation );
    put_str( p, json );
    return send_all( state.client_sock, encode_frame( frame::INVENTORY_SETTINGS, p ) );
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
    std::vector<char> p;
    put_i32( p, node );
    put_i32( p, generation );
    put_i32( p, dx );
    put_i32( p, dy );
    return send_all( state.client_sock, encode_frame( frame::AIM_INFO_REQ, p ) );
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

/* ------------------------------------------------------------------ */
/* client                                                              */
/* ------------------------------------------------------------------ */

/* DDA MP v3.9: diagnostics master switch (rx snapshot size logs through
 * netplay_tiles.log). Flip to true and rebuild to re-enable. */
/* DDA MP v3.9: diagnostics master switch — built-in (mp_diag_dir).
 * Default ON without any env; DDA_MP_DIAG=0|off disables. */
static const bool mp_netplay_diag = mp_diag_enabled();

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
            snap.status_line = r.get_str();
            if( r.ok && snap.width > 0 && snap.height > 0
                && snap.width * snap.height * 8/* bytes/cell */ <= body.size() - r.off ) {
                const size_t cells = snap.width * snap.height;
                snap.cells.reserve( cells );
                for( size_t i = 0; i < cells; i++ ) {
                    cell c;
                    c.glyph = static_cast<int>( r.get_u32() );
                    c.color = r.get_i32();
                    snap.cells.push_back( c );
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
                            auto try_decode = [&]( size_t extras, bool with_origin ) {
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
                                        t.critter_overlays = rr.get_str();
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
                                    out.push_back( std::move( t ) );
                                }
                                if( with_origin && rr.ok ) {
                                    o_x = rr.get_i32();
                                    o_y = rr.get_i32();
                                    o_z = rr.get_i32();
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
                            std::vector<tile_cell> parsed = try_decode( 16, true );
                            if( parsed.empty() ) {
                                parsed = try_decode( 16, false );
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
                        if( mp_netplay_diag && mp_rx_log < 120 ) {
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
        } else if( op == frame::BYE ) {
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
    state.client_address = address_;
    state.last_address = address_;
    state.connected = true;
    state.client_sock = sock;
    state.client_thread = std::thread( [] { client_run( state.client_sock ); } );

    // HELLO
    std::vector<char> hello;
    put_u16( hello, /* NETPLAY_PROTOCOL */ 0x0001 );
    put_str( hello, "player" ); // v1: replace with a configured player name
    std::vector<char> h = encode_frame( frame::HELLO, hello );
    if( !send_all( sock, h ) ) {
        state.stop_client();
        return false;
    }

    return true;
}

void client_disconnect()
{
    if( state.connected ) {
        send_all( state.client_sock, encode_frame( frame::BYE, {} ) );
    }
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

bool client_inventory( std::string &out )
{
    std::lock_guard<std::mutex> lk( state.inv_mutex );
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
    if( !send_all( state.client_sock, encode_frame( frame::CLIENT_VIEW, p ) ) ) {
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
    if( !send_all( state.client_sock, encode_frame( frame::KEYS, p ) ) ) {
        // Sending failed: the session is dead. Closing the socket unblocks the
        // receiver thread; the view loop will surface the reconnect prompt.
        state.stop_client();
    }
}

void client_request_snapshot()
{
    if( !state.connected ) {
        return;
    }
    if( !send_all( state.client_sock, encode_frame( frame::SNAP_REQ, {} ) ) ) {
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
    if( !send_all( state.client_sock, encode_frame( frame::AVATAR_LIST_REQ, p ) ) ) {
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
    if( !send_all( state.client_sock, encode_frame( frame::AVATAR_RESUME, p ) ) ) {
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
    if( !send_all( state.client_sock, encode_frame( frame::AVATAR_PROFILE, p ) ) ) {
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
