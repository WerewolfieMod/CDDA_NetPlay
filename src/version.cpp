#include "get_version.h" // IWYU pragma: associated

#include <string>

#if (defined(_WIN32) || defined(MINGW)) && !defined(GIT_VERSION) && !defined(CROSS_LINUX) && !defined(_MSC_VER)

#ifndef VERSION
#define VERSION "0.J"
#endif

#else

#include "version.h"

#endif

const char *getVersionString()
{
    // DDA NetPlay: keep the official upstream build name and mark the fork.
    // The suffix is added at runtime so a regenerated version.h keeps working.
    static const std::string version = std::string( VERSION ) + "_netplay";
    return version.c_str();
}
