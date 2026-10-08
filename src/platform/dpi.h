#pragma once
#include <windows.h>
#include <string>
namespace bone_eater {
// Establish physical-pixel coordinates before creating windows or reading monitors.
inline std::string enablePhysicalPixelDpi() {
    const auto user = GetModuleHandleW(L"user32.dll");
    using SetContext = BOOL(WINAPI*)(HANDLE);
    using GetContext = HANDLE(WINAPI*)();
    using Awareness = int(WINAPI*)(HANDLE);
    const auto set = reinterpret_cast<SetContext>(GetProcAddress(user, "SetProcessDpiAwarenessContext"));
    const bool applied = set ? set(reinterpret_cast<HANDLE>(-4)) != FALSE : SetProcessDPIAware() != FALSE;
    const auto error = applied ? ERROR_SUCCESS : GetLastError();
    const auto get = reinterpret_cast<GetContext>(GetProcAddress(user, "GetThreadDpiAwarenessContext"));
    const auto awareness = reinterpret_cast<Awareness>(GetProcAddress(user, "GetAwarenessFromDpiAwarenessContext"));
    const int effective = get && awareness ? awareness(get()) : (IsProcessDPIAware() ? 1 : 0);
    return "DPI: requested PerMonitorV2; applied=" + std::to_string(applied) +
        "; effective awareness=" + std::to_string(effective) + " (0=unaware,1=system,2=per-monitor); error=" + std::to_string(error);
}
}
