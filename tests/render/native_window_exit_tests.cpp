// Real inert windows, no game, no installed external hook, no shutdown call.
// Only scheduling, installation failure and reported PID can be substituted.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <commctrl.h>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>
#include "input/scope_button_events.h"

namespace bone_eater::input {
// The gesture policy has its own tests. This fixture only isolates the existing
// cooperative-close callback from input publication and native game state.
void observeScopeButtonMessage(HWND, UINT, WPARAM, LPARAM) noexcept {}
}

namespace fixture {
bool failInstall = false, failQueue = false, foreignPid = false;
unsigned queueCalls = 0, shutdownCalls = 0, nativeCloses = 0, otherDestroys = 0;
unsigned removedProperties = 0;
DWORD shutdownThread = 0;
LPTHREAD_START_ROUTINE queuedCallback = nullptr;
void* queuedContext = nullptr;
BOOL WINAPI install(HWND window, SUBCLASSPROC callback, UINT_PTR identifier, DWORD_PTR reference) {
    return failInstall ? FALSE : SetWindowSubclass(window, callback, identifier, reference);
}
BOOL WINAPI queue(LPTHREAD_START_ROUTINE callback, PVOID context, ULONG) {
    ++queueCalls;
    if (failQueue) return FALSE;
    if (queuedCallback) return FALSE;
    queuedCallback = callback; queuedContext = context;
    return TRUE;
}
DWORD WINAPI windowThread(HWND window, DWORD* process) {
    const auto thread = GetWindowThreadProcessId(window, process);
    if (foreignPid && process) *process = GetCurrentProcessId() + 1;
    return thread;
}
HANDLE WINAPI removeProperty(HWND window, LPCWSTR name) {
    ++removedProperties;
    return RemovePropW(window, name);
}
void shutdown() { ++shutdownCalls; shutdownThread = GetCurrentThreadId(); }
void throwingShutdown() { shutdown(); throw std::runtime_error("inert fixture callback"); }
void drain() {
    const auto callback = queuedCallback; const auto context = queuedContext;
    queuedCallback = nullptr; queuedContext = nullptr;
    if (callback) { std::thread worker([=] { callback(context); }); worker.join(); }
}
LRESULT CALLBACK windowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_CLOSE) { ++nativeCloses; return 73; }
    if (message == WM_KEYDOWN || message == WM_KEYUP) return 92;
    if (message == WM_APP + 1) return 91;
    return DefWindowProcW(window, message, wparam, lparam);
}
LRESULT CALLBACK otherSubclass(HWND window, UINT message, WPARAM wparam, LPARAM lparam, UINT_PTR, DWORD_PTR) {
    if (message == WM_NCDESTROY) ++otherDestroys;
    return DefSubclassProc(window, message, wparam, lparam);
}
}
#define SetWindowSubclass fixture::install
#define QueueUserWorkItem fixture::queue
#define GetWindowThreadProcessId fixture::windowThread
#define RemovePropW fixture::removeProperty
#include "../../src/render/native_window_exit.cpp"
#undef RemovePropW
#undef GetWindowThreadProcessId
#undef QueueUserWorkItem
#undef SetWindowSubclass

