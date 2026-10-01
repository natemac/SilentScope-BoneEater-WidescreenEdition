#include "input/native_aim.h"
#include "input/native_wide_input.h"
#include "input/native_input_ownership.h"
#include "input/native_precision_bypass.h"
#include "input/scope_control.h"
#include "render/native_viewport.h"
#include "util/detour.h"
#include "util/logging.h"

#include <bcrypt.h>
#include <intrin.h>
#include <cwchar>
#include <limits>
#include <vector>
#include <atomic>

namespace bone_eater::input {
namespace {

using NativeGetInput = std::int32_t(__fastcall*)(void*);
using NativeScreenToRay = void*(__fastcall*)(void*, void*, float, float);
constexpr std::uintptr_t kExportRva = 0x3370;
constexpr std::uintptr_t kCallerRva = 0x88C80;
constexpr std::uintptr_t kReturnRva = 0x88C86; // FF 15 disp32 is six bytes.
constexpr DWORD kArkImageSize = 0x3F7000;
constexpr std::uint64_t kArkFileSize = 1681040;
constexpr char kArkSha256[] =
    "f8da7add776feb9dde20724ddd80112a6315a07d9856878939c0f7a52fb93e98";
NativeGetInput originalGetInput = nullptr;
NativeGetInput originalStartTrigger = nullptr;
std::uintptr_t acceptedStartCaller = 0;
std::atomic<bool> startRoutingReady {false};
NativeScreenToRay originalScreenToRay = nullptr;
std::uintptr_t acceptedCaller = 0;
std::uintptr_t acceptedRayCaller = 0;
std::uintptr_t expectedCameraVtable = 0;
std::uintptr_t arkDimensions = 0;
std::uintptr_t scopeOffGlobal = 0;
std::uintptr_t expectedScopeOffVtable = 0;
bool installAttempted = false;

NativeAimState& state() {
    // Intentionally process-lifetime: a hook may run during static teardown.
    static NativeAimState* value = new NativeAimState;
    return *value;
}

NativeRayState& rayState() {
    static NativeRayState* value = new NativeRayState;
    return *value;
}

bool readBytes(std::uintptr_t address, void* output, std::size_t size) noexcept {
    if (!address || address > std::numeric_limits<std::uintptr_t>::max() - size) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address),
        output, size, &copied) && copied == size;
}

template<typename T>
bool readAt(std::uintptr_t base, std::size_t offset, T& output) noexcept {
    return base && base <= std::numeric_limits<std::uintptr_t>::max() - offset &&
        readBytes(base + offset, &output, sizeof(output));
}

struct ScopeOffObservation { bool known = false; bool off = true; };

ScopeOffObservation readScopeOff() noexcept {
    std::uintptr_t holder = 0, ui = 0, vtable = 0;
    std::uint8_t off = 0;
    if (!readAt(scopeOffGlobal, 0, holder) || !readAt(holder, 0, ui) ||
        !readAt(ui, 0, vtable) || vtable != expectedScopeOffVtable ||
        !readAt(ui, 0x558, off) || off > 1) return {};
    std::uintptr_t holderAfter = 0, uiAfter = 0, vtableAfter = 0;
    std::uint8_t offAfter = 0;
    if (!readAt(scopeOffGlobal, 0, holderAfter) || holderAfter != holder ||
        !readAt(holder, 0, uiAfter) || uiAfter != ui ||
        !readAt(ui, 0, vtableAfter) || vtableAfter != vtable ||
        !readAt(ui, 0x558, offAfter) || offAfter != off) return {};
    return {true, off != 0};
}

template<std::size_t N>
bool matches(std::uintptr_t address, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> found {};
    return readBytes(address, found.data(), found.size()) && found == expected;
}

struct FileHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~FileHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
};

struct HashHandles {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    std::vector<unsigned char> object;
    ~HashHandles() {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    }
};

