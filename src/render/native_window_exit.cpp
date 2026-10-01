#include "render/native_window_exit.h"
#include "render/window_exit_protocol.h"
#include "input/scope_button_events.h"
#include <commctrl.h>
#include <bcrypt.h>
#include <atomic>
#include <cstdint>
#include <cwchar>
#include <new>
#include "util/logging.h"

namespace bone_eater::render {
namespace {
constexpr UINT_PTR kMainExitSubclass = 0x42454E44; // Private callback/ID pair.
struct Binding {
    HWND window;
    DWORD ownerThread;
    NativeWindowShutdown shutdown;
    UINT_PTR cookie;
    UINT requestMessage;
};
struct ShutdownWork {
    NativeWindowShutdown shutdown;
};
// Binding access/detachment is confined to its owning window thread. Other
// installers only compare the pointer atomically and never dereference it.
std::atomic<Binding*> activeBinding {nullptr};
std::atomic<bool> shutdownRequested {false};

void warning(const char* message) noexcept {
    try { log_warning("bone-eater", "Main-window exit: {}", message); }
    catch (...) {}
}

DWORD WINAPI runShutdown(void* context) noexcept {
    const auto* work = static_cast<ShutdownWork*>(context);
    const auto shutdown = work->shutdown;
    delete work;
    // The queued work holds only a process-lifetime callback, never a Binding
    // or HWND. Destroying the window before this worker starts is safe.
    try { shutdown(); }
    catch (...) { warning("existing shutdown callback raised; no additional termination was attempted"); }
    // The native routine normally exits the process. Do not schedule another
    // teardown if it unexpectedly returns or throws after partial cleanup.
    return 0;
}

LRESULT CALLBACK mainWindowSubclass(HWND window, UINT message, WPARAM wparam,
        LPARAM lparam, UINT_PTR identifier, DWORD_PTR reference) noexcept {
    auto* binding = reinterpret_cast<Binding*>(reference);
    if (!binding || identifier != kMainExitSubclass || binding->window != window ||
            binding->ownerThread != GetCurrentThreadId())
        return DefSubclassProc(window, message, wparam, lparam);

    // Observe input only from our verified current main window. All messages
    // continue through the native handler; scope routing never steals UI input.
    if (activeBinding.load() == binding)
        input::observeScopeButtonMessage(window, message, wparam, lparam);

    if (message == WM_NCDESTROY) {
        if (reinterpret_cast<UINT_PTR>(GetPropW(window, window_exit_protocol::property)) == binding->cookie)
            RemovePropW(window, window_exit_protocol::property);
        const bool removed = RemoveWindowSubclass(window, mainWindowSubclass, kMainExitSubclass) != FALSE;
        Binding* expected = binding;
        activeBinding.compare_exchange_strong(expected, nullptr);
        const auto result = DefSubclassProc(window, message, wparam, lparam);
        if (removed) delete binding;
        else {
            // Retain the small record if removal failed; a possible late
            // callback must never observe freed reference data. It is inert
            // because it no longer matches activeBinding.
            warning("subclass removal failed during window destruction; retained inactive binding");
        }
        return result;
    }
    const bool cooperative = message == binding->requestMessage;
    if (cooperative) {
        wchar_t title[256] {}, type[80] {};
        if (activeBinding.load() != binding || wparam != binding->cookie ||
                lparam != window_exit_protocol::version ||
                reinterpret_cast<UINT_PTR>(GetPropW(window, window_exit_protocol::property)) != binding->cookie ||
                GetClassNameW(window, type, 80) <= 0 || std::wcscmp(type, window_exit_protocol::windowClass) != 0 ||
                GetWindowTextW(window, title, 256) <= 0 || !window_exit_protocol::mainTitle(title)) return 0;
    }
    if ((!cooperative && message != WM_CLOSE) || activeBinding.load() != binding)
        return DefSubclassProc(window, message, wparam, lparam);

    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (thread != binding->ownerThread || process != GetCurrentProcessId()) {
        if (cooperative) return 0;
        return DefSubclassProc(window, message, wparam, lparam);
    }
    if (shutdownRequested.load()) return cooperative ? window_exit_protocol::accepted : 0;

    auto* work = new (std::nothrow) ShutdownWork {binding->shutdown};
    if (!work) {
        warning("cannot allocate shutdown work; preserving native WM_CLOSE handling");
        if (cooperative) return 0;
        return DefSubclassProc(window, message, wparam, lparam);
    }
    bool expected = false;
    if (!shutdownRequested.compare_exchange_strong(expected, true)) {
        delete work;
        return cooperative ? window_exit_protocol::accepted : 0;
    }
    if (!QueueUserWorkItem(runShutdown, work, WT_EXECUTEDEFAULT)) {
        delete work;
        shutdownRequested.store(false);
        warning("cannot queue shutdown; preserving native WM_CLOSE handling");
        if (cooperative) return 0;
        return DefSubclassProc(window, message, wparam, lparam);
    }
    try { log_info("bone-eater", "Main-window WM_CLOSE accepted; existing runtime shutdown queued once"); }
    catch (...) {}
    return cooperative ? window_exit_protocol::accepted : 0;
}
}

bool installNativeMainWindowExit(HWND window, NativeWindowShutdown shutdown) noexcept {
    if (!window || !shutdown || shutdownRequested.load()) return false;
    DWORD process = 0;
    const DWORD thread = GetWindowThreadProcessId(window, &process);
    if (!thread || process != GetCurrentProcessId() || thread != GetCurrentThreadId()) {
        warning("installation requires the current process's window-owning thread");
        return false;
    }
    // This is a creation-time check, not a later global title search. Native
    // scaling may change the title after registration without changing identity.
    wchar_t title[8] {};
    if (GetWindowTextW(window, title, 8) != 4 || std::wcscmp(title, L"ASKA") != 0) return false;
    wchar_t type[80] {};
    if (GetClassNameW(window, type, 80) <= 0 || std::wcscmp(type, window_exit_protocol::windowClass) != 0) return false;

    DWORD_PTR installed = 0;
    if (GetWindowSubclass(window, mainWindowSubclass, kMainExitSubclass, &installed)) {
        const auto* binding = reinterpret_cast<Binding*>(installed);
        // This HWND is owned by this thread, so its callback cannot be removed
        // concurrently here. Never overwrite an existing callback/identity.
        return binding && activeBinding.load() == binding && binding->window == window && binding->shutdown == shutdown &&
            reinterpret_cast<UINT_PTR>(GetPropW(window, window_exit_protocol::property)) == binding->cookie;
    }
    if (GetPropW(window, window_exit_protocol::property)) return false;
    const UINT requestMessage = RegisterWindowMessageW(window_exit_protocol::message);
    UINT_PTR cookie = 0;
    if (!requestMessage || BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(&cookie), sizeof(cookie),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0 || !cookie) {
        warning("cannot create close-request identity; native window handling retained");
        return false;
    }
    auto* binding = new (std::nothrow) Binding {window, thread, shutdown, cookie, requestMessage};
    if (!binding) { warning("cannot allocate main-window binding"); return false; }
    Binding* expected = nullptr;
    if (!activeBinding.compare_exchange_strong(expected, binding)) {
        delete binding;
        warning("a different main window is already registered");
        return false;
    }
    if (!SetPropW(window, window_exit_protocol::property, reinterpret_cast<HANDLE>(cookie)) ||
            !SetWindowSubclass(window, mainWindowSubclass, kMainExitSubclass, reinterpret_cast<DWORD_PTR>(binding))) {
        if (reinterpret_cast<UINT_PTR>(GetPropW(window, window_exit_protocol::property)) == cookie)
            RemovePropW(window, window_exit_protocol::property);
        expected = binding;
        activeBinding.compare_exchange_strong(expected, nullptr);
        delete binding;
        warning("subclass installation failed; native window handling retained");
        return false;
    }
    input::observeScopeButtonMessage(window, WM_NULL, 0, 0);
    try { log_info("bone-eater", "Main-window exit registered hwnd=0x{:x} owner_thread={}",
        reinterpret_cast<std::uintptr_t>(window), thread); }
    catch (...) {}
    return true;
}

} // namespace bone_eater::render
