// Standalone check of the netplay payload compression path (same code as
// src/netplay/netplay.cpp) linked against the same zstd build.
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include "zstd/zstd.h"

#include <algorithm>

static constexpr size_t SNAP_MAX_UNCOMPRESSED = 64u * 1024u * 1024u;

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
    const int vnum = ZSTD_versionNumber();
    const size_t got = ZSTD_decompress( out.data(), out.size(), data + 1, size - 1 );
    printf( "zstd version %d; decompress got=%zu isError=%d (%s)\n",
            vnum, got, ZSTD_isError( got ) ? 1 : 0, ZSTD_getErrorName( got ) );
    if( ZSTD_isError( got ) ) {
        return {};
    }
    out.resize( got );
    return out;
}

static void put_u16( std::vector<char> &b, uint16_t v )
{
    b.push_back( char( v & 0xff ) );
    b.push_back( char( ( v >> 8 ) & 0xff ) );
}
static void put_str( std::vector<char> &b, const std::string &s )
{
    put_u16( b, uint16_t( s.size() ) );
    b.insert( b.end(), s.begin(), s.end() );
}

int main()
{
    // 45x25 frame like the real host would build
    std::vector<char> raw;
    put_u16( raw, 45 );
    put_u16( raw, 25 );
    put_str( raw, "HP: 87/100 | Year 1 day 12 08:34 | light rain" );
    for( int i = 0; i < 45 * 25; i++ ) {
        const uint32_t glyph = uint32_t( 'a' + ( i % 26 ) );
        raw.push_back( char( glyph & 0xff ) ); raw.push_back( char( ( glyph >> 8 ) & 0xff ) );
        raw.push_back( char( ( glyph >> 16 ) & 0xff ) ); raw.push_back( char( ( glyph >> 24 ) & 0xff ) );
        const int32_t color = i % 16;
        raw.push_back( char( color & 0xff ) ); raw.push_back( char( ( color >> 8 ) & 0xff ) );
        raw.push_back( char( ( color >> 16 ) & 0xff ) ); raw.push_back( char( ( color >> 24 ) & 0xff ) );
    }
    const std::vector<char> comp = maybe_compress( raw );
    printf( "raw=%zu comp=%zu tag=%d first=0x%02x\n", raw.size(), comp.size(), uint8_t( comp[0] ), uint8_t( comp[1] ) );
    const size_t hint = ZSTD_getFrameContentSize( comp.data() + 1, comp.size() - 1 );
    printf( "hint=%zu err=%d unknown=%d\n", hint,
            hint == ZSTD_CONTENTSIZE_ERROR ? 1 : 0,
            hint == ZSTD_CONTENTSIZE_UNKNOWN ? 1 : 0 );
    const std::vector<char> dec = maybe_decompress( comp.data(), comp.size() );
    printf( "dec=%zu equal=%d\n", dec.size(), dec == raw ? 1 : 0 );
    if( dec != raw ) {
        printf( "MISMATCH!\n" );
        for( size_t i = 0; i < dec.size() && i < raw.size(); i++ ) {
            if( dec[i] != raw[i] ) { printf( "first diff at %zu: raw=%02x dec=%02x\n", i, uint8_t( raw[i] ), uint8_t( dec[i] ) ); break; }
        }
        return 1;
    }
    // also check the raw-tag path
    std::vector<char> rawtag{ 0 };
    rawtag.insert( rawtag.end(), raw.begin(), raw.end() );
    const std::vector<char> dec2 = maybe_decompress( rawtag.data(), rawtag.size() );
    printf( "raw-path equal=%d\n", dec2 == raw ? 1 : 0 );
    return 0;
}