bool verifiedArkFile(HMODULE module) {
    std::vector<wchar_t> path(32768);
    const DWORD count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (!count || count >= path.size()) return false;
    FileHandle file;
    file.value = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
        nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) return false;
    LARGE_INTEGER size {};
    if (!GetFileSizeEx(file.value, &size) || size.QuadPart != kArkFileSize) return false;
    HashHandles hash;
    if (BCryptOpenAlgorithmProvider(&hash.algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) return false;
    DWORD objectSize = 0, digestSize = 0, bytes = 0;
    if (BCryptGetProperty(hash.algorithm, BCRYPT_OBJECT_LENGTH,
            reinterpret_cast<PUCHAR>(&objectSize), sizeof(objectSize), &bytes, 0) < 0 ||
        BCryptGetProperty(hash.algorithm, BCRYPT_HASH_LENGTH,
            reinterpret_cast<PUCHAR>(&digestSize), sizeof(digestSize), &bytes, 0) < 0 ||
        !objectSize || objectSize > 1024 * 1024 || digestSize != 32) return false;
    hash.object.resize(objectSize);
    if (BCryptCreateHash(hash.algorithm, &hash.hash, hash.object.data(), objectSize, nullptr, 0, 0) < 0) return false;
    std::array<unsigned char, 65536> block {};
    std::uint64_t total = 0;
    for (;;) {
        DWORD countRead = 0;
        if (!ReadFile(file.value, block.data(), static_cast<DWORD>(block.size()), &countRead, nullptr)) return false;
        if (!countRead) break;
        total += countRead;
        if (total > kArkFileSize || BCryptHashData(hash.hash, block.data(), countRead, 0) < 0) return false;
    }
    if (total != kArkFileSize) return false;
    std::array<unsigned char, 32> digest {};
    if (BCryptFinishHash(hash.hash, digest.data(), static_cast<ULONG>(digest.size()), 0) < 0) return false;
    constexpr char digits[] = "0123456789abcdef";
    char actual[65] {};
    for (std::size_t i = 0; i < digest.size(); ++i) {
        actual[i * 2] = digits[digest[i] >> 4];
        actual[i * 2 + 1] = digits[digest[i] & 15];
    }
    return std::strcmp(actual, kArkSha256) == 0;
}

bool verifiedArk(HMODULE module) {
    if (!module || GetModuleHandleW(L"arkndd.dll") != module) return false;
    const auto base = reinterpret_cast<std::uintptr_t>(module);
    IMAGE_DOS_HEADER dos {};
    IMAGE_NT_HEADERS64 pe {};
    if (!readBytes(base, &dos, sizeof(dos)) || dos.e_magic != IMAGE_DOS_SIGNATURE ||
        dos.e_lfanew < sizeof(dos) || dos.e_lfanew > 0x100000 ||
        !readBytes(base + static_cast<std::uintptr_t>(dos.e_lfanew), &pe, sizeof(pe)) ||
        pe.Signature != IMAGE_NT_SIGNATURE || pe.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
        pe.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC ||
        pe.OptionalHeader.SizeOfImage != kArkImageSize ||
        reinterpret_cast<std::uintptr_t>(GetProcAddress(module, "arkNDDIOGetInput")) != base + kExportRva) return false;
    return verifiedArkFile(module);
}

__declspec(noinline) std::int32_t __fastcall observedGetInput(void* output) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    const std::int32_t result = originalGetInput(output);
    if (caller == acceptedCaller) {
        // Copy the completed caller-local result from the existing call. The
        // observer does not invoke another getter; optional wide conversion is
        // a distinct step after raw publication. Never change the return code.
        // Diagnostics must not throw through the game's ABI boundary.
        try {
            std::array<std::uint8_t, 52> completed {};
            if (result == 0 && readBytes(reinterpret_cast<std::uintptr_t>(output),
                    completed.data(), completed.size())) {
                std::array<std::int32_t, 2> dimensions {}, repeated {};
                if (!readBytes(arkDimensions, dimensions.data(), sizeof(dimensions)) ||
                    !readBytes(arkDimensions, repeated.data(), sizeof(repeated)) ||
                    dimensions != repeated) dimensions = {};
                state().publish(completed, dimensions[0], dimensions[1]);
                // Optional, separately gated experiment. Preserve the raw ARK
                // snapshot above; the later ray observer records native changes.
                convertNativeWideInput(output, completed, dimensions[0], dimensions[1]);
            }
            else state().invalidate();
        } catch (...) {}
    }
    return result;
}

