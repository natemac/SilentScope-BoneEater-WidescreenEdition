#include "render/native_movie_fit.h"
#include "render/native_viewport.h"
#include "render/native_display.h"
#include "render/movie_fit_math.h"
#include "render/checked_data_write.h"
#include "util/detour.h"
#include "util/logging.h"
#include <atomic>
#include <cstring>
#include <cwchar>
#include <limits>

namespace bone_eater::render {
namespace {

// CMovieTextureRenderer::SetMediaType, complete-object vftable +120.
using SetMediaType = HRESULT(__fastcall*)(void*, const void*);
SetMediaType originalSetMediaType = nullptr;
using LoadMovie = bool(__fastcall*)(void*, const void*, unsigned);
LoadMovie originalLoadMovie = nullptr;
std::uintptr_t movieModule = 0;
std::atomic<bool> movieFailed {false};
std::atomic<unsigned> movieReports {0};
constexpr std::uintptr_t kMovieVtable = 0x10CAF48;
constexpr std::uintptr_t kMovieSpriteVtable = 0x10CD1E8;
constexpr std::uintptr_t kMovieFrameVtable = 0x703E78;
constexpr std::uintptr_t kMovieHelperVtable = 0x10C07A8;
constexpr std::uintptr_t kMovieGeometryOffset = 0x580;

template<class T>
bool movieRead(std::uintptr_t address, std::size_t offset, T& value) noexcept {
    if (!address || address > std::numeric_limits<std::uintptr_t>::max() - offset ||
            address + offset > std::numeric_limits<std::uintptr_t>::max() - sizeof(T)) return false;
    SIZE_T copied = 0;
    return ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<const void*>(address + offset),
        &value, sizeof(value), &copied) && copied == sizeof(value);
}

struct MovieIdentity {
    std::uintptr_t owner = 0, sprite = 0, texture = 0, textureResource = 0;
    std::uintptr_t helper = 0, framebuffer = 0;
    std::uint64_t textureId = 0;
    std::uint32_t sourceWidth = 0, sourceHeight = 0;
    std::uint16_t canvasWidth = 0, canvasHeight = 0;
    std::array<float, 4> geometry {}, uv {};
};

bool movieWideContext(MovieIdentity& identity) noexcept {
    std::uintptr_t vtable = 0;
    return originalNativeMainAspect() > 0.0f &&
        movieRead(movieModule, 0x13D8E98, identity.helper) &&
        movieRead(identity.helper, 0, vtable) && vtable == movieModule + kMovieHelperVtable &&
        movieRead(movieModule, 0x12E24E8, identity.framebuffer) &&
        movieRead(identity.framebuffer, 0, vtable) && vtable == movieModule + kMovieFrameVtable &&
        movieRead(identity.framebuffer, 0x28, identity.canvasWidth) && identity.canvasWidth == 1920 &&
        movieRead(identity.framebuffer, 0x2A, identity.canvasHeight) && identity.canvasHeight == 1080;
}

bool movieOwner(std::uintptr_t owner, std::uintptr_t& sprite, std::uintptr_t& texture) noexcept {
    std::uintptr_t vtable = 0;
    std::uint32_t display = ~0U;
    return movieRead(owner, 0, vtable) && vtable == movieModule + kMovieVtable &&
        movieRead(owner, 0x204, display) && display == 0 &&
        movieRead(owner, 0x1E8, texture) && movieRead(owner, 0x1F0, sprite);
}

bool movieNewOwner(std::uintptr_t owner) noexcept {
    std::uintptr_t sprite = 0, texture = 0;
    MovieIdentity context;
    return !movieFailed.load(std::memory_order_relaxed) &&
        movieOwner(owner, sprite, texture) && !sprite && !texture && movieWideContext(context);
}

bool movieIdentity(std::uintptr_t owner, MovieIdentity& identity, bool fittedSource = false) noexcept {
    identity = {};
    identity.owner = owner;
    std::uintptr_t vtable = 0;
    std::array<float, 2> scale {};
    float rotation = 0;
    if (!movieOwner(owner, identity.sprite, identity.texture) || !identity.sprite || !identity.texture ||
            !movieWideContext(identity) ||
            !movieRead(identity.sprite, 0, vtable) || vtable != movieModule + kMovieSpriteVtable ||
            !movieRead(identity.texture, 0, identity.textureResource) || !identity.textureResource ||
            !movieRead(owner, 0x208, identity.textureId) ||
            !movieRead(owner, 0x1F8, identity.sourceWidth) || !identity.sourceWidth || identity.sourceWidth > 16384 ||
            !movieRead(owner, 0x1FC, identity.sourceHeight) || !identity.sourceHeight || identity.sourceHeight > 16384 ||
            !movieRead(identity.sprite, kMovieGeometryOffset, identity.geometry) ||
            !movieRead(identity.sprite, 0x598, identity.uv) ||
            !movieRead(identity.sprite, 0x590, scale) || scale != std::array<float, 2> {1, 1} ||
            !movieRead(identity.sprite, 0x5AC, rotation) || rotation != 0) return false;
    // Exact native display-0 initialization with the two-display branch active.
    // This rejects already adapted or custom geometry, so reentry never compounds.
    const auto expected = fittedSource ? std::array<float, 4> {960, 540, 675, 1080}
        : std::array<float, 4> {384, 540, 801, 1281};
    if (identity.geometry != expected) return false;
    // Preserve the native padded texture crop and vertical inversion exactly.
    return identity.uv[0] == 0 && std::isfinite(identity.uv[1]) && identity.uv[1] > 0 && identity.uv[1] <= 1 &&
        std::isfinite(identity.uv[2]) && identity.uv[2] > 0 && identity.uv[2] <= 1 &&
        identity.uv[3] == -identity.uv[1];
}

bool sameMovieIdentity(const MovieIdentity& a, const MovieIdentity& b) noexcept {
    return a.owner == b.owner && a.sprite == b.sprite && a.texture == b.texture &&
        a.textureResource == b.textureResource && a.textureId == b.textureId &&
        a.helper == b.helper && a.framebuffer == b.framebuffer &&
        a.sourceWidth == b.sourceWidth && a.sourceHeight == b.sourceHeight &&
        a.canvasWidth == b.canvasWidth && a.canvasHeight == b.canvasHeight &&
        a.geometry == b.geometry && a.uv == b.uv;
}

// This one-shot permission covers only the newly initialized sprite's four
// geometry floats. It is not a lifetime lock; native SetMediaType synchronously
// owns the same object, and identity is repeated after the page query. Native
// graph initialization publishes player state 1 only after RenderFile returns
// (18D0CC -> 18D116); start then registers the sprite at 18D434 and publishes
// state 2. Our fresh-resource callback completes before that publication.
bool movieGeometryWritable(std::uintptr_t sprite) noexcept {
    if (!sprite || sprite > std::numeric_limits<std::uintptr_t>::max() - kMovieGeometryOffset - 16) return false;
    const auto address = sprite + kMovieGeometryOffset;
    checked_data_detail::Region region;
    return checked_data_detail::queryPrivateWritable(address, region) &&
        16 <= region.size - (address - region.start);
}

bool movieWriteGeometry(std::uintptr_t sprite, const std::array<float, 4>& geometry) noexcept {
    if (!sprite || sprite > std::numeric_limits<std::uintptr_t>::max() - kMovieGeometryOffset - 16) return false;
    __try {
        auto* destination = reinterpret_cast<volatile unsigned char*>(sprite + kMovieGeometryOffset);
        const auto* source = reinterpret_cast<const unsigned char*>(geometry.data());
        for (unsigned index = 0; index < 16; ++index) destination[index] = source[index];
        for (unsigned index = 0; index < 16; ++index) if (destination[index] != source[index]) return false;
        return true;
    } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION || GetExceptionCode() == EXCEPTION_IN_PAGE_ERROR)
            ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH) { return false; }
}

