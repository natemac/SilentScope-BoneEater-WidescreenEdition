#include "display.h"
#include <algorithm>
#include <cmath>
#include <stdexcept>
#include <sstream>
#include <optional>
namespace bone_eater::launcher {
namespace {
DisplayConfiguration captureConfiguration() {
    DisplayConfiguration config;
    UINT32 pc=0,mc=0;
    LONG result=ERROR_INSUFFICIENT_BUFFER;
    for(int attempt=0;attempt<3 && result==ERROR_INSUFFICIENT_BUFFER;++attempt) {
        result=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pc,&mc);
        if(result!=ERROR_SUCCESS) break;
        config.paths.resize(pc);config.modes.resize(mc);
        result=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pc,config.paths.data(),&mc,config.modes.data(),nullptr);
    }
    if(result!=ERROR_SUCCESS) throw std::runtime_error(windowsError("Read display configuration",result));
    config.paths.resize(pc);config.modes.resize(mc);
    return config;
}
class WindowsDisplay final : public DisplayBackend {
    HANDLE guard = nullptr;
    std::wstring device;
    bool preserveSignal;
    std::optional<DisplayConfiguration> saved, scaled;
    void prepareScaled() {
        if(saved) return;
        auto original=captureConfiguration();
        std::optional<std::size_t> primary;
        for(std::size_t i=0;i<original.paths.size();++i) {
            const auto& p=original.paths[i];
            DISPLAYCONFIG_SOURCE_DEVICE_NAME name{};
            name.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(name),p.sourceInfo.adapterId,p.sourceInfo.id};
            const LONG result=DisplayConfigGetDeviceInfo(&name.header);
            if(result!=ERROR_SUCCESS) throw std::runtime_error(windowsError("Identify display source",result));
            if(device==name.viewGdiDeviceName) {
                if(primary) throw std::runtime_error("Mirrored primary displays are not supported by the 1080p switch. Use extended displays or set force_1080p to false.");
                primary=i;
            }
        }
        if(!primary) throw std::runtime_error("Primary display path not found; no resolution change made.");
        auto target=gpuScaled1080p(original,*primary);
        saved=std::move(original);scaled=std::move(target);
    }
