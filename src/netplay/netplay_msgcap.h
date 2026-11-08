#pragma once
#ifndef CATA_SRC_NETPLAY_MSGCAP_H
#define CATA_SRC_NETPLAY_MSGCAP_H

#include <string>
#include <utility>

/* DDA MP v4.0a-166: capture of the untranslated source of a game message.
 *
 * The message system formats a string with the sender's translation catalog
 * and keeps only the result. A netplay client running in another language
 * therefore received the host's language and could not re-translate it.
 *
 * The add_msg templates format the message twice: once translated for the
 * sender (as before) and once from the raw msgid for the wire. They stash the
 * msgid, that msgid-language instance and the sender's final text here; the
 * virtual add_msg overloads (which are the ones that know whether the message
 * belongs to a netplay session) pick the entry up again.
 *
 * The stash is matched by comparing the text the overload received with the
 * text stored here, so an entry left behind by an overload that does not
 * consume it (a monster message, for instance) can never be attached to an
 * unrelated message.
 *
 * The type is a plain struct plus inline functions on purpose: this header is
 * included from creature.h and messages.h and must not pull anything heavy.
 */
namespace netplay_msgcap
{

struct captured {
    /// Untranslated format string ("You pick up the %s.").
    std::string msgid;
    /// The same format string with the arguments filled in, untranslated.
    std::string instance;
    /// The formatted text the sender's add_msg call received.
    std::string text;

    bool valid() const {
        return !msgid.empty();
    }
    void clear() {
        msgid.clear();
        instance.clear();
        text.clear();
    }
};

/// Per-thread single-entry stash (messages are built and delivered on the
/// same thread; a separate thread cannot pick up another thread's entry).
inline captured &slot()
{
    static thread_local captured entry;
    return entry;
}

inline void stash( std::string msgid, std::string instance, std::string text )
{
    captured &entry = slot();
    entry.msgid = std::move( msgid );
    entry.instance = std::move( instance );
    entry.text = std::move( text );
}

/// Take the entry if it belongs to `text` and clear the stash either way.
inline bool take( const std::string &text, captured &out )
{
    captured &entry = slot();
    const bool hit = !entry.msgid.empty() && entry.text == text;
    if( hit ) {
        out = entry;
    }
    entry.clear();
    return hit;
}

inline void clear()
{
    slot().clear();
}

/* DDA MP v4.0a-166: the `_()` macro returns an already translated string, so
 * the raw msgid is lost before add_msg sees it. Every `_()` call notes the
 * pair (raw msgid, translated text) here. When the message system then relays
 * a formatted line to a netplay client, capture_msgid_for() checks whether the
 * line is an instance of that translated format, recovers the arguments and
 * sends the raw msgid instead of the sender's sentence.
 *
 * The raw string is stored as a pointer when the `_()` argument was a literal
 * (valid for the whole full-expression that contains the add_msg call) and as a
 * copy when the argument was a std::string. */
struct noted {
    const char *raw_ptr = nullptr;  // literal msgid (stable storage)
    std::string raw;                // copy, when the source was a std::string
    const char *tr_ptr = nullptr;   // translated format (stable storage)
    bool from_ptr = false;
};

inline noted &note_slot()
{
    static thread_local noted entry;
    return entry;
}

inline void note_translation( const char *raw, const char *translated )
{
    // Both pointers are stable for the whole full-expression that contains the
    // add_msg call: the msgid is a literal and the translation lives in the
    // per-call-site cache. No copies, so this stays cheap on the hot UI path.
    noted &n = note_slot();
    n.raw_ptr = raw;
    n.from_ptr = true;
    n.raw.clear();
    n.tr_ptr = translated;
}

inline void note_translation( const std::string &raw, const std::string &translated )
{
    // `translated` is the static per-call-site cache's own buffer (stable);
    // `raw` may be a temporary, so keep a copy.
    noted &n = note_slot();
    n.raw_ptr = nullptr;
    n.from_ptr = false;
    n.raw = raw;
    n.tr_ptr = translated.c_str();
}

inline std::string noted_raw()
{
    const noted &n = note_slot();
    return n.from_ptr && n.raw_ptr != nullptr ? std::string( n.raw_ptr ) : n.raw;
}

inline bool noted_empty()
{
    const noted &n = note_slot();
    return n.tr_ptr == nullptr || n.tr_ptr[0] == '\0';
}

inline const char *noted_translated()
{
    return note_slot().tr_ptr;
}

inline void clear_note()
{
    noted &n = note_slot();
    n.raw_ptr = nullptr;
    n.from_ptr = false;
    n.raw.clear();
    n.tr_ptr = nullptr;
}

} // namespace netplay_msgcap

#endif // CATA_SRC_NETPLAY_MSGCAP_H
