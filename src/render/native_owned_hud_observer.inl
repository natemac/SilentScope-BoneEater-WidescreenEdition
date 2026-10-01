// Included only by native_front_observer.cpp. Read-only, explicit diagnostic opt-in.
// This probe deliberately records quad ordinals: several GUI leaves share one
// native sprite/material, so sprite identity cannot certify a complete draw.
namespace owned_hud_probe {
using Submit = void(__fastcall*)(void*);
using SpriteDraw = void(__fastcall*)(void*, void*, int);
Submit originalSubmit = nullptr;
SpriteDraw originalSpriteDraw = nullptr;
std::uintptr_t module = 0;
std::atomic<unsigned> remainingRows {0};
template<typename T, std::size_t N>
T field(const std::array<unsigned char,N>& data, std::size_t offset) noexcept {
    T value {};
    std::memcpy(&value,data.data()+offset,sizeof(value));
    return value;
}
struct Leaf { std::size_t offset; const char* name; };
constexpr Leaf leaves[] = {
    {0x1C0,"lcd_bt_heart_0"},{0x1C8,"lcd_bt_heart_1"},{0x1D0,"lcd_bt_heart_2"},
    {0x1D8,"lcd_bt_heart_3"},{0x1E0,"lcd_bt_heart_4"},{0x1E8,"lcd_bt_heart_num"},
    {0x200,"lcd_bt_bulletnum_2"},{0x208,"lcd_bt_bulletnum_1"},{0x210,"lcd_bt_bulletnum_0"},
    {0x218,"lcd_bt_bulletnum_2F"},{0x220,"lcd_bt_bulletnum_1F"},{0x228,"lcd_bt_bulletnum_0F"},
    {0x230,"lcd_bt_bullet_unlimited"},{0x288,"lcd_bt_font_score"},
    {0x290,"lcd_bt_scorenum_0"},{0x298,"lcd_bt_scorenum_1"},{0x2A0,"lcd_bt_scorenum_2"},
    {0x2A8,"lcd_bt_scorenum_3"},{0x2B0,"lcd_bt_scorenum_4"},{0x2B8,"lcd_bt_scorenum_5"},
    {0x2C0,"lcd_bt_scorenum_6"},{0x2C8,"lcd_bt_scorenum_7"},
    {0x6A8,"lcd_bt_accRateNum_0"},{0x6B0,"lcd_bt_accRateNum_1"},{0x6B8,"lcd_bt_accRateNum_2"}
};
struct Identity {
    std::uintptr_t owner=0, gui=0, record=0, sprite=0, material=0;
    unsigned group=0;
    bool operator==(const Identity&) const = default;
};
struct Local {
    ULONGLONG refresh=0;
    std::array<Identity,std::size(leaves)> identities {};
    std::array<bool,std::size(leaves)> reported {};
    std::uintptr_t drawingSprite=0;
    bool drawingEligible=false, drawReported=false;
};
thread_local Local local;

bool identityFor(std::size_t index, Identity& id) noexcept {
    std::uintptr_t manager=0, vt=0, root=0, value=0;
    unsigned state=0;
    if (!read(module,0x13DCF78,manager) || !read(manager,0,vt) || vt!=module+0x10CA298 ||
            !read(manager,0x48,state) || state!=3 || !read(manager,0xD0,id.owner) ||
            !read(id.owner,0,vt) || vt!=module+0x10CA1D8 ||
            !read(id.owner,0x48,state) || state!=3 || !read(id.owner,0xE0,id.group) ||
            !id.group || id.group>=32 || !read(id.owner,0x140,root) || !root ||
            !read(id.owner,leaves[index].offset,id.gui)) return false;
    std::array<unsigned char,0x118> gui {};
    if (!read(id.gui,0,gui) || field<std::uintptr_t>(gui,0)!=module+0x10CEBA8 ||
            field<std::uintptr_t>(gui,8)!=id.gui || field<unsigned>(gui,0x3C)!=id.group ||
            !std::memchr(gui.data()+0x40,0,32) ||
            std::strcmp(reinterpret_cast<const char*>(gui.data()+0x40),leaves[index].name)) return false;
    value=field<std::uintptr_t>(gui,0x10);
    bool ancestor=false;
    for (unsigned depth=0;depth<8 && value;++depth) {
        std::array<unsigned char,0x60> parent {};
        if (!read(value,0,parent) || field<std::uintptr_t>(parent,8)!=value ||
                field<unsigned>(parent,0x3C)!=id.group ||
                (field<std::uintptr_t>(parent,0)!=module+0x10CEBA8 &&
                 field<std::uintptr_t>(parent,0)!=module+0x10CEB48)) return false;
        if (value==root) {
            if (!std::memchr(parent.data()+0x40,0,32) ||
                    std::strcmp(reinterpret_cast<const char*>(parent.data()+0x40),"root_upper")) return false;
            ancestor=true; break;
        }
        value=field<std::uintptr_t>(parent,0x10);
    }
    if (!ancestor) return false;
    id.record=field<std::uintptr_t>(gui,0x30);
    id.sprite=field<std::uintptr_t>(gui,0x110);
    unsigned selector=0;
    std::uintptr_t camera=0, slot=0;
    return read(id.record,0,vt) && vt==module+0x10CEC08 &&
        read(id.record,0x40,value) && value==id.sprite &&
        read(id.sprite,0,vt) && (vt==module+0x10CC0D8 || vt==module+0x10CD1E8) &&
        read(id.sprite,0x570,selector) && selector==2 &&
        read(id.sprite,0x1A8,camera) && read(module,0x13DB5B0,slot) && camera==slot && camera &&
        read(id.sprite,0x308,id.material) && read(id.material,0,vt) && vt==module+0x706570;
}
void refresh() noexcept {
    const auto now=GetTickCount64();
    if (now<local.refresh) return;
    local.refresh=now+1000;
    local.reported={}; local.drawReported=false;
    for (std::size_t i=0;i<std::size(leaves);++i) {
        Identity id;
        local.identities[i]=identityFor(i,id) ? id : Identity{};
    }
}
bool takeRow() noexcept {
    unsigned left=remainingRows.load();
    while (left && !remainingRows.compare_exchange_weak(left,left-1)) {}
    return left!=0;
}
struct Observation {
    Identity identity;
    std::size_t index=std::size(leaves);
    unsigned ordinal=0, bytes=0;
    std::uintptr_t cursor=0;
};
Observation beforeSubmit(void* record) noexcept {
    Observation result;
    if (!remainingRows.load()) return result;
    refresh();
    for (std::size_t i=0;i<std::size(leaves);++i) {
        if (local.reported[i] || local.identities[i].record!=reinterpret_cast<std::uintptr_t>(record)) continue;
        Identity current;
        if (!identityFor(i,current) || !(current==local.identities[i]) ||
                !read(current.material,0x44,result.ordinal) || result.ordinal>16384 ||
                !read(current.material,0x40,result.bytes) ||
                !read(current.material,0x38,result.cursor)) return result;
        result.identity=current; result.index=i; return result;
    }
    return result;
}
void afterSubmit(const Observation& observation) noexcept {
    if (observation.index==std::size(leaves)) return;
    try {
        Identity current;
        unsigned ordinal=0, bytes=0;
        std::uintptr_t cursor=0;
        const auto& id=observation.identity;
        if (!identityFor(observation.index,current) || !(current==id) ||
                !read(id.material,0x44,ordinal) || !read(id.material,0x40,bytes) ||
                !read(id.material,0x38,cursor) || !takeRow()) return;
        local.reported[observation.index]=true;
        log_info("bone-eater", "Owned HUD quad tick={} thread={} name={} gui={:x} record={:x} sprite={:x} material={:x} ordinal_before={} ordinal_after={} bytes_before={} bytes_after={} cursor_before={:x} cursor_after={:x} single_quad={}",
            GetTickCount64(),GetCurrentThreadId(),leaves[observation.index].name,id.gui,id.record,id.sprite,id.material,
            observation.ordinal,ordinal,observation.bytes,bytes,observation.cursor,cursor,
            ordinal==observation.ordinal+1 && bytes==observation.bytes+0x70 &&
            observation.cursor && cursor==observation.cursor+0x70);
    } catch (...) {}
}
void __fastcall submitHook(void* record) {
    const auto observation=beforeSubmit(record);
    originalSubmit(record);
    afterSubmit(observation);
}
bool eligibleSprite(void* sprite) noexcept {
    if (!remainingRows.load()) return false;
    refresh();
    for (std::size_t i=0;i<std::size(leaves);++i) {
        if (local.identities[i].sprite!=reinterpret_cast<std::uintptr_t>(sprite)) continue;
        Identity current;
        if (identityFor(i,current) && current==local.identities[i]) return true;
    }
    return false;
}
void __fastcall spriteDrawHook(void* sprite, void* records, int count) {
    const auto previous=local.drawingSprite;
    const bool eligible=local.drawingEligible;
    local.drawingSprite=reinterpret_cast<std::uintptr_t>(sprite);
    local.drawingEligible=count>0 && count<=256 && eligibleSprite(sprite);
    __try { originalSpriteDraw(sprite,records,count); }
    __finally { local.drawingSprite=previous; local.drawingEligible=eligible; }
}
void primitive(void* object, bool nativeFrontTarget) noexcept {
    if (!remainingRows.load() || !local.drawingEligible || local.drawReported) return;
    try {
        std::array<unsigned char,0x20> p {};
        if (!read(reinterpret_cast<std::uintptr_t>(object),0,p) || !takeRow()) return;
        local.drawReported=true;
        log_info("bone-eater", "Owned HUD batch tick={} thread={} sprite={:x} primitive={:x} index_record={:x} vertex_record={:x} type={} count={} native_front_target={} ownership=batch_not_leaf",
            GetTickCount64(),GetCurrentThreadId(),local.drawingSprite,reinterpret_cast<std::uintptr_t>(object),
            field<std::uintptr_t>(p,8),field<std::uintptr_t>(p,0x10),
            field<unsigned short>(p,0x18),field<unsigned short>(p,0x1A),nativeFrontTarget);
    } catch (...) {}
}
void install(std::uintptr_t address) noexcept {
    wchar_t value[4] {};
    if (GetEnvironmentVariableW(L"BONE_EATER_OWNED_HUD_OBSERVE",value,4)!=1 || value[0]!=L'1' || originalSubmit) return;
    // Parent observer has verified and pinned the complete native module.
    module=address;
    constexpr std::array<unsigned char,18> submitBytes {{0x48,0x89,0x5c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xa9,0x48,0x81,0xec,0xd0,0,0,0}};
    constexpr std::array<unsigned char,20> drawBytes {{0x48,0x89,0x5c,0x24,0x08,0x48,0x89,0x6c,0x24,0x10,0x48,0x89,0x74,0x24,0x18,0x57,0x48,0x83,0xec,0x20}};
    std::array<unsigned char,18> s {};
    std::array<unsigned char,20> d {};
    if (!read(module,0x1ECD80,s) || s!=submitBytes || !read(module,0x1A0E60,d) || d!=drawBytes) return;
    originalSubmit=reinterpret_cast<Submit>(module+0x1ECD80);
    if (!detour::trampoline_try(originalSubmit,&submitHook,&originalSubmit)) return;
    originalSpriteDraw=reinterpret_cast<SpriteDraw>(module+0x1A0E60);
    if (!detour::trampoline_try(originalSpriteDraw,&spriteDrawHook,&originalSpriteDraw)) return;
    remainingRows=256;
    try { log_info("bone-eater", "Read-only owned HUD record/batch probe installed; <=256total rows, per-leaf <=1Hz; shared batches are not certified leaf draws"); } catch (...) {}
}
} // namespace owned_hud_probe
