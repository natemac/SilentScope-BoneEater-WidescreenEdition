#include "render/window_layout.h"
#include "render/auxiliary_windows.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstring>
#include <cwchar>
#include <memory>
#include <mutex>
#include <new>

namespace bone_eater::render {
namespace {

using Clock = std::chrono::steady_clock;

struct Output {
    HWND window = nullptr;
    unsigned int textureWidth = 0;
    unsigned int textureHeight = 0;
    RECT placement {};
    bool placed = false;
};

struct Layout {
    std::mutex mutex;
    std::array<Output, 3> outputs;
    HMONITOR monitor = nullptr;
    Clock::time_point nextCheck {};
};

Layout& layout() {
    static Layout value;
    return value;
}

int roleIndex(const char* role) noexcept {
    if (!role) return -1;
    if (std::strcmp(role, "main") == 0) return 0;
    if (std::strcmp(role, "multidisplay0") == 0) return 1;
    if (std::strcmp(role, "multidisplay1") == 0) return 2;
    return -1;
}

bool belongsToGame(HWND window) noexcept {
    DWORD process = 0;
    return window && GetWindowThreadProcessId(window, &process) &&
        process == GetCurrentProcessId();
}

bool sameRectangle(const RECT& a, const RECT& b) noexcept {
    return a.left == b.left && a.top == b.top &&
        a.right == b.right && a.bottom == b.bottom;
}

bool widePreviewEnabled() noexcept {
    static const bool enabled = [] {
        wchar_t value[32] {};
        const DWORD count = GetEnvironmentVariableW(L"BONE_EATER_LAYOUT", value,
            static_cast<DWORD>(std::size(value)));
        return count && count < std::size(value) &&
            (_wcsicmp(value, L"wide") == 0 || _wcsicmp(value, L"wide-monitor") == 0);
    }();
    return enabled;
}

bool mainMonitorEnabled() noexcept {
    static const bool enabled = [] {
        wchar_t value[32] {};
        const DWORD count = GetEnvironmentVariableW(L"BONE_EATER_LAYOUT", value,
            static_cast<DWORD>(std::size(value)));
        return count && count < std::size(value) && _wcsicmp(value, L"wide-monitor") == 0;
    }();
    return enabled;
}

bool fitWindow(const Output& output, const RECT& box, RECT& result) noexcept {
    const LONG boxWidth = box.right - box.left;
    const LONG boxHeight = box.bottom - box.top;
    if (boxWidth <= 0 || boxHeight <= 0 || !output.textureWidth || !output.textureHeight) {
        return false;
    }

    const auto style = static_cast<DWORD>(GetWindowLongPtrW(output.window, GWL_STYLE));
    const auto extendedStyle = static_cast<DWORD>(GetWindowLongPtrW(output.window, GWL_EXSTYLE));
    const BOOL menu = GetMenu(output.window) != nullptr;
    RECT frame {0, 0, 100, 100};
    // Resolve DPI-aware helpers dynamically so this does not raise the runtime's
    // minimum Windows version or depend on the parent translation unit's SDK macro.
    using GetDpi = UINT(WINAPI*)(HWND);
    using AdjustForDpi = BOOL(WINAPI*)(LPRECT, DWORD, BOOL, DWORD, UINT);
    static const auto user32 = GetModuleHandleW(L"user32.dll");
    static const auto getDpi = reinterpret_cast<GetDpi>(GetProcAddress(user32, "GetDpiForWindow"));
    static const auto adjustForDpi = reinterpret_cast<AdjustForDpi>(
        GetProcAddress(user32, "AdjustWindowRectExForDpi"));
    bool adjusted = false;
    if (getDpi && adjustForDpi) {
        const UINT dpi = getDpi(output.window);
        if (dpi) adjusted = adjustForDpi(&frame, style, menu, extendedStyle, dpi) != FALSE;
    }
    if (!adjusted) {
        frame = {0, 0, 100, 100};
        if (!AdjustWindowRectEx(&frame, style, menu, extendedStyle)) return false;
    }
    const LONG borderWidth = frame.right - frame.left - 100;
    const LONG borderHeight = frame.bottom - frame.top - 100;
    const LONG availableWidth = boxWidth - borderWidth;
    const LONG availableHeight = boxHeight - borderHeight;
    if (availableWidth <= 0 || availableHeight <= 0) return false;

    const double scale = std::min(
        static_cast<double>(availableWidth) / output.textureWidth,
        static_cast<double>(availableHeight) / output.textureHeight);
    const LONG clientWidth = std::max<LONG>(1, std::min(availableWidth,
        static_cast<LONG>(std::floor(output.textureWidth * scale))));
    const LONG clientHeight = std::max<LONG>(1, std::min(availableHeight,
        static_cast<LONG>(std::floor(output.textureHeight * scale))));
    const LONG width = clientWidth + borderWidth;
    const LONG height = clientHeight + borderHeight;
    result.left = box.left + (boxWidth - width) / 2;
    result.top = box.top + (boxHeight - height) / 2;
    result.right = result.left + width;
    result.bottom = result.top + height;
    return true;
}

DWORD WINAPI applyPlacements(void* context) noexcept {
    std::unique_ptr<std::array<Output, 3>> changes(
        static_cast<std::array<Output, 3>*>(context));
    for (const auto& change : *changes) {
        if (!belongsToGame(change.window)) continue;
        const auto& rect = change.placement;
        int x = 0, y = 0, width = 0, height = 0;
        // A newer metadata observation may have superseded this queued work.
        if (!diagnosticWindowPlacement(change.window, x, y, width, height) ||
                x != rect.left || y != rect.top || width != rect.right - rect.left ||
                height != rect.bottom - rect.top || IsIconic(change.window)) continue;
        // No layout lock is held here. A worker queued before scope parking
        // must not pull a currently managed auxiliary back onto the desktop.
        if (auxiliaryWindowManaged(change.window)) continue;
        SetWindowPos(change.window, nullptr, x, y, width, height,
            SWP_NOACTIVATE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_ASYNCWINDOWPOS);
    }
    return 0;
}

} // namespace

bool diagnosticWindowLayoutEnabled() noexcept {
    static const bool enabled = [] {
        wchar_t value[32] {};
        const DWORD count = GetEnvironmentVariableW(L"BONE_EATER_LAYOUT", value,
            static_cast<DWORD>(std::size(value)));
        return count && count < std::size(value) &&
            (_wcsicmp(value, L"diagnostic") == 0 || _wcsicmp(value, L"wide") == 0 ||
                _wcsicmp(value, L"wide-monitor") == 0);
    }();
    return enabled;
}

void observeDiagnosticWindow(void* window, const char* role,
                             unsigned int textureWidth,
                             unsigned int textureHeight) noexcept {
    if (!diagnosticWindowLayoutEnabled() || !textureWidth || !textureHeight) return;
    const int index = roleIndex(role);
    const auto hwnd = static_cast<HWND>(window);
    if (index < 0 || !belongsToGame(hwnd)) return;
    try {
        std::array<Output, 3> changes {};
        {
            auto& state = layout();
            std::lock_guard<std::mutex> guard(state.mutex);
            auto& output = state.outputs[static_cast<std::size_t>(index)];
            const bool changed = output.window != hwnd ||
                output.textureWidth != textureWidth || output.textureHeight != textureHeight;
            if (index == 0 && output.window != hwnd) state.monitor = nullptr;
            output.window = hwnd;
            output.textureWidth = textureWidth;
            output.textureHeight = textureHeight;
            const auto now = Clock::now();
            if (!changed && now < state.nextCheck) return;
            state.nextCheck = now + std::chrono::seconds(1);

            if (!belongsToGame(state.outputs[0].window)) return;
            MONITORINFO monitor {sizeof(MONITORINFO)};
            if (!state.monitor || !GetMonitorInfoW(state.monitor, &monitor)) {
                state.monitor = MonitorFromWindow(state.outputs[0].window, MONITOR_DEFAULTTONEAREST);
                if (!state.monitor || !GetMonitorInfoW(state.monitor, &monitor)) return;
            }
            constexpr LONG margin = 8;
            RECT work = monitor.rcWork;
            work.left += margin;
            work.top += margin;
            work.right -= margin;
            work.bottom -= margin;
            const LONG width = work.right - work.left;
            const LONG height = work.bottom - work.top;
            if (width < 64 || height < 64) return;
            const LONG divider = work.left + static_cast<LONG>(width * 0.6);
            const LONG middle = work.top + height / 2;
            const std::array<RECT, 3> boxes {{
                {work.left, work.top, divider - margin / 2, work.bottom},
                {divider + margin / 2, work.top, work.right, middle - margin / 2},
                {divider + margin / 2, middle + margin / 2, work.right, work.bottom}
            }};
            for (std::size_t slot = 0; slot < state.outputs.size(); ++slot) {
                auto& candidate = state.outputs[slot];
                if (!belongsToGame(candidate.window)) {
                    candidate.placed = false;
                    continue;
                }
                RECT desired {};
                Output sizing = candidate;
                if (slot == 0 && widePreviewEnabled()) {
                    // Experiment: let the engine handle a 16:9 client resize.
                    // This alone does not prove FOV, HUD, or aim correctness.
                    sizing.textureWidth = 16;
                    sizing.textureHeight = 9;
                }
                const RECT& box = slot == 0 && mainMonitorEnabled() ? monitor.rcMonitor : boxes[slot];
                if (!fitWindow(sizing, box, desired)) continue;
                candidate.placement = desired;
                candidate.placed = true;
                RECT current {};
                if (GetWindowRect(candidate.window, &current) &&
                        !sameRectangle(current, desired) && !IsIconic(candidate.window)) {
                    changes[slot] = candidate;
                }
            }
        }

        // Always submit from a pool thread. ASYNCWINDOWPOS alone is insufficient
        // if Present happens on the window-owner thread: Windows can otherwise
        // synchronously dispatch game resize handlers while telemetry is locked.
        // No placement work is queued when the windows already match the layout.
        const bool anyChange = std::any_of(changes.begin(), changes.end(),
            [](const Output& candidate) { return candidate.window != nullptr; });
        if (!anyChange) return;
        auto* work = new (std::nothrow) std::array<Output, 3>(changes);
        if (work && !QueueUserWorkItem(applyPlacements, work, WT_EXECUTEDEFAULT)) delete work;
    } catch (...) {
        // An optional diagnostic feature must not unwind into the render hook.
    }
}

bool diagnosticWindowPlacement(void* window, int& x, int& y,
                                int& width, int& height) noexcept {
    if (!diagnosticWindowLayoutEnabled() || !window) return false;
    try {
        auto& state = layout();
        std::lock_guard<std::mutex> guard(state.mutex);
        for (const auto& output : state.outputs) {
            if (!output.placed || output.window != window) continue;
            x = output.placement.left;
            y = output.placement.top;
            width = output.placement.right - output.placement.left;
            height = output.placement.bottom - output.placement.top;
            return true;
        }
    } catch (...) {
    }
    return false;
}

} // namespace bone_eater::render
