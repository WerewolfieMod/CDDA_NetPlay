#pragma once
#ifndef CATA_SRC_NETPLAY_INVENTORY_SOURCE_H
#define CATA_SRC_NETPLAY_INVENTORY_SOURCE_H

#include <cstdlib>
#include <functional>
#include <string>
#include <vector>

#include "character.h"
#include "item.h"
#include "item_category.h"
#include "units.h"

/* DDA MP v4.0a-16: source-agnostic inventory data.
 *
 * The host inventory UI (inventory_ui.cpp) is built around live item_locations
 * and cannot be reused as-is on a client that only has raw data. This header
 * introduces the data shape both sides agree on, plus:
 *   - character_inventory_source: host adapter over a live Character;
 *   - serialize/parse helpers: the exact same fields travel over the wire,
 *     so the client renderer consumes identical data without re-deriving it.
 *
 * The shape is deliberately flat and self-contained (no pointers, no ids the
 * client cannot resolve) so it can also feed other menus later.
 */
namespace netplay
{

struct inv_entry_view {
    std::string id;             // itype id (for icons/localisation later)
    std::string name;           // display name, colour markup stripped
    int count = 1;
    int charges = 0;
    long weight_g = 0;
    long volume_ml = 0;
    int side_mm = 0;            // longest side, like the host's iteminfo
    bool worn = false;
    bool wielded = false;
    bool container = false;
    std::string category;       // display name of the item category
    std::string category_id;
    int category_rank = 0;
    char invlet = '\0';         // assigned inventory letter, as shown by the host
    int depth = 0;              // 0 = top level, >0 = nested inside a container
    int color_index = 8;        // base-16 palette index of the host item colour
    bool favorite = false;      // host favorite flag (drives the * marker/menu)
};

/// Header numbers shown above the host inventory (volume/weight/space).
struct inv_header_view {
    long volume_ml = 0;
    long volume_cap_ml = 0;
    long weight_g = 0;
    long weight_cap_g = 0;
    long free_ml = 0;
    long large_free_ml = 0;     // largest single free pocket volume
    int large_free_len_mm = 0;  // longest item that pocket accepts
    int hotkeys_assigned = 0;
    int hotkeys_total = 0;
    int generation = 0;         // packet version, guards row-index actions
};

/// Map an nc_color to the 16-colour palette index used by minimap/items.
inline int inv_color_index( nc_color c )
{
    static const nc_color base16[] = {
        c_black, c_red, c_green, c_brown, c_blue, c_magenta, c_cyan,
        c_light_gray, c_dark_gray, c_light_red, c_light_green, c_yellow,
        c_light_blue, c_pink, c_light_cyan, c_white
    };
    for( int i = 0; i < 16; i++ ) {
        if( c == base16[i] ) {
            return i;
        }
    }
    return 8;
}

struct inv_packet {
    inv_header_view header;
    std::vector<inv_entry_view> entries;
};

class inventory_source
{
    public:
        virtual ~inventory_source() = default;
        virtual std::string title() const = 0;
        virtual std::vector<inv_entry_view> entries() const = 0;
        virtual inv_header_view header() const {
            return inv_header_view();
        }

        long total_weight_g() const {
            long sum = 0;
            for( const inv_entry_view &e : entries() ) {
                sum += e.weight_g;
            }
            return sum;
        }
        long total_volume_ml() const {
            long sum = 0;
            for( const inv_entry_view &e : entries() ) {
                sum += e.volume_ml;
            }
            return sum;
        }
};

/// Strip <color_...> markup from an item name (tname() carries it).
inline std::string inv_strip_tags( const std::string &raw )
{
    std::string out;
    out.reserve( raw.size() );
    for( std::size_t i = 0; i < raw.size(); i++ ) {
        if( raw[i] == '<' ) {
            const std::size_t e = raw.find( '>', i );
            if( e != std::string::npos ) {
                i = e;
                continue;
            }
        }
        out += raw[i];
    }
    return out;
}

/// Host-side adapter: turns a live Character into plain rows.
class character_inventory_source : public inventory_source
{
    public:
        explicit character_inventory_source( Character &who ) : u( who ) {}

        std::string title() const override {
            return u.get_name();
        }

