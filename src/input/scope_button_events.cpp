#include "input/scope_button_events.h"
#include "input/scope_button_event_policy.h"
#include <array>
#include <mutex>

namespace bone_eater::input {
namespace {
struct Shared {
    std::mutex mutex;
    ScopeButtonEventPolicy policy;
    std::array<int, 8> keys {};
    std::size_t count = 0;
    HWND window = nullptr;
};
Shared& shared() {
    // Window callbacks and input teardown can outlive static destruction.
    static Shared* value = new Shared;
    return *value;
}
int mouseButton(UINT message, WPARAM wparam) noexcept {
    switch (message) {
    case WM_LBUTTONDOWN: case WM_LBUTTONUP: case WM_LBUTTONDBLCLK: return VK_LBUTTON;
    case WM_RBUTTONDOWN: case WM_RBUTTONUP: case WM_RBUTTONDBLCLK: return VK_RBUTTON;
    case WM_MBUTTONDOWN: case WM_MBUTTONUP: case WM_MBUTTONDBLCLK: return VK_MBUTTON;
    case WM_XBUTTONDOWN: case WM_XBUTTONUP: case WM_XBUTTONDBLCLK:
        return HIWORD(wparam) == XBUTTON1 ? VK_XBUTTON1 :
            HIWORD(wparam) == XBUTTON2 ? VK_XBUTTON2 : 0;
    default: return 0;
    }
}
bool isMouseUp(UINT message) noexcept {
    return message == WM_LBUTTONUP || message == WM_RBUTTONUP ||
        message == WM_MBUTTONUP || message == WM_XBUTTONUP;
}
std::uint64_t eventTime(std::uint64_t now) noexcept {
    // Queued-message timestamps use the low32 uptime milliseconds. Sent
    // messages have no queued timestamp of their own; use callback receipt.
    if (InSendMessageEx(nullptr) != ISMEX_NOSEND) return now;
    const auto age = static_cast<DWORD>(now) - static_cast<DWORD>(GetMessageTime());
    return age <= now ? now - age : 0;
}
}

void configureScopeButtonEvents(const std::vector<int>& keys) noexcept {
    try {
        auto& s = shared(); std::lock_guard<std::mutex> lock(s.mutex);
        s.policy.invalidate(); s.count = 0;
        resetScopeControl();
        if (keys.empty() || keys.size() > s.keys.size()) return;
        for (const int key : keys) if (key <= 0 || key > 255) return;
        std::copy(keys.begin(), keys.end(), s.keys.begin());
        s.count = keys.size();
    } catch (...) {}
}
void resetScopeButtonEvents() noexcept {
    try {
        auto& s = shared(); std::lock_guard<std::mutex> lock(s.mutex);
        s.policy.invalidate(); resetScopeControl();
    } catch (...) {}
}
void observeScopeButtonMessage(HWND window, UINT message, WPARAM wparam, LPARAM lparam) noexcept {
    try {
        auto& s = shared(); std::lock_guard<std::mutex> lock(s.mutex);
        if (!s.count || !window) return;
        if (s.window != window) {
            s.window = window; s.policy.invalidate(); resetScopeControl();
        }
        if (message == WM_KILLFOCUS || message == WM_NCDESTROY ||
            (message == WM_ACTIVATEAPP && !wparam) ||
            (message == WM_ACTIVATE && LOWORD(wparam) == WA_INACTIVE)) {
            s.policy.invalidate(); resetScopeControl();
            if (message == WM_NCDESTROY) s.window = nullptr;
            return;
        }
        int key = 0; bool down = false;
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN ||
            message == WM_KEYUP || message == WM_SYSKEYUP) {
            if (wparam > 255) return;
            key = static_cast<int>(wparam);
            down = message == WM_KEYDOWN || message == WM_SYSKEYDOWN;
            if (down && (static_cast<std::uintptr_t>(lparam) & (std::uintptr_t{1} << 30))) return;
        } else {
            key = mouseButton(message, wparam);
            if (!key) return;
            down = !isMouseUp(message);
        }
        const auto tick = eventTime(GetTickCount64());
        for (std::size_t i = 0; i < s.count; ++i) if (s.keys[i] == key) {
            s.policy.observe(static_cast<unsigned>(i), down, tick); break;
        }
    } catch (...) {}
}
ScopeControlSnapshot consumeScopeButtonEvents(const ScopeControlInput& request) noexcept {
    try {
        auto& s = shared(); std::lock_guard<std::mutex> lock(s.mutex);
        auto routed = request;
        // A window event can arrive after the game thread sampled request.nowMs
        // but before it acquired this queue lock. Certify time under the lock
        // rather than misclassifying that genuine event as a future message.
        routed.nowMs = (std::max)(request.nowMs, static_cast<std::uint64_t>(GetTickCount64()));
        if (!s.count || !s.window) routed.usable = false;
        std::uint32_t asyncHeld = 0;
        for (std::size_t i = 0; i < s.count; ++i)
            if (GetAsyncKeyState(s.keys[i]) & 0x8000) asyncHeld |= std::uint32_t{1} << i;
        return s.policy.consume(routed, asyncHeld,
            [](const ScopeControlInput& input) noexcept { return publishScopeControl(input); });
    } catch (...) { resetScopeControl(); return {}; }
}
} // namespace bone_eater::input
