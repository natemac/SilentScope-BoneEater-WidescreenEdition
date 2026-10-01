#include "render/checked_dialogue_position.h"
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
using namespace bone_eater::render;
namespace {
void require(bool value, int line) { if (!value) throw std::runtime_error("line " + std::to_string(line)); }
#define CHECK(v) require((v), __LINE__)
struct Memory {
    unsigned char* bytes = nullptr; std::size_t page = 0;
    Memory() { SYSTEM_INFO info{}; GetSystemInfo(&info); page=info.dwPageSize;
        bytes=static_cast<unsigned char*>(VirtualAlloc(nullptr,page*2,MEM_RESERVE|MEM_COMMIT,PAGE_READWRITE));
        CHECK(bytes); std::memset(bytes,0x5a,page*2); }
    ~Memory() { if(bytes) VirtualFree(bytes,0,MEM_RELEASE); }
    std::uintptr_t sprite() const { return reinterpret_cast<std::uintptr_t>(bytes)+64; }
    void protect(DWORD flags, bool second=false) { DWORD old=0;
        CHECK(VirtualProtect(bytes+(second?page:0),second?page:page*2,flags,&old)); }
};
const std::array<float,2> original {{0,213.46999f}}, translated {{656.39824f,213.46999f}};
bool same(std::uintptr_t sprite,const std::array<float,2>& xy) {
    return !std::memcmp(reinterpret_cast<const void*>(sprite+0x70),xy.data(),8);
}
void exactXyOnlyAndCapturedRestoration() {
    Memory memory; auto access=observeDialoguePosition(memory.sprite(),GetTickCount64());
    const auto permission=acquireDialoguePosition(access,memory.sprite());
    CHECK(permission.valid && writeDialoguePosition(permission,translated) && same(memory.sprite(),translated));
    for(unsigned i=0;i<0xD0;++i) if(i<0x70||i>=0x78) CHECK(memory.bytes[64+i]==0x5a);
    access.observed-=101; CHECK(!acquireDialoguePosition(access,memory.sprite()).valid);
    access={}; CHECK(writeDialoguePosition(permission,original)&&same(memory.sprite(),original));
}
void leaseDoesNotRenewOrTransfer() {
    Memory memory; auto access=observeDialoguePosition(memory.sprite(),GetTickCount64());
    access.observed=GetTickCount64()-40;
    const auto reused=observeDialoguePosition(memory.sprite(),GetTickCount64(),&access);
    CHECK(reused.valid && reused.observed==access.observed);
    memory.protect(PAGE_READONLY);
    CHECK(!observeDialoguePosition(memory.sprite()+8,GetTickCount64(),&access).valid);
    CHECK(!acquireDialoguePosition(access,memory.sprite()+8).valid);
    access.observed=GetTickCount64()-51;
    CHECK(!observeDialoguePosition(memory.sprite(),GetTickCount64(),&access).valid);
}
void invalidMemoryAndTimesReject() {
    Memory memory;
    CHECK(!observeDialoguePosition(0,GetTickCount64()).valid);
    CHECK(!observeDialoguePosition(std::numeric_limits<std::uintptr_t>::max()-8,GetTickCount64()).valid);
    CHECK(!observeDialoguePosition(memory.sprite(),GetTickCount64()+60000).valid);
    for(DWORD flags:{PAGE_READONLY,PAGE_NOACCESS,PAGE_EXECUTE_READWRITE,PAGE_READWRITE|PAGE_GUARD}) {
        memory.protect(flags); CHECK(!observeDialoguePosition(memory.sprite(),GetTickCount64()).valid);
    }
}
void partialWriteCanRestoreWritablePrefix() {
    Memory memory;
    const auto sprite=reinterpret_cast<std::uintptr_t>(memory.bytes)+memory.page-0x70-4;
    const auto access=observeDialoguePosition(sprite,GetTickCount64());
    const auto permission=acquireDialoguePosition(access,sprite);
    CHECK(permission.valid&&writeDialoguePosition(permission,original));
    memory.protect(PAGE_READONLY,true);
    CHECK(!writeDialoguePosition(permission,translated));
    CHECK(!writeDialoguePosition(permission,original));
    CHECK(same(sprite,original)); // The unwritable suffix was never changed.
    CHECK(!observeDialoguePosition(sprite,GetTickCount64()).valid);
    memory.protect(PAGE_READWRITE,true); CHECK(writeDialoguePosition(permission,original));
}
}
int main() {
    try { exactXyOnlyAndCapturedRestoration(); leaseDoesNotRenewOrTransfer();
        invalidMemoryAndTimesReject(); partialWriteCanRestoreWritablePrefix(); }
    catch(const std::exception& error) { std::cerr<<error.what()<<'\n';return 1; }
    std::cout<<"Passed 4 dialogue XY permission and restoration cases\n";
}