void movieReport(const char* status, const MovieIdentity& identity, const std::array<float, 4>& fitted) noexcept {
    if (movieReports.fetch_add(1, std::memory_order_relaxed) >= 32) return;
    try {
        log_info("bone-eater", "Native movie fit pid={} thread={} status={} owner=0x{:X} sprite=0x{:X} source={}x{} canvas={}x{} center=({:.3f},{:.3f}) size=({:.3f},{:.3f})",
            GetCurrentProcessId(), GetCurrentThreadId(), status, identity.owner, identity.sprite,
            identity.sourceWidth, identity.sourceHeight, identity.canvasWidth, identity.canvasHeight,
            fitted[0], fitted[1], fitted[2], fitted[3]);
    } catch (...) {}
}

bool pairedOpeningPath(std::uintptr_t path) noexcept {
    // Pinned native std::string layout. Only the reviewed two-screen opening
    // pair uses the narrower front-film safe area; standalone movies keep fit.
    std::size_t size = 0, capacity = 0;
    std::uintptr_t chars = path;
    std::array<char, 512> name {};
    if (!movieRead(path, 0x10, size) || !movieRead(path, 0x18, capacity) ||
            !size || size >= name.size() || size > capacity || capacity > 4096 ||
            (capacity >= 16 && !movieRead(path, 0, chars))) return false;
    SIZE_T copied = 0;
    if (!ReadProcessMemory(GetCurrentProcess(), reinterpret_cast<void*>(chars),
            name.data(), size + 1, &copied) || copied != size + 1 || name[size]) return false;
    const char* file = name.data();
    for (std::size_t i = 0; i < size; ++i) {
        if (!name[i]) return false;
        if (name[i] >= 'A' && name[i] <= 'Z') name[i] += 'a' - 'A';
        if (name[i] == '/' || name[i] == '\\') file = name.data() + i + 1;
    }
    return !std::strcmp(file, "ndd_worldview_main.wmv") ||
        !std::strcmp(file, "ndd_worldview_main_nobgm.wmv");
}

