#include "input/native_precision_bypass.h"
#include "input/precision_pair.h"
#include "input/native_wide_input.h"
#include "input/selected_hid_bridge.h"
#include "render/d3d11_diagnostics.h"
#include "render/native_viewport.h"
#include "diagnostics/optional_csv.h"
#include "util/detour.h"
#include "util/logging.h"
#include <intrin.h>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace bone_eater::input {
namespace {
using Setter = void(__fastcall*)(float);
Setter originalX = nullptr, originalY = nullptr;
std::uintptr_t base = 0;
struct Shared {
    std::atomic<bool> ready {false}, failed {false};
    bool setters = false, scope = false, aim = false, legacy = false;
    std::atomic<DWORD> thread {0};
    std::atomic<ULONGLONG> nextReport {0};
    std::mutex outputMutex;
    FILE* output = nullptr;
    unsigned rows = 0;
};
Shared* shared = nullptr; // All callbacks and storage have process lifetime.
bool attempted = false;
struct Domain { std::uint64_t serial = 0; std::uintptr_t config = 0; render::NativeHudGeometry hud; };
thread_local Domain domain;

struct Snapshot {
    std::uintptr_t owner=0,input=0,config=0,wrapper=0,main=0,manager=0,scope=0,owned=0;
    std::uintptr_t holder=0,ui=0,battle=0,windowObject=0,window=0;
    OwnershipPoint point{},anchor{},offsets{},gains{};
    unsigned state=0,mode=0,script=0;
    float zoom=0;
    std::uint8_t filtered=0,enabled=0,off=1;
    bool operator==(const Snapshot&) const = default;
};
bool sameIdentity(const Snapshot& a,const Snapshot& b) noexcept {
    return a.owner==b.owner && a.input==b.input && a.config==b.config && a.wrapper==b.wrapper &&
        a.main==b.main && a.manager==b.manager && a.scope==b.scope && a.owned==b.owned &&
        a.holder==b.holder && a.ui==b.ui && a.battle==b.battle &&
        a.windowObject==b.windowObject && a.window==b.window;
}
bool sameHud(const render::NativeHudGeometry& a,const render::NativeHudGeometry& b) noexcept {
    return validWideInputGeometry(a) && validWideInputGeometry(b) && a.helper==b.helper && a.sprite==b.sprite &&
        a.left==b.left && a.top==b.top && a.width==b.width && a.height==b.height;
}
struct Warmup { Snapshot identity; render::NativeHudGeometry hud; bool anchor=false,returning=false; };
thread_local Warmup warmup;
struct Transaction {
    bool eligible=false,active=false,returned=false;
    const InputOwnershipState* input=nullptr;
    std::uint64_t serial=0,tick=0;
    Snapshot before;
    PrecisionPair pair;
    unsigned reads=0,bytes=0;
    bool budgetExceeded=false;
};
thread_local Transaction transaction;
thread_local Transaction* active = nullptr;

bool charge(unsigned count,unsigned bytes) noexcept {
    if (!active) return true;
    if (count>96-active->reads || bytes>8192-active->bytes) {
        active->budgetExceeded=true; return false;
    }
    active->reads+=count; active->bytes+=bytes; return true;
}
template<class T> bool read(std::uintptr_t p,std::size_t offset,T& value) noexcept {
    if (!p || p>UINTPTR_MAX-offset || p+offset>UINTPTR_MAX-sizeof(T) || !charge(1,sizeof(T))) return false;
    SIZE_T done=0;
    return ReadProcessMemory(GetCurrentProcess(),reinterpret_cast<const void*>(p+offset),&value,sizeof(T),&done) && done==sizeof(T);
}
template<class T,std::size_t N> T field(const std::array<unsigned char,N>& b,std::size_t offset) noexcept {
    T result{}; std::memcpy(&result,b.data()+offset,sizeof(T)); return result;
}
bool vt(std::uintptr_t p,std::uintptr_t rva) noexcept { std::uintptr_t x=0; return read(p,0,x) && x==base+rva; }
template<std::size_t N> bool matches(std::uintptr_t rva,const std::array<unsigned char,N>& expected) noexcept {
    std::array<unsigned char,N> actual{}; return read(base,rva,actual) && actual==expected;
}
bool focused(std::uintptr_t value) noexcept {
    HWND window=reinterpret_cast<HWND>(value);
    if (!window || std::strcmp(render::registeredD3D11WindowRole(window),"main") ||
        !IsWindow(window) || !IsWindowVisible(window) || IsIconic(window)) return false;
    DWORD owner=0,again=0;
    const HWND foreground=GetForegroundWindow();
    return GetWindowThreadProcessId(window,&owner) && owner==GetCurrentProcessId() &&
        GetAncestor(window,GA_ROOT)==window && foreground && GetAncestor(foreground,GA_ROOT)==window &&
        IsWindowVisible(window) && !IsIconic(window) &&
        GetWindowThreadProcessId(window,&again) && again==owner && GetForegroundWindow()==foreground;
}
bool collect(std::uintptr_t owner,Snapshot& s) noexcept {
    s.owner=owner;
    std::uintptr_t published=0,managerCamera=0;
    std::array<unsigned char,0x68> head{};
    std::array<unsigned char,0x58> state{};
    std::array<unsigned char,0x34> optics{};
    std::uint8_t uiMode=0; unsigned battleState=0;
    if (!read(base,0x13DCF88,published) || published!=owner || !read(owner,0,head) ||
        field<std::uintptr_t>(head,0)!=base+0x10C7350) return false;
    s.scope=field<std::uintptr_t>(head,8); s.owned=field<std::uintptr_t>(head,0x60);
    if (!vt(s.scope,0x6FDF48) || !vt(s.owned,0x6FDF48) ||
        !read(base,0x13DCED0,s.input) || !read(base,0x13DCF58,s.config) || !vt(s.config,0x10C5FA8) ||
        !read(s.config,0x136A,uiMode) || uiMode!=1 || !read(s.config,0x12D8,s.gains) ||
        !read(base,0x13DB588,s.wrapper) || !read(s.wrapper,8,s.main) || !vt(s.main,0x6FDF48) ||
        s.main==s.scope || s.main==s.owned || !read(s.main,0xE88,optics) ||
        field<std::uint8_t>(optics,6)!=0 || field<OwnershipPoint>(optics,0x2C)!=OwnershipPoint{{1920,1080}} ||
        !read(base,0x12E1930,s.manager) || !read(s.manager,0x340,managerCamera) || managerCamera!=s.main ||
        !read(base,0x13DCF48,s.holder) || !read(s.holder,0,s.ui) || !vt(s.ui,0x10CA5E0) ||
        !read(s.ui,0x558,s.off) || s.off!=0 || !read(base,0x13DCF78,s.battle) ||
        !vt(s.battle,0x10CA298) || !read(s.battle,0x48,battleState) || battleState!=3 ||
        !read(base,0x12E17A0,s.windowObject) || !read(s.windowObject,0x28,s.window) ||
        !read(base,0x13D9F7C,s.script) || s.script!=0 || !read(s.input,0x0C,s.point) ||
        !read(owner,0x174,state)) return false;
    s.zoom=field<float>(state,0); s.anchor=field<OwnershipPoint>(state,0x1C);
    s.offsets=field<OwnershipPoint>(state,0x44); s.filtered=field<std::uint8_t>(state,0x4D);
    s.enabled=field<std::uint8_t>(state,0x4E); s.state=field<unsigned>(state,0x50); s.mode=field<unsigned>(state,0x54);
    return validOwnershipPoint(s.point) && s.gains==OwnershipPoint{{.5f,.5f}} &&
        s.offsets==OwnershipPoint{} && std::isfinite(s.anchor[0]) && std::isfinite(s.anchor[1]) &&
        std::isfinite(s.zoom) && s.zoom>0 && s.mode<=3 && !s.filtered && s.enabled==1 &&
        (s.state==0 || s.state==3);
}
bool critical(const Transaction& t) noexcept {
    const auto& s=t.before;
    const std::array<std::pair<std::uintptr_t,std::uintptr_t>,8> globals{{
        {0x13DCF88,s.owner},{0x13DCED0,s.input},{0x13DCF58,s.config},{0x13DB588,s.wrapper},
        {0x12E1930,s.manager},{0x13DCF48,s.holder},{0x13DCF78,s.battle},{0x12E17A0,s.windowObject}}};
    for (const auto& [rva,expected]:globals) { std::uintptr_t p=0; if (!read(base,rva,p) || p!=expected) return false; }
    std::uintptr_t main=0,window=0;
    return t.input && t.input->serial==t.serial && t.input->completed && !t.input->updating &&
        t.input->scopeCalls==1 && t.input->input==s.input &&
        read(s.manager,0x340,main) && main==s.main && read(s.windowObject,0x28,window) && window==s.window;
}
void fault(const char* reason) noexcept {
    if (active) { active->pair.fault=true; active->eligible=false; }
    warmup={};
    if (shared && !shared->failed.exchange(true)) {
        try { log_warning("bone-eater","Precision bypass stopped: {}; native state is not rolled back",reason); } catch (...) {}
    }
}
bool inputIdentity(const Transaction& t) noexcept {
    std::uintptr_t input=0;
    return t.input && t.input->serial==t.serial && t.input->completed && !t.input->updating &&
        t.input->scopeCalls==1 && read(base,0x13DCED0,input) && input==t.before.input;
}
bool freshBeforeX(const Transaction& t) noexcept {
    OwnershipPoint current{};
    return critical(t) && focused(t.before.window) && inputIdentity(t) &&
        read(t.before.input,0x0C,current) &&
        t.input->match(t.before.input,current,GetTickCount64())==OwnershipMatch::Completed;
}
void setter(bool y,float value,std::uintptr_t caller) {
    auto* t=active;
    float argument=value;
    if (t && t->eligible) {
        if (!inputIdentity(*t)) {
            if (t->pair.changed) fault("input identity/serial changed after X");
            else { t->eligible=false; warmup={}; }
        } else if (t->pair.step==PrecisionPair::Step::X && !freshBeforeX(*t)) {
            t->eligible=false; warmup={};
        }
        if (t->eligible) {
            argument=t->pair.argument(y,caller-base,value);
            if (t->pair.fault) fault("unexpected coordinate setter sequence or nonfinite argument");
        }
    }
    // Native exception behavior is unchanged; the Scope boundary clears TLS.
    (y?originalY:originalX)(argument);
    if (t && t->eligible) {
        float stored=0;
        if (!read(t->before.input,y?0x10:0x0C,stored) ||
            std::memcmp(&stored,&argument,sizeof(float))) fault("native setter did not store its argument");
    }
}
__declspec(noinline) void __fastcall setX(float value) { setter(false,value,reinterpret_cast<std::uintptr_t>(_ReturnAddress())); }
__declspec(noinline) void __fastcall setY(float value) { setter(true,value,reinterpret_cast<std::uintptr_t>(_ReturnAddress())); }

void publishReady() noexcept {
    if (shared && shared->setters && shared->scope && shared->aim && shared->legacy) {
        shared->ready.store(true,std::memory_order_release);
        try { log_info("bone-eater","Experimental native precision bypass ready: legacy only; per-branch native warm-up; authored Y/optics retained"); } catch (...) {}
    }
}
void report(const Transaction& t) noexcept {
    if (!shared || !shared->output || !diagnostics::optionalOutputEnabled()) return;
    const auto now=GetTickCount64();
    auto next=shared->nextReport.load();
    if (now<next || !shared->nextReport.compare_exchange_strong(next,now+200)) return;
    try {
        std::unique_lock<std::mutex> lock(shared->outputMutex,std::try_to_lock);
        if (!lock.owns_lock() || !shared->output) return;
        if (fprintf(shared->output,"1,%lu,%lu,%llu,%llu,%u,%u,%u,%u,%u,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%u,%u\n",
                GetCurrentProcessId(),GetCurrentThreadId(),static_cast<unsigned long long>(now),
                static_cast<unsigned long long>(t.serial),t.pair.branch,t.returned,t.pair.substitute,t.pair.complete(),t.pair.fault,
                t.pair.replacement[0],t.pair.replacement[1],t.pair.nativePair[0],t.pair.nativePair[1],
                t.pair.sentPair[0],t.pair.sentPair[1],t.pair.authoredY,t.reads,t.bytes)<0 ||
                fflush(shared->output) || ferror(shared->output) || ++shared->rows>=36000) {
            fclose(shared->output); shared->output=nullptr;
        }
    } catch (...) {} // Optional output never changes functional readiness or pair state.
}
} // namespace

