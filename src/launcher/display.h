#pragma once
#include "launcher.h"
#include <functional>
#include <memory>
namespace bone_eater::launcher {
struct DisplayMode { std::wstring device; DEVMODEW mode {}; };
struct DisplayBackend {
    virtual ~DisplayBackend() = default;
    virtual DisplayMode current() = 0;
    virtual std::vector<DEVMODEW> modes() = 0;
    virtual bool apply(const DisplayMode&, bool test) = 0;
};
struct DisplayConfiguration {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
};
DisplayConfiguration gpuScaled1080p(const DisplayConfiguration&, std::size_t primaryPath);
// Preserve the working HDMI timing by default; false is for native-mode probes.
std::unique_ptr<DisplayBackend> primaryDisplayBackend(bool preserveSignal = true);
// Read-only CCD source/target timings; desktop size alone does not identify
// the physical HDMI signal (GPU scaling may keep a different target mode).
std::string activeDisplaySignals();
class DisplaySession {
    DisplayBackend& backend;
    std::function<void(const std::string&)> log;
    DisplayMode original;
    bool changed = false;
public:
    DisplaySession(DisplayBackend&, std::function<void(const std::string&)>);
    DisplaySession(const DisplaySession&) = delete;
    DisplaySession& operator=(const DisplaySession&) = delete;
    ~DisplaySession();
    void restore();
};
}
