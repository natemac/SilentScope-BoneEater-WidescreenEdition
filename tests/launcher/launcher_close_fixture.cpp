// Hidden inert child. Uses the production registration/worker protocol, but
// its callback requests only this fixture's own normal message-loop exit.
#include "../../src/render/native_window_exit.cpp"
#include <string>

namespace bone_eater::input {
void observeScopeButtonMessage(HWND, UINT, WPARAM, LPARAM) noexcept {}
}

namespace {
HWND mainWindow = nullptr;
bool stayRunning = false;
void fixtureShutdown() { if (!stayRunning) PostMessageW(mainWindow, WM_APP + 1, 0, 0); }
LRESULT CALLBACK procedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    if (message == WM_APP + 1) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(window, message, wparam, lparam);
}
}
int wmain(int argc, wchar_t** argv) {
    if (argc != 4) return 20;
    const std::wstring mode = argv[3];
    stayRunning = mode == L"pending" || mode == L"hung";
    HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[1]);
    HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[2]);
    if (!ready || !release) return 21;
    WNDCLASSW type {}; type.lpfnWndProc = procedure; type.hInstance = GetModuleHandleW(nullptr);
    type.lpszClassName = L"AskaWnd";
    if (!RegisterClassW(&type)) return 22;
    mainWindow = CreateWindowExW(0, type.lpszClassName, L"ASKA", WS_OVERLAPPED,
        0, 0, 100, 100, nullptr, nullptr, type.hInstance, nullptr);
    if (!mainWindow) return 23;
    if (mode != L"old" && !bone_eater::render::installNativeMainWindowExit(mainWindow, fixtureShutdown)) return 24;
    HWND extra = nullptr;
    if (mode == L"ambiguous") {
        extra = CreateWindowExW(0, type.lpszClassName, L"ASKA", WS_OVERLAPPED,
            0, 0, 100, 100, nullptr, nullptr, type.hInstance, nullptr);
        if (!extra || !SetPropW(extra, bone_eater::render::window_exit_protocol::property, reinterpret_cast<HANDLE>(1))) return 25;
    }
    if (mode == L"stale") {
        const auto cookie = reinterpret_cast<UINT_PTR>(GetPropW(mainWindow, bone_eater::render::window_exit_protocol::property));
        if (!SetPropW(mainWindow, bone_eater::render::window_exit_protocol::property, reinterpret_cast<HANDLE>(cookie ^ 1))) return 26;
    }
    if (mode == L"scaled" && !SetWindowTextW(mainWindow, L"ASKA (x0.59 : ダブルクリック)")) return 27;
    SetEvent(ready);
    if (mode == L"hung") Sleep(500); // Bounded fixture-only deliberate non-pumping interval.
    const ULONGLONG deadline = GetTickCount64() + 5000;
    bool running = true;
    while (running && GetTickCount64() < deadline) {
        const DWORD state = MsgWaitForMultipleObjects(1, &release, FALSE, 100, QS_ALLINPUT);
        if (state == WAIT_OBJECT_0) break;
        MSG message {};
        while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            if (message.message == WM_QUIT) { running = false; break; }
            TranslateMessage(&message); DispatchMessageW(&message);
        }
    }
    if (extra) DestroyWindow(extra);
    DestroyWindow(mainWindow);
    CloseHandle(ready); CloseHandle(release);
    return 0;
}
