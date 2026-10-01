#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <iostream>
#include <string>

int wmain(int argc, wchar_t** argv) {
    if (argc == 4 && std::wstring(argv[1]) == L"--wait-event") {
        HANDLE ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, argv[2]);
        HANDLE release = OpenEventW(SYNCHRONIZE, FALSE, argv[3]);
        if (!ready || !release) return 30;
        SetEvent(ready);
        const auto result = WaitForSingleObject(release, 5000);
        CloseHandle(ready); CloseHandle(release);
        return result == WAIT_OBJECT_0 ? 0 : 31;
    }
    if (argc == 2 && std::wstring(argv[1]) == L"--exit-42") {
        std::cerr << "fixture startup failure detail\n";
        return 42;
    }
    const wchar_t* expected[] {L"BoneEater.exe", L"-2Display", L"space 雪 path", L"C:\\tail space\\",
        L"literal\"quote", L"", L"line\nbreak"};
    if (argc != 7) return 10;
    for (int i = 0; i < argc; ++i) if (std::wstring(argv[i]) != expected[i]) return 11 + i;
    if (std::wstring(GetCommandLineW()).find(L"BoneEater.exe -2Display ") != 0) return 20;
    if (GetConsoleWindow()) return 21;
    wchar_t value[100] {};
    if (!GetEnvironmentVariableW(L"LAUNCHER_FIXTURE_KEEP", value, 100) || std::wstring(value) != L"kept-雪") return 22;
    for (const auto* name : {L"BONE_EATER_CAPTURE", L"BONE_EATER_BATTLE_BACKGROUND_FILTER", L"BONE_EATER_NATIVE_MOVIE_FIT"})
        if (GetEnvironmentVariableW(name, value, 100)) return 23;
    wchar_t directory[32768] {};
    if (!GetCurrentDirectoryW(32768, directory) || std::wstring(directory).find(L"fixture 雪 directory") == std::wstring::npos) return 24;
    std::cout << "fixture: exact Unicode argv, short argv0, isolated environment, working directory and no console passed\n";
    std::cerr << "fixture: stderr captured\n";
    return 0;
}
