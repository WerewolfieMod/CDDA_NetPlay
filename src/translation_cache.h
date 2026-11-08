#pragma once
#ifndef CATA_SRC_TRANSLATION_CACHE_H
#define CATA_SRC_TRANSLATION_CACHE_H

#include <string>

#if defined(LOCALIZE)
#include "translation_manager.h"
#endif

#include "netplay/netplay_msgcap.h"

constexpr int INVALID_LANGUAGE_VERSION = 0;

namespace detail
{

#if defined(LOCALIZE)

inline const char *_translate_internal( const char *msg )
{
    return TranslationManager::GetInstance().Translate( msg );
}

inline std::string _translate_internal( const std::string &msg )
{
    return TranslationManager::GetInstance().Translate( msg );
}

#else

inline const char *_translate_internal( const char *msg )
{
    return msg;
}

inline std::string _translate_internal( const std::string &msg )
{
    return msg;
}

#endif

// returns current language generation/version
int get_current_language_version();

template<typename T>
class local_translation_cache;

template<>
class local_translation_cache<std::string>
{
    private:
#ifndef CATA_IN_TOOL
        int cached_lang_version = INVALID_LANGUAGE_VERSION;
#endif
        std::string cached_arg;
        std::string cached_translation;
    public:
        const std::string &operator()( const std::string &arg ) {
#ifndef CATA_IN_TOOL
            if( cached_lang_version != get_current_language_version() || cached_arg != arg ) {
                cached_lang_version = get_current_language_version();
                cached_arg = arg;
                cached_translation = _translate_internal( arg );
            }
            // DDA MP: remember the raw msgid behind the translation, so a
            // netplay client can re-translate a game message (see
            // netplay_msgcap.h).
            netplay_msgcap::note_translation( arg, cached_translation );
            return cached_translation;
#else
            netplay_msgcap::note_translation( arg, arg );
            return arg;
#endif
        }
};

template<>
class local_translation_cache<const char *>
{
    private:
        std::string cached_arg;
#ifndef CATA_IN_TOOL
        int cached_lang_version = INVALID_LANGUAGE_VERSION;
        bool same_as_arg = false;
        const char *cached_translation = nullptr;
#endif
    public:
        const char *operator()( const char *arg ) {
#ifndef CATA_IN_TOOL
            if( cached_lang_version != get_current_language_version() || cached_arg != arg ) {
                cached_lang_version = get_current_language_version();
                cached_translation = _translate_internal( arg );
                same_as_arg = cached_translation == arg;
                cached_arg = arg;
            }
            // mimic gettext() behavior: return `arg` if no translation is found
            // `same_as_arg` is needed to ensure that the current `arg` is returned (not a cached one)
            const char *result = same_as_arg ? arg : cached_translation;
            // DDA MP: remember the raw msgid behind the translation (see
            // netplay_msgcap.h), so game messages can be re-translated on a
            // client running in another language.
            netplay_msgcap::note_translation( arg, result );
            return result;
#else
            netplay_msgcap::note_translation( arg, arg );
            return arg;
#endif
        }
};

// these getters are used to work around the MSVC bug that happened with using decltype in lambda
// see build log: https://gist.github.com/Aivean/e76a70edce0a1589c76bcf754ffb016b
inline local_translation_cache<const char *> get_local_translation_cache( const char * )
{
    return local_translation_cache<const char *>();
}

inline local_translation_cache<std::string> get_local_translation_cache(
    const std::string_view )
{
    return local_translation_cache<std::string>();
}

} // namespace detail

#endif // CATA_SRC_TRANSLATION_CACHE_H
