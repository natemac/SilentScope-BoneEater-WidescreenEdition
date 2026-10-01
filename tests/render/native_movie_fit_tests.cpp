// Exact production callback with inert detour/log stubs. No game, device, hook,
// process creation or media playback; private allocations exercise Win32 guards.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <array>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace fixture {
std::uintptr_t corruptAfterQuery = 0, protectAfterQuery = 0;
unsigned queries = 0;
SIZE_T query(LPCVOID address, PMEMORY_BASIC_INFORMATION result, SIZE_T length) noexcept {
    ++queries;
    const auto count = VirtualQuery(address, result, length);
    if (corruptAfterQuery) {
        *reinterpret_cast<std::uintptr_t*>(corruptAfterQuery) = 1;
        corruptAfterQuery = 0;
    }
    if (protectAfterQuery) {
        DWORD old = 0;
        VirtualProtect(reinterpret_cast<void*>(protectAfterQuery), 4096, PAGE_READONLY, &old);
        protectAfterQuery = 0;
    }
    return count;
}
float originalAspect = 800.f / 1366.f;
unsigned nativeCalls = 0;
HRESULT nativeResult = S_OK;
const void* receivedMedia = nullptr;
bool initialize = true;
}
#define VirtualQuery fixture::query
#include "../../src/render/native_movie_fit.cpp"
#undef VirtualQuery

namespace bone_eater::render {
const char* verifyNativeGameModule(void*) noexcept { return "fixture_only"; }
float originalNativeMainAspect() noexcept { return fixture::originalAspect; }
}
using namespace bone_eater::render;
#define CHECK(value) do { if (!(value)) throw std::runtime_error(#value); } while (false)

template<class T> void put(std::uintptr_t object, std::size_t offset, const T& value) {
    std::memcpy(reinterpret_cast<void*>(object + offset), &value, sizeof(value));
}
struct Scene {
    unsigned char* data = nullptr;
    std::uintptr_t owner = 0, sprite = 0, texture = 0, helper = 0, framebuffer = 0;
    explicit Scene(bool crossPage = false) {
        data = static_cast<unsigned char*>(VirtualAlloc(nullptr, 0x1400000, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));
        CHECK(data);
        movieModule = reinterpret_cast<std::uintptr_t>(data);
        owner = movieModule + 0x1000;
        sprite = movieModule + (crossPage ? 0x4000 - 0x580 - 8 : 0x3000);
        texture = movieModule + 0x6000; helper = movieModule + 0x7000; framebuffer = movieModule + 0x8000;
        put(owner, 0, movieModule + kMovieVtable);
        put(owner, 0x204, std::uint32_t {0});
        put(owner, 0x1F8, std::uint32_t {800}); put(owner, 0x1FC, std::uint32_t {1280});
        put(owner, 0x208, std::uint64_t {7});
        put(movieModule, 0x13D8E98, helper); put(helper, 0, movieModule + kMovieHelperVtable);
        put(movieModule, 0x12E24E8, framebuffer); put(framebuffer, 0, movieModule + kMovieFrameVtable);
        put(framebuffer, 0x28, std::uint16_t {1920}); put(framebuffer, 0x2A, std::uint16_t {1080});
        put(sprite, 0, movieModule + kMovieSpriteVtable);
        put(sprite, 0x590, std::array<float, 2> {1, 1});
        put(sprite, 0x598, std::array<float, 4> {0, 0.625f, 0.390625f, -0.625f});
        put(texture, 0, texture + 0x100);
        fixture::originalAspect = 800.f / 1366.f; fixture::queries = 0;
        fixture::nativeCalls = 0; fixture::nativeResult = S_OK; fixture::initialize = true;
        fixture::corruptAfterQuery = 0; fixture::protectAfterQuery = 0;
        movieFailed.store(false); movieReports.store(0);
    }
    ~Scene() { VirtualFree(data, 0, MEM_RELEASE); }
    void initialized() {
        put(owner, 0x1E8, texture); put(owner, 0x1F0, sprite);
        put(sprite, 0x580, std::array<float, 4> {384, 540, 801, 1281});
    }
    std::array<float, 4> geometry() const {
        std::array<float, 4> result {}; CHECK(movieRead(sprite, 0x580, result)); return result;
    }
};
Scene* scene = nullptr;
HRESULT __fastcall native(void* owner, const void* media) {
    CHECK(reinterpret_cast<std::uintptr_t>(owner) == scene->owner);
    ++fixture::nativeCalls; fixture::receivedMedia = media;
    if (fixture::initialize) scene->initialized();
    return fixture::nativeResult;
}
void invoke(Scene& value) {
    scene = &value; originalSetMediaType = &native;
    const auto* media = reinterpret_cast<const void*>(0x1234);
    CHECK(movieSetMediaType(reinterpret_cast<void*>(value.owner), media) == fixture::nativeResult);
    CHECK(fixture::receivedMedia == media);
}

bool __fastcall nativeLoad(void* player, const void* path, unsigned display) {
    CHECK(reinterpret_cast<std::uintptr_t>(player) == movieModule + 0x9000);
    CHECK(display == 0);
    put(reinterpret_cast<std::uintptr_t>(player), 0x40, scene->owner);
    std::memcpy(reinterpret_cast<void*>(reinterpret_cast<std::uintptr_t>(player) + 0x10), path, 32);
    std::thread decoder([] { invoke(*scene); });
    decoder.join();
    put(reinterpret_cast<std::uintptr_t>(player), 8, unsigned {1});
    return true;
}

