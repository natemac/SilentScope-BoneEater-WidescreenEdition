#include "camera/checked_target_write.h"

#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

using namespace bone_eater::camera;

namespace {
void require(bool condition, int line) {
    if (!condition) throw std::runtime_error("failed at line " + std::to_string(line));
}
#define CHECK(value) require((value), __LINE__)

struct Region {
    unsigned char* memory = nullptr;
    std::size_t page = 0;
    Region() {
        SYSTEM_INFO info {};
        GetSystemInfo(&info);
        page = info.dwPageSize;
        memory = static_cast<unsigned char*>(VirtualAlloc(nullptr, page * 2, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
        CHECK(memory != nullptr);
        std::memset(memory, 0x5a, page * 2);
    }
    ~Region() { if (memory) VirtualFree(memory, 0, MEM_RELEASE); }
    std::uintptr_t wrapper() const { return reinterpret_cast<std::uintptr_t>(memory) + 64; }
    void protect(DWORD protection, bool secondPage = false) {
        DWORD old = 0;
        CHECK(VirtualProtect(memory + (secondPage ? page : 0), secondPage ? page : page * 2, protection, &old));
    }
    void release() { CHECK(VirtualFree(memory, 0, MEM_RELEASE)); memory = nullptr; }
};

const NativeTargetVector baseline {{1, 2, 3, 4}}, pitched {{-4, 20, 30, 1}};
bool same(std::uintptr_t wrapper, const NativeTargetVector& value) {
    return std::memcmp(reinterpret_cast<const void*>(wrapper + 0x20), value.data(), sizeof(value)) == 0;
}
NativeTargetPermission permission(std::uintptr_t wrapper) {
    return acquireNativeTargetPermission(observeNativeTargetAccess(wrapper, GetTickCount64()), wrapper);
}

void exactPrivateSpan() {
    Region region;
    const auto token = permission(region.wrapper());
    CHECK(token.valid && writeNativeTarget(token, pitched));
    CHECK(same(region.wrapper(), pitched));
    CHECK(region.memory[95] == 0x5a && region.memory[112] == 0x5a);
}

void nullOverflowAndMismatchedToken() {
    CHECK(!observeNativeTargetAccess(0, GetTickCount64()).valid);
    CHECK(!observeNativeTargetAccess(std::numeric_limits<std::uintptr_t>::max() - 8, GetTickCount64()).valid);
    Region region;
    auto token = permission(region.wrapper());
    ++token.address;
    CHECK(!writeNativeTarget(token, pitched));
    token = permission(region.wrapper());
    token.valid = false;
    CHECK(!writeNativeTarget(token, pitched));
}

void freshAcquisitionAndAgeIndependentRestoration() {
    Region region;
    auto access = observeNativeTargetAccess(region.wrapper(), GetTickCount64());
    const auto token = acquireNativeTargetPermission(access, region.wrapper());
    CHECK(token.valid && writeNativeTarget(token, pitched));
    access.observed -= 101;
    CHECK(!acquireNativeTargetPermission(access, region.wrapper()).valid);
    access = {}; // A replaced current snapshot cannot revoke this commit's restore token.
    CHECK(writeNativeTarget(token, baseline) && same(region.wrapper(), baseline));
}

void futureObservationCannotAcquire() {
    Region region;
    auto access = observeNativeTargetAccess(region.wrapper(), GetTickCount64());
    access.observed += 60000;
    CHECK(!acquireNativeTargetPermission(access, region.wrapper()).valid);
}

void shortLeaseDoesNotRenewTimestamp() {
    Region region;
    const auto tick = GetTickCount64();
    auto access = observeNativeTargetAccess(region.wrapper(), tick);
    access.observed = tick - 50;
    bool queried = true;
    const auto reused = observeNativeTargetAccess(region.wrapper(), tick, &access, &queried);
    CHECK(reused.valid && !queried && reused.observed == access.observed);
    const auto refreshed = observeNativeTargetAccess(region.wrapper(), tick + 1, &reused, &queried);
    CHECK(refreshed.valid && queried && refreshed.observed == tick + 1);
}

void changedWrapperCannotInheritPermission() {
    Region region;
    const auto access = observeNativeTargetAccess(region.wrapper(), GetTickCount64());
    CHECK(!acquireNativeTargetPermission(access, region.wrapper() + 16).valid);
    bool queried = false;
    const auto other = observeNativeTargetAccess(region.wrapper() + 16, GetTickCount64(), &access, &queried);
    CHECK(other.valid && queried && other.address == region.wrapper() + 16 + 0x20);
}

void disallowedPageKinds() {
    for (const auto protection : {PAGE_READONLY, PAGE_EXECUTE_READWRITE, PAGE_NOACCESS, PAGE_READWRITE | PAGE_GUARD}) {
        Region region;
        region.protect(protection);
        CHECK(!observeNativeTargetAccess(region.wrapper(), GetTickCount64()).valid);
        MEMORY_BASIC_INFORMATION info {};
        CHECK(VirtualQuery(region.memory, &info, sizeof(info)) == sizeof(info));
        CHECK(info.Protect == static_cast<DWORD>(protection)); // In particular, the guard was not consumed.
    }
}

void mappedWritablePagesAreRejected() {
    HANDLE mapping = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr);
    CHECK(mapping != nullptr);
    void* view = MapViewOfFile(mapping, FILE_MAP_WRITE, 0, 0, 4096);
    CHECK(view != nullptr);
    const bool rejected = !observeNativeTargetAccess(reinterpret_cast<std::uintptr_t>(view) + 64, GetTickCount64()).valid;
    CHECK(UnmapViewOfFile(view));
    CHECK(CloseHandle(mapping));
    CHECK(rejected);
}

void freedMemoryFailsSafely() {
    Region region;
    const auto wrapper = region.wrapper();
    const auto token = permission(wrapper);
    region.release();
    CHECK(!writeNativeTarget(token, pitched));
    CHECK(!observeNativeTargetAccess(wrapper, GetTickCount64()).valid);
}

void cachedProtectionLossFailsWithoutChangingProtection() {
    Region region;
    auto access = observeNativeTargetAccess(region.wrapper(), GetTickCount64());
    const auto token = acquireNativeTargetPermission(access, region.wrapper());
    CHECK(writeNativeTarget(token, baseline));
    region.protect(PAGE_READONLY);
    bool queried = true;
    const auto reused = observeNativeTargetAccess(region.wrapper(), access.observed, &access, &queried);
    CHECK(reused.valid && !queried && reused.observed == access.observed);
    CHECK(!writeNativeTarget(token, pitched) && same(region.wrapper(), baseline));
    CHECK(!observeNativeTargetAccess(region.wrapper(), access.observed + 51, &access, &queried).valid && queried);
    CHECK(!observeNativeTargetAccess(region.wrapper() + 16, access.observed, &access, &queried).valid && queried);
}

void completeSpanMustFitTheObservedRegion() {
    Region region;
    const auto wrapper = reinterpret_cast<std::uintptr_t>(region.memory + region.page - 8) - 0x20;
    region.protect(PAGE_NOACCESS, true);
    CHECK(!observeNativeTargetAccess(wrapper, GetTickCount64()).valid);
}

void partialStoreStillAllowsBaselineRestorationAttempt() {
    Region region;
    const auto wrapper = reinterpret_cast<std::uintptr_t>(region.memory + region.page - 8) - 0x20;
    const auto token = permission(wrapper);
    CHECK(token.valid && writeNativeTarget(token, baseline));
    region.protect(PAGE_READONLY, true);
    CHECK(!writeNativeTarget(token, pitched));
    CHECK(!same(wrapper, baseline)); // First page was written before the second page faulted.
    CHECK(!writeNativeTarget(token, baseline)); // Conservative failure remains: the readonly suffix was not writable.
    CHECK(same(wrapper, baseline)); // Nevertheless, the partially changed prefix was restored.
}

void objectRepresentationIsPreserved() {
    Region region;
    const std::array<std::uint32_t, 4> bits {{0x80000000u, 0x00000000u, 0x7fc01234u, 0x3f800000u}};
    NativeTargetVector value {};
    std::memcpy(value.data(), bits.data(), sizeof(value));
    CHECK(writeNativeTarget(permission(region.wrapper()), value));
    CHECK(same(region.wrapper(), value));
}
} // namespace

int main() {
    try {
        exactPrivateSpan();
        nullOverflowAndMismatchedToken();
        freshAcquisitionAndAgeIndependentRestoration();
        futureObservationCannotAcquire();
        shortLeaseDoesNotRenewTimestamp();
        changedWrapperCannotInheritPermission();
        disallowedPageKinds();
        mappedWritablePagesAreRejected();
        freedMemoryFailsSafely();
        cachedProtectionLossFailsWithoutChangingProtection();
        completeSpanMustFitTheObservedRegion();
        partialStoreStillAllowsBaselineRestorationAttempt();
        objectRepresentationIsPreserved();
        std::cout << "13 checked target-write scenarios passed\n";
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
