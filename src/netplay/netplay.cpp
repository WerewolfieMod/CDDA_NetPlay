// CataDDA Multiplayer ("DDA MP") — transport layer.
// Windows-first: WinSock2 TCP; length-prefixed frames.
// Public API in "netplay/netplay.h".

#include "netplay.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <mutex>
#include <stdexcept>
#include <thread>

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

    /* ---- client ---- */
    std::atomic<bool> connected = false;
    socket_t client_sock = BAD_SOCKET;
    std::thread client_thread;
    std::string deny_reason;
    snapshot latest;
    bool have_latest = false;
    std::vector<std::vector<char>> client_out;
    std::mutex client_out_mutex;
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
        std::lock_guard<std::mutex> lk( sessions_mutex );
        for( auto &[id, sock] : id2sock ) {
            ( void )id;
            close_socket( sock );
        }
        id2sock.clear();
        id2name.clear();
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

bool init()
{
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
        send_all( peer, encode_frame( frame::WELCOME, w ) );
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
/* client                                                              */
/* ------------------------------------------------------------------ */

static void client_run( socket_t sock )
{
    std::string local_deny;
    while( state.connected ) {
        frame op;
        std::vector<char> payload;
        if( !recv_frame( sock, op, payload ) ) {
            break;
        }
        reader r;
        r.data = payload.data();
        r.size = payload.size();
        if( op == frame::SNAPSHOT ) {
            snapshot snap;
            snap.width = static_cast<int>( r.get_u16() );
            snap.height = static_cast<int>( r.get_u16() );
            snap.status_line = r.get_str();
            if( r.ok && snap.width > 0 && snap.height > 0
                && snap.width * snap.height * 8/* bytes/cell */ <= payload.size() - r.off ) {
                const size_t cells = snap.width * snap.height;
                snap.cells.reserve( cells );
                for( size_t i = 0; i < cells; i++ ) {
                    cell c;
                    c.glyph = static_cast<int>( r.get_u32() );
                    c.color = r.get_i32();
                    snap.cells.push_back( c );
                }
                if( r.ok ) {
                    state.latest = snap;
                    state.have_latest = true;
                }
            }
        } else if( op == frame::DENY ) {
            local_deny = r.get_str();
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

bool client_receive( snapshot &out )
{
    if( !state.have_latest ) {
        return false;
    }
    out = state.latest;
    return true;
}

void client_send_keys( const std::vector<key_event> &keys )
{
    if( !state.connected ) {
        return;
    }
    std::vector<char> p;
    put_u32( p, static_cast<uint32_t>( keys.size() ) );
    for( const key_event &k : keys ) {
        put_u32( p, k.code );
        put_i32( p, k.mods );
    }
    send_all( state.client_sock, encode_frame( frame::KEYS, p ) );
}

void client_pump()
{
    // No-op: receiving happens on the background thread.
}

} // namespace netplay