void fitPairedOpening(std::uintptr_t player) noexcept {
    std::uintptr_t vt = 0, renderer = 0, repeatedRenderer = 0;
    unsigned state = 0;
    MovieIdentity identity, repeated;
    if (movieFailed.load(std::memory_order_relaxed) ||
            !movieRead(player, 0, vt) || vt != movieModule + 0x10CAE78 ||
            !movieRead(player, 8, state) || state != 1 ||
            !pairedOpeningPath(player + 0x10) ||
            !movieRead(player, 0x40, renderer) || !movieIdentity(renderer, identity, true) ||
            identity.sourceWidth != 800 || identity.sourceHeight != 1280 ||
            !movieGeometryWritable(identity.sprite) || !movieRead(player, 8, state) || state != 1 ||
            !movieRead(player, 0x40, repeatedRenderer) || renderer != repeatedRenderer ||
            !movieIdentity(renderer, repeated, true) || !sameMovieIdentity(identity, repeated)) return;
    auto fitted = identity.geometry;
    fitted[2] = 768.f * identity.canvasHeight / 1366.f;
    fitted[3] = fitted[2] * 1280.f / 800.f;
    if (movieWriteGeometry(identity.sprite, fitted)) {
        movieReport("paired_opening_fitted", identity, fitted);
        return;
    }
    const bool restored = movieWriteGeometry(identity.sprite, identity.geometry);
    movieFailed.store(true, std::memory_order_relaxed);
    movieReport(restored ? "pair_write_failed_restored_disabled" : "pair_write_failed_restore_failed_disabled", identity, fitted);
}

__declspec(noinline) bool __fastcall loadMovie(void* player, const void* path, unsigned display) {
    const auto address = reinterpret_cast<std::uintptr_t>(player);
    std::uintptr_t vt = 0;
    unsigned state = ~0U;
    const bool candidate = display == 0 && movieRead(address, 0, vt) &&
        vt == movieModule + 0x10CAE78 && movieRead(address, 8, state) && state == 0 &&
        pairedOpeningPath(reinterpret_cast<std::uintptr_t>(path));
    // RenderFile may initialize the decoder on a different thread. The native
    // load return joins graph creation and leaves state 1, before Start registers
    // the sprite. Fit that exact player-owned sprite here, without thread-local
    // assumptions or a persistent pointer registry.
    const bool result = originalLoadMovie(player, path, display);
    if (candidate && result) fitPairedOpening(address);
    return result;
}