using namespace bone_eater::render;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)
struct Window {
    HWND value = nullptr;
    explicit Window(const wchar_t* title = L"ASKA") {
        value = CreateWindowExW(0, L"AskaWnd", title, WS_OVERLAPPED,
            0, 0, 100, 100, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        CHECK(value != nullptr);
    }
    ~Window() { if (value && IsWindow(value)) DestroyWindow(value); }
    void destroy() { CHECK(DestroyWindow(value)); value = nullptr; }
};
void resetFixture() {
    fixture::drain();
    CHECK(activeBinding.load() == nullptr);
    shutdownRequested.store(false);
    fixture::failInstall = fixture::failQueue = fixture::foreignPid = false;
    fixture::queueCalls = fixture::shutdownCalls = fixture::nativeCloses = fixture::otherDestroys = 0;
    fixture::shutdownThread = 0;
    fixture::removedProperties = 0;
}
int main() {
    unsigned cases = 0;
    try {
        WNDCLASSW type {}; type.lpfnWndProc = fixture::windowProc;
        type.hInstance = GetModuleHandleW(nullptr); type.lpszClassName = L"AskaWnd";
        CHECK(RegisterClassW(&type));
        { resetFixture(); Window main, auxiliary(L"Aska MultiDisplay[0](multipssID:33)");
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          CHECK(SendMessageW(auxiliary.value, WM_KEYDOWN, VK_ESCAPE, 0) == 92);
          CHECK(SendMessageW(main.value, WM_KEYDOWN, VK_RETURN, 0) == 92);
          CHECK(fixture::queueCalls == 0);
          CHECK(SendMessageW(main.value, WM_KEYDOWN, VK_ESCAPE, 0) == 0);
          CHECK(SendMessageW(main.value, WM_KEYDOWN, VK_ESCAPE, 1LL << 30) == 0);
          CHECK(SendMessageW(main.value, WM_KEYUP, VK_ESCAPE, 0) == 0);
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0);
          CHECK(fixture::queueCalls == 1 && fixture::nativeCloses == 0);
          fixture::drain(); CHECK(fixture::shutdownCalls == 1); ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          fixture::failQueue = true;
          CHECK(SendMessageW(main.value, WM_KEYDOWN, VK_ESCAPE, 0) == 73);
          CHECK(!shutdownRequested.load() && fixture::nativeCloses == 1);
          fixture::failQueue = false;
          CHECK(SendMessageW(main.value, WM_KEYDOWN, VK_ESCAPE, 0) == 0);
          fixture::drain(); CHECK(fixture::shutdownCalls == 1); ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          CHECK(SendMessageW(main.value, WM_APP + 1, 0, 0) == 91);
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0);
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0);
          CHECK(fixture::queueCalls == 1 && fixture::shutdownCalls == 0 && fixture::nativeCloses == 0);
          fixture::drain();
          CHECK(fixture::shutdownCalls == 1 && fixture::shutdownThread != GetCurrentThreadId());
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0 && fixture::queueCalls == 1); ++cases; }
        { resetFixture(); Window main, auxiliary(L"Aska MultiDisplay[0](multipssID:33)");
          CHECK(!installNativeMainWindowExit(nullptr, fixture::shutdown));
          CHECK(!installNativeMainWindowExit(main.value, nullptr));
          CHECK(!installNativeMainWindowExit(auxiliary.value, fixture::shutdown));
          CHECK(SendMessageW(auxiliary.value, WM_CLOSE, 0, 0) == 73 && fixture::queueCalls == 0); ++cases; }
        { resetFixture(); Window main;
          fixture::foreignPid = true;
          CHECK(!installNativeMainWindowExit(main.value, fixture::shutdown));
          fixture::foreignPid = false;
          bool installed = true;
          std::thread wrongThread([&] { installed = installNativeMainWindowExit(main.value, fixture::shutdown); });
          wrongThread.join(); CHECK(!installed && activeBinding.load() == nullptr); ++cases; }
        { resetFixture(); Window main;
          fixture::failInstall = true;
          CHECK(!installNativeMainWindowExit(main.value, fixture::shutdown) && !activeBinding.load());
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 73 && fixture::queueCalls == 0);
          fixture::failInstall = false;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown)); ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown)); fixture::failQueue = true;
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 73);
          CHECK(fixture::queueCalls == 1 && !shutdownRequested.load() && fixture::nativeCloses == 1);
          fixture::failQueue = false;
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0 && fixture::queueCalls == 2);
          fixture::drain(); CHECK(fixture::shutdownCalls == 1); ++cases; }
        { resetFixture(); Window main;
          CHECK(SetWindowSubclass(main.value, fixture::otherSubclass, 123, 456));
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          DWORD_PTR reference = 0;
          CHECK(GetWindowSubclass(main.value, fixture::otherSubclass, 123, &reference) && reference == 456);
          main.destroy(); CHECK(activeBinding.load() == nullptr && fixture::otherDestroys == 1);
          Window replacement;
          CHECK(installNativeMainWindowExit(replacement.value, fixture::shutdown)); ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0);
          main.destroy(); CHECK(activeBinding.load() == nullptr);
          fixture::drain(); CHECK(fixture::shutdownCalls == 1 && shutdownRequested.load()); ++cases; }
        { resetFixture(); Window first, second;
          CHECK(installNativeMainWindowExit(first.value, fixture::shutdown));
          CHECK(!installNativeMainWindowExit(second.value, fixture::shutdown));
          CHECK(SendMessageW(second.value, WM_CLOSE, 0, 0) == 73 && fixture::queueCalls == 0);
          CHECK(SetWindowTextW(first.value, L"ASKA (scaled)"));
          CHECK(SendMessageW(first.value, WM_CLOSE, 0, 0) == 0);
          fixture::drain(); CHECK(fixture::shutdownCalls == 1); ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown)); fixture::foreignPid = true;
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 73 && fixture::queueCalls == 0);
          fixture::foreignPid = false; ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::throwingShutdown));
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0);
          fixture::drain(); CHECK(fixture::shutdownCalls == 1 && shutdownRequested.load());
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0 && fixture::queueCalls == 1); ++cases; }
        { resetFixture(); Window main; const HWND stale = main.value; main.destroy();
          CHECK(!installNativeMainWindowExit(stale, fixture::shutdown)); ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          const auto* binding = activeBinding.load();
          CHECK(binding->cookie != 0 && reinterpret_cast<UINT_PTR>(GetPropW(main.value, window_exit_protocol::property)) == binding->cookie);
          CHECK(SendMessageW(main.value, binding->requestMessage, binding->cookie ^ 1, window_exit_protocol::version) == 0);
          CHECK(SendMessageW(main.value, binding->requestMessage, binding->cookie, 2) == 0);
          CHECK(fixture::queueCalls == 0 && fixture::nativeCloses == 0);
          CHECK(SetWindowTextW(main.value, L"ASKA (x0.59 : ダブルクリック)"));
          CHECK(SendMessageW(main.value, binding->requestMessage, binding->cookie, window_exit_protocol::version) == window_exit_protocol::accepted);
          CHECK(SendMessageW(main.value, binding->requestMessage, binding->cookie, window_exit_protocol::version) == window_exit_protocol::accepted);
          CHECK(fixture::queueCalls == 1); fixture::drain();
          main.destroy(); CHECK(fixture::removedProperties == 1); ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          const auto* binding = activeBinding.load(); fixture::failQueue = true;
          CHECK(SendMessageW(main.value, binding->requestMessage, binding->cookie, window_exit_protocol::version) == 0);
          CHECK(!shutdownRequested.load() && fixture::nativeCloses == 0);
          fixture::failQueue = false;
          CHECK(SendMessageW(main.value, binding->requestMessage, binding->cookie, window_exit_protocol::version) == window_exit_protocol::accepted);
          fixture::drain(); CHECK(fixture::shutdownCalls == 1); ++cases; }
        { resetFixture(); Window main;
          CHECK(installNativeMainWindowExit(main.value, fixture::shutdown));
          const auto* binding = activeBinding.load();
          CHECK(SetPropW(main.value, window_exit_protocol::property, reinterpret_cast<HANDLE>(binding->cookie ^ 1)));
          CHECK(SendMessageW(main.value, binding->requestMessage, binding->cookie, window_exit_protocol::version) == 0);
          CHECK(SendMessageW(main.value, binding->requestMessage, binding->cookie ^ 1, window_exit_protocol::version) == 0);
          CHECK(fixture::queueCalls == 0);
          CHECK(SendMessageW(main.value, WM_CLOSE, 0, 0) == 0); fixture::drain();
          main.destroy(); CHECK(fixture::removedProperties == 0 && fixture::shutdownCalls == 1); ++cases; }
        { resetFixture(); UINT_PTR oldCookie = 0;
          { Window first; CHECK(installNativeMainWindowExit(first.value, fixture::shutdown));
            oldCookie = activeBinding.load()->cookie; }
          Window next; CHECK(installNativeMainWindowExit(next.value, fixture::shutdown));
          const auto* binding = activeBinding.load(); CHECK(binding->cookie != oldCookie);
          CHECK(SendMessageW(next.value, binding->requestMessage, oldCookie, window_exit_protocol::version) == 0);
          CHECK(fixture::queueCalls == 0); ++cases; }
        { resetFixture();
          CHECK(window_exit_protocol::mainTitle(L"ASKA"));
          CHECK(window_exit_protocol::mainTitle(L"ASKA (x 0.5 : localized hint)"));
          CHECK(!window_exit_protocol::mainTitle(L"ASKA (xno : hint)"));
          CHECK(!window_exit_protocol::mainTitle(L"ASKA (x0.5 : )"));
          CHECK(!window_exit_protocol::mainTitle(L"ASKA scope"));
          Window main; CHECK(SetPropW(main.value, window_exit_protocol::property, reinterpret_cast<HANDLE>(1)));
          CHECK(!installNativeMainWindowExit(main.value, fixture::shutdown));
          CHECK(GetPropW(main.value, window_exit_protocol::property) == reinterpret_cast<HANDLE>(1)); ++cases; }
        resetFixture(); CHECK(UnregisterClassW(type.lpszClassName, type.hInstance));
        std::cout << "Passed " << cases << " native main-window exit groups; no game or real shutdown used\n";
    } catch (const std::exception& error) {
        fixture::drain();
        std::cerr << "After " << cases << " groups: " << error.what() << '\n'; return 1;
    }
}