        std::vector<inv_entry_view> entries() const override {
            std::vector<inv_entry_view> out;
            std::function<void( const item &, int, bool, bool )> add =
            [&]( const item & it, int depth, bool worn, bool wielded ) {
                inv_entry_view e;
                e.id = it.typeId().str();
                // display_name() carries the host's per-item details in
                // parentheses: charges/battery, unread, worn state, mods.
                e.name = inv_strip_tags( it.display_name( it.count(), false ) );
                e.count = it.count();
                e.charges = it.charges;
                e.weight_g = static_cast<long>( units::to_gram( it.weight() ) );
                e.volume_ml = static_cast<long>( units::to_milliliter( it.volume() ) );
                e.side_mm = static_cast<int>( units::to_millimeter( it.length() ) );
                e.worn = worn;
                e.wielded = wielded;
                e.container = it.is_container();
                e.invlet = it.invlet;
                e.depth = depth;
                e.color_index = inv_color_index( it.color_in_inventory() );
                e.favorite = it.is_favorite;
                const item_category &cat = it.get_category_shallow();
                e.category = cat.name_header();
                e.category_id = cat.get_id().str();
                e.category_rank = cat.sort_rank();
                out.push_back( std::move( e ) );
                // Container hierarchy: children follow their parent, indented.
                for( const item *child : it.get_contents().all_items_top() ) {
                    if( child != nullptr ) {
                        add( *child, depth + 1, worn, wielded );
                    }
                }
            };
            for( const item *it : u.inv_dump() ) {
                if( it != nullptr ) {
                    add( *it, 0, u.is_worn( *it ), u.is_wielding( *it ) );
                }
            }
            return out;
        }

        inv_header_view header() const override {
            inv_header_view h;
            h.volume_ml = static_cast<long>( units::to_milliliter( u.volume_carried() ) );
            h.volume_cap_ml = static_cast<long>( units::to_milliliter( u.volume_capacity() ) );
            h.weight_g = static_cast<long>( units::to_gram( u.weight_carried() ) );
            h.weight_cap_g = static_cast<long>( units::to_gram( u.weight_capacity() ) );
            h.free_ml = static_cast<long>( units::to_milliliter( u.free_space() ) );
            // Largest single pocket, like the host's "Largest Space" stat.
            {
                const auto pockets = u.get_all_pockets_with_constraints(
                []( const item_pocket & p ) {
                    return p.is_type( pocket_type::CONTAINER ) && item_pocket::ok_for_solids( p );
                },
                item_pocket::ok_default_containers );
                units::volume best = 0_ml;
                units::length best_len = 0_mm;
                for( const auto &pc : pockets ) {
                    if( pc.second.remaining_volume > best ) {
                        best = pc.second.remaining_volume;
                        best_len = pc.second.max_containable_length;
                    }
                }
                h.large_free_ml = static_cast<long>( units::to_milliliter( best ) );
                h.large_free_len_mm = static_cast<int>( units::to_millimeter( best_len ) );
            }
            return h;
        }