int main() {
    unsigned cases = 0;
    try {
        { Scene value; scene = &value;
          const auto player = movieModule + 0x9000, path = movieModule + 0xA000, chars = movieModule + 0xB000;
          const char name[] = "Movie/NDD_WorldView_Main.wmv";
          put(player, 0, movieModule + 0x10CAE78); put(path, 0, chars);
          put(path, 0x10, std::size_t {sizeof(name)-1}); put(path, 0x18, std::size_t {63});
          std::memcpy(reinterpret_cast<void*>(chars), name, sizeof(name));
          CHECK(pairedOpeningPath(path)); originalLoadMovie = nativeLoad;
          CHECK(loadMovie(reinterpret_cast<void*>(player), reinterpret_cast<void*>(path), 0));
          CHECK(fixture::nativeCalls == 1);
          CHECK(std::abs(value.geometry()[2] - 768.f * 1080.f / 1366.f) < .001f);
          CHECK(std::abs(value.geometry()[2] / value.geometry()[3] - 800.f / 1280.f) < .00001f);
          CHECK(value.geometry()[0] == 960 && value.geometry()[1] == 540);
          const char standalone[] = "data/movie/demo_main.wmv";
          std::memcpy(reinterpret_cast<void*>(chars), standalone, sizeof(standalone));
          put(path, 0x10, std::size_t {sizeof(standalone)-1}); CHECK(!pairedOpeningPath(path));
          put(path, 0x10, std::size_t {512}); CHECK(!pairedOpeningPath(path)); ++cases; }
        { Scene value; value.initialized();
          const auto player = movieModule + 0x9000;
          put(player, 0, movieModule + 0x10CAE78); put(player, 0x40, value.owner);
          put(player, 8, unsigned {2});
          fitInitializedMovie(value.owner); fitPairedOpening(player);
          CHECK(value.geometry()[2] == 675); ++cases; }
        { std::array<float, 4> result {};
          CHECK(fittedMovieGeometry(800, 1280, 1920, 1080, result));
          CHECK((result == std::array<float, 4> {960, 540, 675, 1080}));
          CHECK(fittedMovieGeometry(1920, 1080, 1920, 1080, result));
          CHECK((result == std::array<float, 4> {960, 540, 1920, 1080})); ++cases; }
        { std::array<float, 4> result {};
          CHECK(fittedMovieGeometry(2400, 1000, 1920, 1080, result));
          CHECK((result == std::array<float, 4> {960, 540, 1920, 800}));
          CHECK(!fittedMovieGeometry(0, 1280, 1920, 1080, result));
          CHECK(!fittedMovieGeometry(800, 16385, 1920, 1080, result));
          CHECK(!fittedMovieGeometry(800, 1280, 0, 1080, result)); ++cases; }
        { Scene value; const auto uv = std::array<float, 4> {0, .625f, .390625f, -.625f};
          invoke(value); CHECK(fixture::nativeCalls == 1 && fixture::queries == 1);
          CHECK((value.geometry() == std::array<float, 4> {960, 540, 675, 1080}));
          std::array<float, 4> readUv {}; CHECK(movieRead(value.sprite, 0x598, readUv) && readUv == uv); ++cases; }
        { Scene value; put(value.owner, 0x204, std::uint32_t {1}); invoke(value);
          CHECK(fixture::nativeCalls == 1 && fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; put(value.owner, 0x204, std::uint32_t {2}); invoke(value);
          CHECK(fixture::nativeCalls == 1 && fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; put(value.owner, 0, movieModule + kMovieVtable + 0x18); invoke(value);
          CHECK(fixture::nativeCalls == 1 && fixture::queries == 0); ++cases; }
        { Scene value; put(value.framebuffer, 0x28, std::uint16_t {800}); invoke(value);
          CHECK(fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; fixture::originalAspect = 0; invoke(value);
          CHECK(fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; put(value.helper, 0, std::uintptr_t {0}); invoke(value);
          CHECK(fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; fixture::nativeResult = E_FAIL; invoke(value);
          CHECK(fixture::nativeCalls == 1 && fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; value.initialized(); fixture::initialize = false;
          put(value.sprite, 0x580, std::array<float, 4> {960, 540, 675, 1080}); invoke(value);
          CHECK(fixture::nativeCalls == 1 && fixture::queries == 0 && value.geometry()[2] == 675); ++cases; }
        { Scene value; put(value.sprite, 0, std::uintptr_t {0}); invoke(value);
          CHECK(fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; put(value.owner, 0x1FC, std::uint32_t {0}); invoke(value);
          CHECK(fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; put(value.sprite, 0x5AC, 1.f); invoke(value);
          CHECK(fixture::queries == 0 && value.geometry()[0] == 384); ++cases; }
        { Scene value; fixture::corruptAfterQuery = value.owner; invoke(value);
          CHECK(fixture::queries == 1 && value.geometry()[0] == 384 && !movieFailed.load()); ++cases; }
        { Scene value(true); value.initialized(); fixture::initialize = false;
          DWORD old = 0; CHECK(VirtualProtect(reinterpret_cast<void*>(movieModule + 0x4000), 4096, PAGE_READONLY, &old));
          CHECK(!movieGeometryWritable(value.sprite)); ++cases; }
        { Scene value(true); fixture::protectAfterQuery = movieModule + 0x4000; invoke(value);
          CHECK(movieFailed.load());
          CHECK((value.geometry() == std::array<float, 4> {384, 540, 801, 1281}));
          CHECK(fixture::nativeCalls == 1); ++cases; }
        std::cout << cases << " native movie geometry/identity/write cases passed\n";
        return 0;
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}
