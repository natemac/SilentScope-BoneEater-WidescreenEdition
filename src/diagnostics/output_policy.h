#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace bone_eater::diagnostics {

// Read once, after standalone command-line parsing. Quiet wins over optional
// tracing/capture/timing flags; startup messages and failures use their existing
// paths and do not consult this policy. Functional observers must stay active.
inline bool optionalOutputEnabled() noexcept {
    static const bool enabled = [] {
        wchar_t setting[4] {};
        return !(GetEnvironmentVariableW(L"BONE_EATER_QUIET_DIAGNOSTICS", setting, 4) == 1 &&
            setting[0] == L'1');
    }();
    return enabled;
}

} // namespace bone_eater::diagnostics