    private:
        Character &u;
};

/* Wire format: one entry per line, fields separated by US (0x1f).
 * Control characters are replaced in text fields so a single line is always
 * one entry. Field order must match parse_inv_entries(). */
namespace inv_wire
{
constexpr char sep = '\x1f';

inline std::string clean( std::string s )
{
    for( char &c : s ) {
        if( c == '\n' || c == '\r' || c == sep ) {
            c = ' ';
        }
    }
    return s;
}

inline std::string field( long long v )
{
    return std::to_string( v );
}
} // namespace inv_wire

inline std::string serialize_inv_entries( const std::vector<inv_entry_view> &entries )
{
    std::string out;
    for( const inv_entry_view &e : entries ) {
        out += inv_wire::clean( e.id );
        out += inv_wire::sep;
        out += inv_wire::clean( e.name );
        out += inv_wire::sep;
        out += inv_wire::field( e.count );
        out += inv_wire::sep;
        out += inv_wire::field( e.charges );
        out += inv_wire::sep;
        out += inv_wire::field( e.weight_g );
        out += inv_wire::sep;
        out += inv_wire::field( e.volume_ml );
        out += inv_wire::sep;
        out += inv_wire::field( e.side_mm );
        out += inv_wire::sep;
        out += inv_wire::field( e.worn ? 1 : 0 );
        out += inv_wire::sep;
        out += inv_wire::field( e.wielded ? 1 : 0 );
        out += inv_wire::sep;
        out += inv_wire::field( e.container ? 1 : 0 );
        out += inv_wire::sep;
        out += inv_wire::clean( e.category );
        out += inv_wire::sep;
        out += inv_wire::clean( e.category_id );
        out += inv_wire::sep;
        out += inv_wire::field( e.category_rank );
        out += inv_wire::sep;
        out += inv_wire::field( e.invlet == '\0' ? 0 : static_cast<unsigned char>( e.invlet ) );
        out += inv_wire::sep;
        out += inv_wire::field( e.depth );
        out += inv_wire::sep;
        out += inv_wire::field( e.color_index );
        out += inv_wire::sep;
        out += inv_wire::field( e.favorite ? 1 : 0 );
        out += '\n';
    }
    return out;
}

inline std::vector<inv_entry_view> parse_inv_entries( const std::string &text )
{
    std::vector<inv_entry_view> out;
    std::size_t pos = 0;
    while( pos < text.size() ) {
        const std::size_t eol = text.find( '\n', pos );
        const std::size_t end = eol == std::string::npos ? text.size() : eol;
        const std::string line = text.substr( pos, end - pos );
        pos = eol == std::string::npos ? text.size() : eol + 1;
        if( line.empty() ) {
            continue;
        }
        std::vector<std::string> f;
        std::size_t p = 0;
        while( true ) {
            const std::size_t q = line.find( inv_wire::sep, p );
            f.push_back( line.substr( p, q == std::string::npos ? std::string::npos : q - p ) );
            if( q == std::string::npos ) {
                break;
            }
            p = q + 1;
        }
        if( f.size() < 13 ) {
            continue;
        }
        inv_entry_view e;
        e.id = f[0];
        e.name = f[1];
        e.count = std::atoi( f[2].c_str() );
        e.charges = std::atoi( f[3].c_str() );
        e.weight_g = std::atol( f[4].c_str() );
        e.volume_ml = std::atol( f[5].c_str() );
        e.side_mm = std::atoi( f[6].c_str() );
        e.worn = std::atoi( f[7].c_str() ) != 0;
        e.wielded = std::atoi( f[8].c_str() ) != 0;
        e.container = std::atoi( f[9].c_str() ) != 0;
        e.category = f[10];
        e.category_id = f[11];
        e.category_rank = std::atoi( f[12].c_str() );
        if( f.size() > 13 ) {
            const int inv = std::atoi( f[13].c_str() );
            e.invlet = inv == 0 ? '\0' : static_cast<char>( inv );
        }
        if( f.size() > 14 ) {
            e.depth = std::atoi( f[14].c_str() );
        }
        if( f.size() > 15 ) {
            e.color_index = std::atoi( f[15].c_str() );
        }
        if( f.size() > 16 ) {
            e.favorite = std::atoi( f[16].c_str() ) != 0;
        }
        out.push_back( std::move( e ) );
    }
    return out;
}

/* Packet = one header line starting with 'H' + US, then entry lines. The
 * header is optional so old text-only payloads still parse as entries. */
inline std::string serialize_inv_packet( const inv_packet &pkt )
{
    std::string out;
    out += 'H';
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.volume_ml );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.volume_cap_ml );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.weight_g );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.weight_cap_g );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.free_ml );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.large_free_ml );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.large_free_len_mm );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.hotkeys_assigned );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.hotkeys_total );
    out += inv_wire::sep;
    out += inv_wire::field( pkt.header.generation );
    out += '\n';
    out += serialize_inv_entries( pkt.entries );
    return out;
}

inline inv_packet parse_inv_packet( const std::string &text )
{
    inv_packet pkt;
    std::size_t start = 0;
    if( text.size() > 2 && text[0] == 'H' && text[1] == inv_wire::sep ) {
        const std::size_t eol = text.find( '\n' );
        const std::size_t end = eol == std::string::npos ? text.size() : eol;
        const std::string line = text.substr( 2, end - 2 );
        start = eol == std::string::npos ? text.size() : eol + 1;
        std::vector<std::string> f;
        std::size_t p = 0;
        while( true ) {
            const std::size_t q = line.find( inv_wire::sep, p );
            f.push_back( line.substr( p, q == std::string::npos ? std::string::npos : q - p ) );
            if( q == std::string::npos ) {
                break;
            }
            p = q + 1;
        }
        if( f.size() >= 9 ) {
            pkt.header.volume_ml = std::atol( f[0].c_str() );
            pkt.header.volume_cap_ml = std::atol( f[1].c_str() );
            pkt.header.weight_g = std::atol( f[2].c_str() );
            pkt.header.weight_cap_g = std::atol( f[3].c_str() );
            pkt.header.free_ml = std::atol( f[4].c_str() );
            pkt.header.large_free_ml = std::atol( f[5].c_str() );
            pkt.header.large_free_len_mm = std::atoi( f[6].c_str() );
            pkt.header.hotkeys_assigned = std::atoi( f[7].c_str() );
            pkt.header.hotkeys_total = std::atoi( f[8].c_str() );
            if( f.size() >= 10 ) {
                pkt.header.generation = std::atoi( f[9].c_str() );
            }
        }
    }
    pkt.entries = parse_inv_entries( text.substr( start ) );
    return pkt;
}

} // namespace netplay

#endif // CATA_SRC_NETPLAY_INVENTORY_SOURCE_H