public:
    explicit WindowsDisplay(bool keepSignal) : preserveSignal(keepSignal) {
        guard = CreateMutexW(nullptr, FALSE, L"Local\\BoneEaterWidescreenDisplaySession");
        if (!guard) throw std::runtime_error(windowsError("Create display guard"));
        auto wait = WaitForSingleObject(guard, 0);
        if (wait != WAIT_OBJECT_0 && wait != WAIT_ABANDONED) {
            CloseHandle(guard); guard = nullptr;
            throw std::runtime_error("Another Bone Eater launcher owns the display session.");
        }
        DISPLAY_DEVICEW d {}; d.cb = sizeof(d);
        for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &d, 0); ++i) {
            if (d.StateFlags & DISPLAY_DEVICE_PRIMARY_DEVICE) { device = d.DeviceName; break; }
        }
        if (device.empty()) { ReleaseMutex(guard); CloseHandle(guard); guard = nullptr;
            throw std::runtime_error("Cannot identify the primary display."); }
    }
    ~WindowsDisplay() override { if (guard) { ReleaseMutex(guard); CloseHandle(guard); } }
    DisplayMode current() override {
        DisplayMode result; result.device = device; result.mode.dmSize = sizeof(DEVMODEW);
        if (!EnumDisplaySettingsExW(device.c_str(), ENUM_CURRENT_SETTINGS, &result.mode, 0))
            throw std::runtime_error("Cannot read the current display mode.");
        return result;
    }
    std::vector<DEVMODEW> modes() override {
        std::vector<DEVMODEW> result;
        for (DWORD i = 0;; ++i) {
            DEVMODEW mode {}; mode.dmSize = sizeof(mode);
            if (!EnumDisplaySettingsExW(device.c_str(), i, &mode, 0)) break;
            result.push_back(mode);
        }
        return result;
    }
    bool apply(const DisplayMode& mode, bool test) override {
        if(preserveSignal) {
            prepareScaled();
            auto config=(mode.mode.dmPelsWidth==1920 && mode.mode.dmPelsHeight==1080) ? *scaled : *saved;
            const LONG result=SetDisplayConfig(static_cast<UINT32>(config.paths.size()),config.paths.data(),
                static_cast<UINT32>(config.modes.size()),config.modes.data(),
                (test ? SDC_VALIDATE : SDC_APPLY)|SDC_USE_SUPPLIED_DISPLAY_CONFIG);
            if(result!=ERROR_SUCCESS && !test)
                throw std::runtime_error(windowsError("Apply saved-signal display configuration",result));
            return result==ERROR_SUCCESS;
        }
        auto copy = mode.mode;
        return ChangeDisplaySettingsExW(mode.device.c_str(), &copy, nullptr,
            test ? CDS_TEST : CDS_FULLSCREEN, nullptr) == DISP_CHANGE_SUCCESSFUL;
    }
};
std::string describe(const DisplayMode& value) {
    return narrow(value.device) + " " + std::to_string(value.mode.dmPelsWidth) + "x" +
        std::to_string(value.mode.dmPelsHeight) + " @" + std::to_string(value.mode.dmDisplayFrequency) + "Hz";
}
}
DisplayConfiguration gpuScaled1080p(const DisplayConfiguration& original, std::size_t primaryPath) {
    auto result=original;
    if(primaryPath>=result.paths.size()) throw std::runtime_error("Invalid primary display path.");
    auto& path=result.paths[primaryPath];
    const auto si=path.sourceInfo.modeInfoIdx,ti=path.targetInfo.modeInfoIdx;
    if(si>=result.modes.size() || ti>=result.modes.size() ||
            result.modes[si].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
            result.modes[ti].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET ||
            !(path.flags&DISPLAYCONFIG_PATH_ACTIVE))
        throw std::runtime_error("Cannot verify source and signal modes; no resolution change made.");
    for(std::size_t i=0;i<result.paths.size();++i) if(i!=primaryPath && result.paths[i].sourceInfo.modeInfoIdx==si)
        throw std::runtime_error("Mirrored display source cannot be resized independently.");
    auto& source=result.modes[si].sourceMode;
    if(source.height>source.width)
        throw std::runtime_error("Primary desktop must be landscape for 1080p; no rotation change made.");
    source.width=1920;source.height=1080;
    path.targetInfo.scaling=DISPLAYCONFIG_SCALING_STRETCHED;
    return result;
}
std::unique_ptr<DisplayBackend> primaryDisplayBackend(bool preserveSignal) { return std::make_unique<WindowsDisplay>(preserveSignal); }
std::string activeDisplaySignals() {
    UINT32 pathCount=0, modeCount=0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result=ERROR_INSUFFICIENT_BUFFER;
    for(int attempt=0;attempt<3 && result==ERROR_INSUFFICIENT_BUFFER;++attempt) {
        result=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pathCount,&modeCount);
        if(result!=ERROR_SUCCESS) break;
        paths.resize(pathCount);modes.resize(modeCount);
        result=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pathCount,paths.data(),&modeCount,modes.data(),nullptr);
    }
    if(result!=ERROR_SUCCESS) return "Display signal query unavailable: "+std::to_string(result);
    std::ostringstream out;
    for(UINT32 i=0;i<pathCount;++i) {
        const auto& p=paths[i];
        DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
        source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),p.sourceInfo.adapterId,p.sourceInfo.id};
        DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
        target.header={DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME,sizeof(target),p.targetInfo.adapterId,p.targetInfo.id};
        DisplayConfigGetDeviceInfo(&source.header);DisplayConfigGetDeviceInfo(&target.header);
        out<<"Signal "<<narrow(source.viewGdiDeviceName)<<" -> "<<narrow(target.monitorFriendlyDeviceName);
        if(p.sourceInfo.modeInfoIdx<modeCount && modes[p.sourceInfo.modeInfoIdx].infoType==DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
            const auto& s=modes[p.sourceInfo.modeInfoIdx].sourceMode;
            out<<"; desktop="<<s.width<<'x'<<s.height;
        }
        if(p.targetInfo.modeInfoIdx<modeCount && modes[p.targetInfo.modeInfoIdx].infoType==DISPLAYCONFIG_MODE_INFO_TYPE_TARGET) {
            const auto& s=modes[p.targetInfo.modeInfoIdx].targetMode.targetVideoSignalInfo;
            out<<"; active="<<s.activeSize.cx<<'x'<<s.activeSize.cy
               <<"; total="<<s.totalSize.cx<<'x'<<s.totalSize.cy
               <<"; refresh="<<s.vSyncFreq.Numerator<<'/'<<s.vSyncFreq.Denominator
               <<"; pixelClock="<<s.pixelRate<<"; scan="<<s.scanLineOrdering;
        }
        out<<"; scaling="<<p.targetInfo.scaling<<'\n';
    }
    return out.str();
}
DisplaySession::DisplaySession(DisplayBackend& b, std::function<void(const std::string&)> logger)
    : backend(b), log(std::move(logger)), original(b.current()) {
    log("Original display: " + describe(original));
    log(activeDisplaySignals());
    if (original.mode.dmPelsWidth == 1920 && original.mode.dmPelsHeight == 1080) {
        log("Already 1920x1080; no mode change."); return;
    }
    auto modes = backend.modes();
    std::erase_if(modes, [&](const auto& m) { return m.dmPelsWidth != 1920 || m.dmPelsHeight != 1080 ||
        m.dmBitsPerPel != original.mode.dmBitsPerPel || m.dmDisplayOrientation != original.mode.dmDisplayOrientation ||
        (m.dmDisplayFlags & DM_INTERLACED); });
    std::stable_sort(modes.begin(), modes.end(), [&](const auto& a, const auto& b) {
        return std::abs(static_cast<int>(a.dmDisplayFrequency) - static_cast<int>(original.mode.dmDisplayFrequency)) <
            std::abs(static_cast<int>(b.dmDisplayFrequency) - static_cast<int>(original.mode.dmDisplayFrequency));
    });
    for (auto m : modes) {
        m.dmPosition = original.mode.dmPosition;
        m.dmFields |= DM_POSITION;
        DisplayMode target { original.device, m };
        if (!backend.apply(target, true)) continue;
        if (!backend.apply(target, false)) throw std::runtime_error("Windows rejected temporary 1080p mode. Set force_1080p to false to opt out.");
        changed = true;
        try {
            const auto active = backend.current();
            if (active.mode.dmPelsWidth != 1920 || active.mode.dmPelsHeight != 1080)
                throw std::runtime_error("Windows did not activate 1920x1080.");
            log("Temporary display: " + describe(active));
            log(activeDisplaySignals());
        } catch (...) { restore(); throw; }
        return;
    }
    throw std::runtime_error("Primary display cannot activate a supported 1920x1080 desktop with this display configuration. Set force_1080p to false to keep your desktop resolution.");
}
void DisplaySession::restore() {
    if (!changed) return;
    if (!backend.apply(original, false)) throw std::runtime_error("Could not restore the original display mode. Restore it in Windows Display Settings.");
    const auto actual = backend.current();
    const auto& a = actual.mode;
    const auto& b = original.mode;
    if (actual.device != original.device || a.dmPelsWidth != b.dmPelsWidth || a.dmPelsHeight != b.dmPelsHeight ||
        a.dmDisplayFrequency != b.dmDisplayFrequency || a.dmBitsPerPel != b.dmBitsPerPel ||
        a.dmDisplayOrientation != b.dmDisplayOrientation || a.dmPosition.x != b.dmPosition.x || a.dmPosition.y != b.dmPosition.y)
        throw std::runtime_error("Windows did not restore the saved display mode. Restore it in Windows Display Settings.");
    changed = false;
    log("Restored display: " + describe(actual));
    log(activeDisplaySignals());
}
DisplaySession::~DisplaySession() {
    try { restore(); } catch (const std::exception& e) { try { log(e.what()); } catch (...) {} }
}
}