__declspec(noinline) std::int32_t __fastcall observedStartTrigger(void* output) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    // Native export fills16 port-trigger bytes and advances its own IO state.
    // Call it exactly once even when port0 is routed to scope this frame.
    const auto result = originalStartTrigger(output);
    if (result == 0 && caller == acceptedStartCaller &&
            startRoutingReady.load(std::memory_order_acquire) && scopeConsumesStart()) {
        std::uint8_t value = 0;
        if (readBytes(reinterpret_cast<std::uintptr_t>(output), &value, sizeof(value)) && value == 1) {
            const std::uint8_t neutral = 0;
            SIZE_T written = 0;
            if (!WriteProcessMemory(GetCurrentProcess(), output, &neutral, sizeof(neutral), &written) ||
                    written != sizeof(neutral)) {
                startRoutingReady.store(false, std::memory_order_release);
                resetScopeControl();
                try { log_warning("bone-eater", "Scope Start routing stopped: port0 result could not be replaced"); }
                catch (...) {}
            }
        }
    }
    return result;
}

bool installStartRouting(std::uintptr_t gameBase, HMODULE ark) noexcept {
    if (!scopeControlRequested()) return false;
    const auto arkBase = reinterpret_cast<std::uintptr_t>(ark);
    constexpr std::array<unsigned char, 6> call {{0xFF,0x15,0x2E,0xFB,0x34,0x01}};
    constexpr std::array<unsigned char, 18> entry {{
        0x48,0x89,0x5C,0x24,0x08,0x48,0x89,0x74,0x24,0x10,0x57,0x48,0x83,0xEC,0x20,0x48,0x8B,0xF9}};
    constexpr std::array<unsigned char, 14> consumer {{
        0x85,0xC0,0x75,0x0A,0x44,0x38,0x6D,0xCF,0x74,0x04,0x83,0x4F,0x08,0x10}};
    if (reinterpret_cast<std::uintptr_t>(GetProcAddress(ark, "arkGetIOStartTrigger")) != arkBase + 0x44360 ||
        !matches(arkBase + 0x44360, entry) || !matches(gameBase + 0x88DEC, call) ||
        !matches(gameBase + 0x88DF2, consumer)) return false;
    acceptedStartCaller = gameBase + 0x88DF2;
    originalStartTrigger = reinterpret_cast<NativeGetInput>(arkBase + 0x44360);
    if (!detour::trampoline_try(originalStartTrigger, &observedStartTrigger, &originalStartTrigger)) return false;
    startRoutingReady.store(true, std::memory_order_release);
    return true;
}

__declspec(noinline) void* __fastcall observedScreenToRay(
        void* output, void* camera, float logicalX, float logicalY) {
    const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
    // By-value arguments retain the exact XMM2/XMM3 point across the original
    // call. The native helper may recalculate the camera; do not call it again.
    void* const result = originalScreenToRay(output, camera, logicalX, logicalY);
    if (caller == acceptedRayCaller) {
        try {
            const auto cameraAddress = reinterpret_cast<std::uintptr_t>(camera);
            std::uintptr_t vtable = 0;
            const bool verified = readBytes(cameraAddress, &vtable, sizeof(vtable)) &&
                vtable == expectedCameraVtable && result == output;
            const ScopeOffObservation scope = readScopeOff();
            observeNativePrecisionRay(cameraAddress, logicalX, logicalY, verified, scope.known, scope.off);
            rayState().publish(logicalX, logicalY, cameraAddress, verified, scope.known, scope.off);
            observeNativeInputOwnershipRay(cameraAddress, logicalX, logicalY, verified, scope.known, scope.off);
        } catch (...) {}
    }
    return result;
}

} // namespace