void fitInitializedMovie(std::uintptr_t owner) noexcept {
    MovieIdentity identity, repeated;
    std::array<float, 4> fitted {};
    if (!movieIdentity(owner, identity) ||
            !fittedMovieGeometry(identity.sourceWidth, identity.sourceHeight,
                identity.canvasWidth, identity.canvasHeight, fitted) ||
            !movieGeometryWritable(identity.sprite) ||
            !movieIdentity(owner, repeated) || !sameMovieIdentity(identity, repeated)) {
        movieReport("guard_rejected", identity, fitted);
        return;
    }
    if (movieWriteGeometry(identity.sprite, fitted)) {
        movieReport("fitted", identity, fitted);
        return;
    }
    // Even a failed store can have written a prefix. The synchronous, saved
    // identity owns this exact span; always attempt its original-byte rollback.
    const bool restored = movieWriteGeometry(identity.sprite, identity.geometry);
    movieFailed.store(true, std::memory_order_relaxed);
    movieReport(restored ? "write_failed_restored_disabled" : "write_failed_restore_failed_disabled", identity, fitted);
}

__declspec(noinline) HRESULT __fastcall movieSetMediaType(void* owner, const void* mediaType) {
    const auto address = reinterpret_cast<std::uintptr_t>(owner);
    const bool candidate = movieNewOwner(address);
    // Exactly once, unchanged arguments and HRESULT. Native exceptions propagate.
    const HRESULT result = originalSetMediaType(owner, mediaType);
    if (candidate && SUCCEEDED(result)) fitInitializedMovie(address);
    return result;
}

template<std::size_t N>
bool movieInstructions(std::uintptr_t rva, const std::array<unsigned char, N>& expected) noexcept {
    std::array<unsigned char, N> actual {};
    return movieRead(movieModule, rva, actual) && actual == expected;
}

} // namespace

void installNativeMovieFit(void* module) noexcept {
    try {
        wchar_t setting[8] {}, wide[16] {};
        if (GetEnvironmentVariableW(L"BONE_EATER_NATIVE_MOVIE_FIT", setting, 8) != 1 || setting[0] != L'1' ||
                GetEnvironmentVariableW(L"BONE_EATER_NATIVE_DISPLAY", wide, 16) != 4 || wcscmp(wide, L"wide") ||
                originalSetMediaType) return;
        const char* status = verifyNativeGameModule(module);
        if (std::strcmp(status, "verified")) {
            log_warning("bone-eater", "Native movie fit disabled: {}", status);
            return;
        }
        movieModule = reinterpret_cast<std::uintptr_t>(module);
        constexpr std::array<unsigned char, 32> entry {{
            0x48,0x8b,0xc4,0x55,0x48,0x83,0xec,0x70,0x48,0x83,0xb9,0xe8,0x01,0x00,0x00,0x00,
            0x48,0x8b,0xe9,0x0f,0x85,0x0d,0x04,0x00,0x00,0x48,0x83,0xb9,0xf0,0x01,0x00,0x00
        }};
        constexpr std::array<unsigned char, 32> displayBranch {{
            0x83,0xbd,0x04,0x02,0x00,0x00,0x00,0x75,0x54,0x48,0x83,0x3d,0x12,0xc4,0x24,0x01,
            0x00,0x74,0x0a,0xf3,0x0f,0x10,0x05,0x50,0x45,0xf4,0x00,0xeb,0x08,0xf3,0x0f,0x10
        }};
        constexpr std::array<unsigned char, 13> loadEntry {{
            0x4c,0x8b,0xdc,0x56,0x57,0x41,0x54,0x41,0x55,0x48,0x83,0xec,0x78}};
        if (!movieInstructions(0x18C720, entry) || !movieInstructions(0x18CA75, displayBranch) ||
                !movieInstructions(0x18CDF0, loadEntry)) {
            log_warning("bone-eater", "Native movie fit disabled: instruction guard differs");
            return;
        }
        HMODULE pinned = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                reinterpret_cast<LPCWSTR>(module), &pinned) || pinned != module) return;
        originalSetMediaType = reinterpret_cast<SetMediaType>(movieModule + 0x18C720);
        if (!detour::trampoline_try(originalSetMediaType, &movieSetMediaType, &originalSetMediaType)) {
            log_warning("bone-eater", "Native movie fit hook installation failed");
            return;
        }
        originalLoadMovie = reinterpret_cast<LoadMovie>(movieModule + 0x18CDF0);
        if (!detour::trampoline_try(originalLoadMovie, &loadMovie, &originalLoadMovie)) {
            originalLoadMovie = nullptr;
            log_warning("bone-eater", "Paired opening movie hook unavailable; preserving ordinary movie fit");
        }
        log_info("bone-eater", "Native movie fit installed; new display-0 movies only; preserve decoded source aspect");
    } catch (...) {}
}

} // namespace bone_eater::render
