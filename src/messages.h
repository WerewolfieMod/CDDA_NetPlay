#pragma once
#ifndef CATA_SRC_MESSAGES_H
#define CATA_SRC_MESSAGES_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

#include "coords_fwd.h"
#include "debug.h"
#include "enums.h"
#include "netplay/netplay_msgcap.h"
#include "string_formatter.h"
#include "translation.h"

class Creature;
class JsonObject;
class JsonOut;

namespace catacurses
{
class window;
} // namespace catacurses

namespace Messages
{

std::vector<std::pair<std::string, std::string>> recent_messages( size_t count );
/// DDA MP v4.0a-119: recent messages with their game_message_type, so a host
/// action's text can be relayed to the acting client with the right color.
std::vector<std::pair<std::string, int>> recent_messages_typed( size_t count );
bool has_debug_filter( debugmode::debug_filter type );
void add_msg( std::string msg );
void add_msg( const game_message_params &params, std::string msg );
void clear_messages();
void deactivate();
size_t size();
/// Monotonic counter of messages ever appended to the log (survives the
/// MESSAGE_LIMIT rotation, unlike size()). Used to forward exactly the
/// messages a host action produced to the acting client.
size_t generation();
/// Drop the newest `count` messages from the host's log. Client action
/// messages are forwarded to that client and must not stay on the host.
size_t remove_last( size_t count );
bool has_undisplayed_messages();
void display_messages();
void display_messages( const catacurses::window &ipk_target, int left, int top, int right,
                       int bottom );
void serialize( JsonOut &json );
void deserialize( const JsonObject &json );

} // namespace Messages

void add_msg( std::string msg );
template<typename ...Args>
inline void add_msg( const std::string &msg, Args &&... args )
{
    return add_msg( string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg( const char *const msg, Args &&... args )
{
    return add_msg( string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg( const translation &msg, Args &&... args )
{
    return add_msg( string_format( msg.translated(), std::forward<Args>( args )... ) );
}

void add_msg( const game_message_params &params, std::string msg );
template<typename ...Args>
inline void add_msg( const game_message_params &params, const std::string &msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg( params, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg( const game_message_params &params, const char *const msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg( params, string_format( msg, std::forward<Args>( args )... ) );
}

void add_msg_if_player_sees( const tripoint_bub_ms &target, std::string msg,
                             const std::string &msgid = std::string(),
                             const std::string &instance = std::string() );
void add_msg_if_player_sees( const Creature &target, std::string msg,
                             const std::string &msgid = std::string(),
                             const std::string &instance = std::string() );
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target, const std::string &msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const std::string &msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target, const char *const msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const char *const msg, Args &&... args )
{
    return add_msg_if_player_sees( target, string_format( msg, std::forward<Args>( args )... ) );
}
/* DDA MP v4.0a-166: format a "player sees" message and remember its
 * untranslated source for the netplay relay (the client re-formats the line
 * with its own catalog). Kept out of the add_msg_if_player_sees templates so
 * they stay readable. */
template<typename ...Args>
inline std::string mp_format_seen_msg( const translation &msg, Args &&... args )
{
    if( msg.translatable() && !msg.msgid().empty() ) {
        std::string instance = string_format( msg.msgid(), args... );
        std::string text = string_format( msg, std::forward<Args>( args )... );
        netplay_msgcap::stash( msg.msgid(), std::move( instance ), text );
        return text;
    }
    netplay_msgcap::clear();
    return string_format( msg, std::forward<Args>( args )... );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target, const translation &msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, mp_format_seen_msg( msg,
                                   std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const translation &msg,
                                    Args &&... args )
{
    return add_msg_if_player_sees( target, mp_format_seen_msg( msg,
                                   std::forward<Args>( args )... ) );
}

void add_msg_if_player_sees( const tripoint_bub_ms &target, const game_message_params &params,
                             std::string msg, const std::string &msgid = std::string(),
                             const std::string &instance = std::string() );
void add_msg_if_player_sees( const Creature &target, const game_message_params &params,
                             std::string msg, const std::string &msgid = std::string(),
                             const std::string &instance = std::string() );
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target,
                                    const game_message_params &params,
                                    const std::string &msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg_if_player_sees( target, params, string_format( msg,
                                   std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const game_message_params &params,
                                    const std::string &msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg_if_player_sees( target, params, string_format( msg,
                                   std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const tripoint_bub_ms &target,
                                    const game_message_params &params,
                                    const char *const msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg_if_player_sees( target, params, string_format( msg,
                                   std::forward<Args>( args )... ) );
}
template<typename ...Args>
inline void add_msg_if_player_sees( const Creature &target, const game_message_params &params,
                                    const char *const msg, Args &&... args )
{
    if( params.type == m_debug && !debug_mode ) {
        return;
    }
    return add_msg_if_player_sees( target, params, string_format( msg,
                                   std::forward<Args>( args )... ) );
}

// This function acts as a marker to clang-tidy to enforce no translations
// in developer facing debug messages.
template <typename T>
inline T &&clang_tidy_no_translations( T &&t )
{
    return std::forward<T>( t );
}

#define add_msg_debug_if(condition, type, ...)                                                          \
    do {                                                                                                \
        if( debug_mode && Messages::has_debug_filter( type ) && ( condition ) ) {                       \
            std::string cata_dbg_msg = clang_tidy_no_translations( string_format( __VA_ARGS__ ) );      \
            Messages::add_msg( m_debug, cata_dbg_msg );                                                 \
            debug_menu::log_debug_msg( type, cata_dbg_msg );                                            \
        }                                                                                               \
    } while( false )

#define add_msg_debug(type, ...) \
    add_msg_debug_if( true, type, __VA_ARGS__ )

namespace debug_menu
{
void log_debug_msg( debugmode::debug_filter type, const std::string &msg );
} // namespace debug_menu

void modify_msg_with_exclamations( std::string &msg, game_message_type type );

#endif // CATA_SRC_MESSAGES_H