bool nativePrecisionRequested() noexcept {
    static const bool requested=[] { wchar_t value[4]{}; return
        GetEnvironmentVariableW(L"BONE_EATER_NATIVE_PRECISION_BYPASS",value,4)==1 && value[0]==L'1'; }();
    return requested;
}
bool nativePrecisionReady() noexcept {
    return shared && shared->ready.load(std::memory_order_acquire) && !shared->failed.load(std::memory_order_relaxed);
}
bool prepareNativePrecisionBypass(void* gamendd) noexcept {
    if (!nativePrecisionRequested()) return false;
    if (attempted) return shared && shared->setters;
    attempted=true;
    try {
        if (std::strcmp(render::verifyNativeGameModule(gamendd),"verified")) return false;
        const auto source=readSelectedHidBridgeStatus();
        if (!source.configured || source.selected || source.shutdownRequested || source.runtime) {
            log_warning("bone-eater","Precision bypass unavailable: initialized legacy input is required"); return false;
        }
        base=reinterpret_cast<std::uintptr_t>(gamendd);
        constexpr std::array<unsigned char,68> x{{0x48,0x83,0xEC,0x38,0x48,0x8B,0x05,0xA5,0x20,0x35,0x01,0x0F,0x29,0x74,0x24,0x20,0x0F,0x28,0xF0,0x48,0x85,0xC0,0x75,0x1D,0x4C,0x8D,0x05,0x91,0x52,0x03,0x01,0x8D,0x50,0x21,0x48,0x8D,0x0D,0x17,0x52,0x03,0x01,0xE8,0x82,0xF4,0x10,0x00,0x48,0x8B,0x05,0x7B,0x20,0x35,0x01,0xF3,0x0F,0x11,0x70,0x0C,0x0F,0x28,0x74,0x24,0x20,0x48,0x83,0xC4,0x38,0xC3}};
        constexpr std::array<unsigned char,68> y{{0x48,0x83,0xEC,0x38,0x48,0x8B,0x05,0x55,0x20,0x35,0x01,0x0F,0x29,0x74,0x24,0x20,0x0F,0x28,0xF0,0x48,0x85,0xC0,0x75,0x1D,0x4C,0x8D,0x05,0x41,0x52,0x03,0x01,0x8D,0x50,0x21,0x48,0x8D,0x0D,0xC7,0x51,0x03,0x01,0xE8,0x32,0xF4,0x10,0x00,0x48,0x8B,0x05,0x2B,0x20,0x35,0x01,0xF3,0x0F,0x11,0x70,0x10,0x0F,0x28,0x74,0x24,0x20,0x48,0x83,0xC4,0x38,0xC3}};
        constexpr std::array<unsigned char,16> anchor{{0x0F,0x28,0xC1,0xE8,0xC5,0xDE,0xFB,0xFF,0x0F,0x28,0xC6,0xE8,0x0D,0xDF,0xFB,0xFF}};
        constexpr std::array<unsigned char,17> returning{{0x0F,0x28,0xC7,0xE8,0x6A,0xDD,0xFB,0xFF,0x41,0x0F,0x28,0xC0,0xE8,0xB1,0xDD,0xFB,0xFF}};
        constexpr std::array<unsigned char,135> authored{{0x44,0x38,0xB3,0x48,0x02,0x00,0x00,0x74,0x07,0xF3,0x0F,0x10,0x41,0x10,0xEB,0x24,0x48,0x8B,0x05,0x99,0xFE,0x30,0x01,0x44,0x0F,0xBE,0x88,0x88,0x02,0x00,0x00,0x66,0x41,0x0F,0x6E,0xC1,0x0F,0x5B,0xC0,0xF3,0x0F,0x59,0x05,0xC5,0x3C,0x00,0x01,0xF3,0x0F,0x58,0x41,0x10,0xE8,0xF7,0xDC,0xFB,0xFF,0x4C,0x8B,0x0D,0x50,0xFD,0x30,0x01,0x4D,0x85,0xC9,0x75,0x1E,0x4C,0x8D,0x05,0x44,0x2F,0xFF,0x00,0x41,0x8D,0x51,0x21,0x48,0x8D,0x0D,0xC9,0x2E,0xFF,0x00,0xE8,0x34,0xD1,0x0C,0x00,0x4C,0x8B,0x0D,0x2D,0xFD,0x30,0x01,0x48,0x8B,0x05,0x86,0x47,0x21,0x01,0xF3,0x41,0x0F,0x10,0x59,0x10,0xF3,0x41,0x0F,0x10,0x51,0x0C,0x48,0x8D,0x4C,0x24,0x30,0x48,0x8B,0x90,0x40,0x03,0x00,0x00,0xE8,0x99,0x99,0x0C,0x00}};
        if (!matches(0x8AE20,x) || !matches(0x8AE70,y) || !matches(0xCCF53,anchor) ||
            !matches(0xCD0AE,returning) || !matches(0xCD140,authored)) {
            log_warning("bone-eater","Precision bypass disabled: pinned setter/caller bytes differ"); return false;
        }
        HMODULE pinned=nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_PIN,
            reinterpret_cast<LPCWSTR>(gamendd),&pinned) || pinned!=gamendd) return false;
        shared=new Shared; shared->legacy=true;
        originalX=reinterpret_cast<Setter>(base+0x8AE20); originalY=reinterpret_cast<Setter>(base+0x8AE70);
        if (!detour::trampoline_try(originalX,&setX,&originalX) || !detour::trampoline_try(originalY,&setY,&originalY)) {
            log_warning("bone-eater","Precision bypass disabled: paired setter hooks incomplete"); return false;
        }
        shared->setters=true;
        shared->output=diagnostics::openOptionalCsv(L"desktop/native-precision-v1.csv",[](FILE* f) {
            fputs("schema,pid,thread,tick_ms,serial,branch,returned,substitute,complete,fault,accepted_x,accepted_y,native_x,native_y,sent_x,sent_y,authored_y,read_calls,read_bytes\n",f);
        });
        return true;
    } catch (...) { return shared && shared->setters; }
}
void nativePrecisionScopeInstalled(void* module) noexcept {
    if (shared && reinterpret_cast<std::uintptr_t>(module)==base) { shared->scope=true; publishReady(); }
}
void nativePrecisionAimInstalled(void* module) noexcept {
    if (shared && reinterpret_cast<std::uintptr_t>(module)==base) { shared->aim=true; publishReady(); }
}
void recordNativePrecisionDomain(std::uint64_t serial,std::uintptr_t config,const render::NativeHudGeometry& hud) noexcept {
    if (nativePrecisionReady()) domain={serial,config,hud};
}
NativePrecisionScopeToken beginNativePrecisionScope(std::uintptr_t owner,std::uintptr_t caller,float delta,
        const InputOwnershipState& input,bool nested) noexcept {
    NativePrecisionScopeToken token{active,false};
    if (active) {
        if (active->pair.changed) fault("nested Scope invocation after substitution");
        else { active->eligible=false; warmup={}; }
    }
    active=nullptr;
    if (!nativePrecisionReady() || nested || caller!=base+0xAA923) return token;
    transaction={}; active=&transaction; token.owns=true;
    auto& t=transaction; t.input=&input; t.serial=input.serial; t.tick=GetTickCount64();
    Snapshot again{};
    // readNativeHudGeometry has a fixed eight-read committed-geometry path;
    // reserve its upper bound too, although its helper owns the actual reads.
    const bool hudBudget=charge(8,512);
    const auto hud=hudBudget?render::readNativeHudGeometry():render::NativeHudGeometry{};
    if (!std::isfinite(delta) || delta<0 || !hudBudget || domain.serial!=input.serial ||
        !sameHud(domain.hud,hud) || !collect(owner,t.before) || !collect(owner,again) || !(t.before==again) ||
        domain.config!=t.before.config || !focused(t.before.window) ||
        input.match(t.before.input,t.before.point,GetTickCount64())!=OwnershipMatch::Completed) { warmup={}; return token; }
    DWORD expected=0; const DWORD thread=GetCurrentThreadId();
    if (!shared->thread.compare_exchange_strong(expected,thread) && expected!=thread) {
        fault("eligible Scope changed native thread"); return token;
    }
    if (!sameIdentity(warmup.identity,t.before) || !sameHud(warmup.hud,hud)) warmup={t.before,hud,false,false};
    t.pair.begin(t.before.state,input.converted,t.before.state==0?warmup.anchor:warmup.returning);
    t.eligible=!t.pair.fault; t.active=true;
    return token;
}
void finishNativePrecisionScope(NativePrecisionScopeToken token,bool returned) noexcept {
    if (token.owns) {
        auto& t=transaction; t.returned=returned;
        if (t.active && t.eligible) {
            OwnershipPoint point{};
            if (!returned || !t.pair.complete() || !critical(t) || !read(t.before.input,0x0C,point) ||
                !sameOwnershipPoint(point,{{t.pair.sentPair[0],t.pair.authoredY}}))
                fault("incomplete Scope/setter/ray transaction or changed identity");
            else if (!focused(t.before.window)) warmup={};
            else if (t.pair.branch==0) warmup.anchor=true; else warmup.returning=true;
        } else if (!returned && t.pair.changed) fault("native exception after substituted X");
        // Never leave this transaction armed during optional I/O.
        active=nullptr;
        if (t.active) report(t);
    }
    active=static_cast<Transaction*>(token.previous);
}
void observeNativePrecisionRay(std::uintptr_t camera,float x,float y,bool verified,bool known,bool off) noexcept {
    if (!active || !active->eligible) return;
    auto& t=*active;
    t.pair.ray({{x,y}},verified && known && !off && camera==t.before.main && inputIdentity(t));
    if (t.pair.fault) fault("final native ray disagrees with preserved setter sequence");
}
} // namespace bone_eater::input