void installNativeAimObserver(void* gamendd, void* arkndd) noexcept {
    try {
        const auto featureEnabled = [](const wchar_t* name) {
            wchar_t value[4] {};
            return GetEnvironmentVariableW(name, value, 4) == 1 && value[0] == L'1';
        };
        wchar_t mapping[16] {};
        const auto mappingLength = GetEnvironmentVariableW(L"BONE_EATER_NATIVE_INPUT", mapping, 16);
        const bool wideInput = mappingLength && mappingLength < 16 &&
            (!std::wcscmp(mapping, L"wide") || !std::wcscmp(mapping, L"desktop"));
        if (!featureEnabled(L"BONE_EATER_SCOPE_OVERLAY") &&
                !featureEnabled(L"BONE_EATER_DESKTOP_RETICLE") &&
                !featureEnabled(L"BONE_EATER_AIM_FRAMING") && !wideInput) return;
        if (installAttempted) return;
        installAttempted = true;
        const char* gameStatus = render::verifyNativeGameModule(gamendd);
        if (std::strcmp(gameStatus, "verified") || !verifiedArk(static_cast<HMODULE>(arkndd))) {
            log_warning("bone-eater", "Native aim observer disabled: game={}, ARK identity must match pinned build", gameStatus);
            return;
        }
        const auto gameBase = reinterpret_cast<std::uintptr_t>(gamendd);
        const auto arkBase = reinterpret_cast<std::uintptr_t>(arkndd);
        constexpr std::array<unsigned char, 6> caller {{0xFF,0x15,0xCA,0x00,0x35,0x01}};
        constexpr std::array<unsigned char, 25> prologue {{
            0x48,0x89,0x5C,0x24,0x10,0x55,0x48,0x8B,0xEC,0x48,0x83,0xEC,0x60,
            0x48,0x8B,0x05,0xCC,0x43,0x11,0x00,0x48,0x33,0xC4,0x48,0x89
        }};
        constexpr std::array<unsigned char, 5> rayCall {{0xE8,0x99,0x99,0x0C,0x00}};
        constexpr std::array<unsigned char, 17> rayPrologue {{
            0x48,0x8B,0xC4,0x55,0x53,0x57,0x48,0x8D,0x68,0xD8,
            0x48,0x81,0xEC,0x10,0x01,0x00,0x00
        }};
        if (!matches(gameBase + kCallerRva, caller) || !matches(arkBase + kExportRva, prologue) ||
            !matches(gameBase + 0xCD1C2, rayCall) || !matches(gameBase + 0x196B60, rayPrologue)) {
            log_warning("bone-eater", "Native aim observer disabled: caller/export instruction bytes differ");
            return;
        }
        // Pin both identities for hook lifetime; unloaded/reused bases must
        // never make the accepted caller or original trampoline ambiguous.
        HMODULE pinnedGame = nullptr, pinnedArk = nullptr;
        constexpr DWORD pinFlags = GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN;
        if (!GetModuleHandleExW(pinFlags, reinterpret_cast<LPCWSTR>(gamendd), &pinnedGame) ||
            !GetModuleHandleExW(pinFlags, reinterpret_cast<LPCWSTR>(arkndd), &pinnedArk) ||
            pinnedGame != gamendd || pinnedArk != arkndd) {
            log_warning("bone-eater", "Native aim observer disabled: module lifetime could not be pinned");
            return;
        }
        (void) state(); // Allocate all shared state before enabling the hook.
        (void) rayState();
        installNativeWideInput(gamendd);
        acceptedCaller = gameBase + kReturnRva;
        acceptedRayCaller = gameBase + 0xCD1C7;
        expectedCameraVtable = gameBase + 0x6FDF48;
        arkDimensions = arkBase + 0x3E2F08;
        scopeOffGlobal = gameBase + 0x13DCF48;
        expectedScopeOffVtable = gameBase + 0x10CA5E0;
        originalGetInput = reinterpret_cast<NativeGetInput>(arkBase + kExportRva);
        if (!detour::trampoline_try(originalGetInput, &observedGetInput, &originalGetInput)) {
            log_warning("bone-eater", "Native aim observer hook could not be installed");
            return;
        }
        log_info("bone-eater", "Native aim observer installed: ARK input result, game caller RVA 0x88C86");
        originalScreenToRay = reinterpret_cast<NativeScreenToRay>(gameBase + 0x196B60);
        if (!detour::trampoline_try(originalScreenToRay, &observedScreenToRay, &originalScreenToRay)) {
            log_warning("bone-eater", "Native final ray observer hook could not be installed; ARK observation remains available");
            return;
        }
        log_info("bone-eater", "Native final ray observer installed: ScopeCamera caller RVA 0xCD1C7");
        nativePrecisionAimInstalled(gamendd);
        if (scopeControlRequested()) {
            if (installStartRouting(gameBase, static_cast<HMODULE>(arkndd)))
                log_info("bone-eater", "Scope Start routing installed: exact game input caller, certified playable context only");
            else log_warning("bone-eater", "Scope toggle/hold unavailable: native Start consumer hook could not be installed");
        }
    } catch (...) {
        // An optional observer must not prevent baseline startup.
    }
}

NativeAimSnapshot readNativeAimSnapshot() { return state().read(); }
NativeRaySnapshot readNativeRaySnapshot() { return rayState().read(); }
bool scopeStartRoutingReady() noexcept { return startRoutingReady.load(std::memory_order_acquire); }

} // namespace bone_eater::input
