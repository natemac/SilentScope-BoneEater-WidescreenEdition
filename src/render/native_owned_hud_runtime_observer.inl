// Include native_owned_hud_runtime.h at translation-unit scope first. This
// adapter is included inside native_front_observer.cpp's anonymous namespace.
// Pure observation: no replay, suppression, native data or graphics writes.
namespace owned_hud_runtime_observer {
using Reset=void(__fastcall*)(void*);
using Submit=void(__fastcall*)(void*);
using Manager=void(__fastcall*)(void*);
using Sort=void(__fastcall*)(void*,int);
Manager originalManager=nullptr;
Sort originalSort=nullptr;
std::atomic<bool> admissionInstalled {false};
struct AdmissionScope {
    std::uintptr_t manager=0;
    std::uint64_t token=0;
    bool firstSort=false,invalid=false;
};
thread_local AdmissionScope admissionContext {};
Reset originalReset=nullptr;
Submit originalImage=nullptr,originalFont=nullptr;
// Trampolines remain installed through process teardown. Keep their mutex and
// ledger alive rather than depending on static destructor ordering.
OwnedHudRuntime* runtime=nullptr;
std::atomic<std::uintptr_t> observedRearMaterial{0};
std::atomic<bool> installed {false};
std::atomic<unsigned> rows {0};
std::atomic<ULONGLONG> nextRow {0};
std::uintptr_t module=0;
bool readBytes(std::uintptr_t address,void* value,std::size_t size) noexcept {
    SIZE_T copied=0;
    return address && size && ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(address),
        value,size,&copied) && copied==size;
}
std::atomic<unsigned> lifecycleRows{0};
void lifecycle(const char* event,std::uintptr_t material,std::uintptr_t caller=0) noexcept {
    if(!diagnostics::optionalOutputEnabled()||!observedRearMaterial.load()||lifecycleRows.fetch_add(1)>=160)return;
    auto rd=runtime->rearDiagnostics();unsigned count=0;std::uint16_t prepared=0;std::uintptr_t vb=0;
    readBytes(material+0x44,&count,4);readBytes(material+0x1a,&prepared,2);readBytes(material+0x10,&vb,8);
    log_info("bone-eater","HUD lifecycle {} thread={} frame={} rearframe={} seen={} reason={} material={:x} count={} prepared={} vb={:x} caller={:x}",event,GetCurrentThreadId(),rd.current,rd.raw.frame,rd.raw.seen,rd.reason,material,count,prepared,vb,caller?caller-module:0);
}
void __fastcall resetHook(void* material) {
    if(reinterpret_cast<std::uintptr_t>(material)==observedRearMaterial.load()||runtime->ownsMaterial(reinterpret_cast<std::uintptr_t>(material)))lifecycle("reset",reinterpret_cast<std::uintptr_t>(material),reinterpret_cast<std::uintptr_t>(_ReturnAddress()));
    const auto ticket=runtime->resetBefore(reinterpret_cast<std::uintptr_t>(material));
    originalReset(material);
    runtime->resetAfter(ticket);
}
void __fastcall imageHook(void* record) {
    if(captureLobbyImpact(module,reinterpret_cast<std::uintptr_t>(record)))return;
    if(scoreGameplayLabel(module,reinterpret_cast<std::uintptr_t>(record),false,
        [](auto p,auto o,auto& v)noexcept{return readBytes(p+o,&v,sizeof(v));}))return;
    if(scoreGameplayBackplate(module,reinterpret_cast<std::uintptr_t>(record),
        [](auto p,auto o,auto& v)noexcept{return readBytes(p+o,&v,sizeof(v));}))return;
    const auto ticket=runtime->beforeSubmit(reinterpret_cast<std::uintptr_t>(record));
    unsigned rearIndex=2;
    const auto rear=certifiedNativeBattleBackgroundRecord(reinterpret_cast<std::uintptr_t>(record),rearIndex)
        ? runtime->beforeRear(reinterpret_cast<std::uintptr_t>(record),rearIndex) : OwnedHudRuntime::RearTicket{};
    if(rear.material)observedRearMaterial.store(rear.material);
    originalImage(record);
    runtime->afterRear(rear);
    if(rear.index<2)lifecycle("rear-appended",rear.material);
    if(diagnostics::optionalOutputEnabled()&&rearIndex<2){static std::atomic<ULONGLONG> due[2]{};const auto now=GetTickCount64();auto prior=due[rearIndex].load();
        unsigned count=0,bytes=0;readBytes(rear.material+0x44,&count,4);readBytes(rear.material+0x40,&bytes,4);
        auto pair=runtime->rearPair();
        if(now>=prior&&due[rearIndex].compare_exchange_strong(prior,now+1000))log_info("bone-eater","Wide HUD rear adapter index={} ticket_index={} frame={} material={:x} first={} after={} bytes={} pair={}",rearIndex,rear.index,rear.frame,rear.material,rear.first,count,bytes,pair.seen);}
    runtime->afterSubmit(ticket);
}
void __fastcall fontHook(void* record) {
    if(scoreGameplayLabel(module,reinterpret_cast<std::uintptr_t>(record),true,
        [](auto p,auto o,auto& v)noexcept{return readBytes(p+o,&v,sizeof(v));}))return;
    const auto ticket=runtime->beforeSubmit(reinterpret_cast<std::uintptr_t>(record));
    originalFont(record);
    runtime->afterSubmit(ticket);
}
bool requested() noexcept {
    wchar_t value[4] {};
    if(wideHudRequested())return true;
    return GetEnvironmentVariableW(L"BONE_EATER_OWNED_HUD_COLLECT",value,4)==1 && value[0]==L'1';
}
bool admissionRequested() noexcept {
    wchar_t value[4] {};
    if(wideHudRequested())return true;
    return GetEnvironmentVariableW(L"BONE_EATER_OWNED_HUD_ADMISSION",value,4)==1 && value[0]==L'1';
}
// POD-only SEH boundary: observation methods contain C++ allocation failures;
// native exceptions propagate after invalidating provisional evidence and TLS.
void __fastcall managerHook(void* manager) {
    auto previous=admissionContext;
    const bool nested=previous.manager!=0;
    if (nested) { previous.invalid=true; runtime->abort(); }
    admissionContext={reinterpret_cast<std::uintptr_t>(manager),0,false,nested};
    __try { originalManager(manager); }
    __finally {
        if (admissionInstalled.load()) {
            if (AbnormalTermination() || admissionContext.invalid || !admissionContext.firstSort) runtime->abort();
            else runtime->admissionFinish(admissionContext.token,true);
        }
        admissionContext=previous;
    }
}
void __fastcall sortHook(void* list,int count) {
    if (admissionInstalled.load() && admissionContext.manager &&
            reinterpret_cast<std::uintptr_t>(_ReturnAddress())==module+0x1E62E5) {
        if (admissionContext.firstSort || admissionContext.invalid) {
            admissionContext.invalid=true; runtime->abort();
        } else {
            // Font adapter+10 resets its material during the registry scan.
            // Start the epoch-bound proof only after that scan has completed.
            admissionContext.firstSort=true;
            admissionContext.token=runtime->admissionBegin(admissionContext.manager);
            runtime->admissionCapture(admissionContext.token,reinterpret_cast<std::uintptr_t>(list),count);
        }
    }
    originalSort(list,count);
}
void primitive(void* material,bool nativeFrontTarget) noexcept {
    if(wideHudRequested())return;
    if (!installed.load() || !nativeFrontTarget || rows.load()>=120) return;
    try {
        const auto id=reinterpret_cast<std::uintptr_t>(material);
        if (runtime->active() && !runtime->ownsMaterial(id)) return;
        // Deliberately hypothetical certificates for read-only observations.
        // This call never authorizes replacement or claims backing readiness.
        const auto plan=runtime->prepareDraw(id,true,{{true,true,true}},true);
        const auto d=runtime->diagnostics();
        if (!d.frame) return;
        const auto now=GetTickCount64();
        auto due=nextRow.load();
        if (now<due || !nextRow.compare_exchange_strong(due,now+1000)) return;
        if (rows.fetch_add(1)>=120) return;
        unsigned left=0,right=0,center=0,native=0;
        for (std::size_t i=0;i<plan.count;++i) {
            const auto& r=plan.ranges[i];
            if (r.destination==OwnedHudGroup::Left) left+=r.indexCount;
            else if (r.destination==OwnedHudGroup::Right) right+=r.indexCount;
            else if (r.destination==OwnedHudGroup::Center) center+=r.indexCount;
            else native+=r.indexCount;
        }
        log_info("bone-eater", "Owned HUD complete collector tick={} thread={} frame={} image_epoch={} font_epoch={} active={} sealed={} collect_stage={} collect_node={} primitive={:x} hypothetical_plan={} plan_frame={} total_quads={} ranges={} left_indices={} right_indices={} center_indices={} native_indices={} current={} seen={},{},{} missing={},{},{} pending={},{},{} reject={:x},{:x},{:x} missing_mask={:x} admission_mask={:x} admission_pending={} observation_only=1",
            now,GetCurrentThreadId(),d.frame,d.imageEpoch,d.fontEpoch,d.active,d.sealed,d.collectStage,d.node,id,
            plan.replacement,plan.frame,plan.totalQuads,plan.count,left,right,center,native,runtime->stillCurrent(plan),
            d.selection.seen[0],d.selection.seen[1],d.selection.seen[2],
            d.selection.missing[0],d.selection.missing[1],d.selection.missing[2],
            d.selection.pending[0],d.selection.pending[1],d.selection.pending[2],
            d.selection.reasons[0],d.selection.reasons[1],d.selection.reasons[2],d.selection.missingMask,d.admissionMask,d.admissionToken);
        log_info("bone-eater","Owned HUD admission stage={} frame={}",d.admissionStage,d.frame);
    } catch (...) {}
}
template<std::size_t N> bool exact(std::uintptr_t rva,const std::array<unsigned char,N>& expected) noexcept {
    std::array<unsigned char,N> actual {};
    return readBytes(module+rva,actual.data(),actual.size()) && actual==expected;
}
void install(std::uintptr_t baseAddress) noexcept {
    try {
    if (!requested() || originalReset || installed.load()) return;
    // Parent has verified/pinned module and skipped old record probe install.
    module=baseAddress;
    constexpr std::array<unsigned char,22> reset {{0x40,0x57,0x48,0x83,0xec,0x20,0x48,0x83,0x79,0x10,0,0x48,0x8b,0xf9,0x75,0x06,0x80,0x61,0x36,0xfe,0xeb,0x39}};
    constexpr std::array<unsigned char,18> image {{0x48,0x89,0x5c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xa9,0x48,0x81,0xec,0xd0,0,0,0}};
    // Both adapter hooks use pinned whole-instruction prefixes of >=14 bytes.
    constexpr std::array<unsigned char,18> font {{0x48,0x89,0x5c,0x24,0x10,0x55,0x48,0x8d,0x6c,0x24,0xa9,0x48,0x81,0xec,0xd0,0,0,0}};
    if (!exact(0x329310,reset) || !exact(0x1ECD80,image) || !exact(0x1ECB50,font)) {
        log_warning("bone-eater","Owned HUD complete collector rejected instruction bytes"); return;
    }
    constexpr std::array<unsigned char,15> manager {{0x41,0x56,0xb8,0xa0,0x9c,0,0,0xe8,0x94,0xbe,0x4e,0,0x48,0x2b,0xe0}};
    constexpr std::array<unsigned char,19> sort {{0x48,0x89,0x5c,0x24,0x18,0x55,0x56,0x57,0x41,0x56,0x41,0x57,0x48,0x81,0xec,0x30,0x02,0,0}};
    if (admissionRequested() && (!exact(0x1E6140,manager) || !exact(0x1E6B80,sort))) {
        log_warning("bone-eater","Owned HUD admission rejected instruction bytes"); return;
    }
    runtime=new OwnedHudRuntime;
    runtime->initialize(module,&readBytes);
    originalReset=reinterpret_cast<Reset>(module+0x329310);
    if (!detour::trampoline_try(originalReset,&resetHook,&originalReset)) return;
    originalImage=reinterpret_cast<Submit>(module+0x1ECD80);
    if (!detour::trampoline_try(originalImage,&imageHook,&originalImage)) return;
    originalFont=reinterpret_cast<Submit>(module+0x1ECB50);
    if (!detour::trampoline_try(originalFont,&fontHook,&originalFont)) return;
    if (admissionRequested()) {
        originalSort=reinterpret_cast<Sort>(module+0x1E6B80);
        if (!detour::trampoline_try(originalSort,&sortHook,&originalSort)) return;
        originalManager=reinterpret_cast<Manager>(module+0x1E6140);
        if (!detour::trampoline_try(originalManager,&managerHook,&originalManager)) return;
        admissionInstalled.store(true);
        log_info("bone-eater","Read-only HUD admission installed: secondary manager base, first-sort membership, completion-only omissions");
    }
    installed.store(true);
    log_info("bone-eater","Owned HUD collector installed: exact 55-node selection, immutable upload packets, guarded native fallback");
    } catch (...) {
        installed.store(false);
        // Any already-installed trampoline remains a native pass-through.
    }
}
} // namespace owned_hud_runtime_observer
