#include "display.h"
#include "../platform/dpi.h"
#include <iostream>
#include <string_view>
#include <algorithm>
using namespace bone_eater::launcher;
// Isolated compatibility experiment: change desktop size, keep the exact
// current HDMI target timing, and let the GPU stretch the desktop to it.
void testScaled1080p(const DisplayMode& original) {
    UINT32 pc=0,mc=0;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;
    LONG result=ERROR_INSUFFICIENT_BUFFER;
    for(int attempt=0;attempt<3 && result==ERROR_INSUFFICIENT_BUFFER;++attempt) {
        result=GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS,&pc,&mc);
        if(result!=ERROR_SUCCESS) break;
        paths.resize(pc);modes.resize(mc);
        result=QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS,&pc,paths.data(),&mc,modes.data(),nullptr);
    }
    if(result!=ERROR_SUCCESS) throw std::runtime_error(windowsError("Query display configuration",result));
    paths.resize(pc);modes.resize(mc);
    if(pc!=1) throw std::runtime_error("This manual scaling probe requires one active display; no change made.");
    DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
    source.header={DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME,sizeof(source),paths[0].sourceInfo.adapterId,paths[0].sourceInfo.id};
    result=DisplayConfigGetDeviceInfo(&source.header);
    if(result!=ERROR_SUCCESS || original.device!=source.viewGdiDeviceName)
        throw std::runtime_error("Cannot verify primary display identity; no change made.");
    const auto si=paths[0].sourceInfo.modeInfoIdx,ti=paths[0].targetInfo.modeInfoIdx;
    if(si>=mc || ti>=mc || modes[si].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE ||
            modes[ti].infoType!=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET)
        throw std::runtime_error("Cannot verify saved source/target timing; no change made.");
    auto proposedPaths=paths;auto proposedModes=modes;
    proposedModes[si].sourceMode.width=1920;proposedModes[si].sourceMode.height=1080;
    proposedPaths[0].targetInfo.scaling=DISPLAYCONFIG_SCALING_STRETCHED;
    result=SetDisplayConfig(pc,proposedPaths.data(),mc,proposedModes.data(),SDC_VALIDATE|SDC_USE_SUPPLIED_DISPLAY_CONFIG);
    if(result!=ERROR_SUCCESS) throw std::runtime_error(windowsError("Validate GPU-scaled1080p",result));
    struct Restore {
        std::vector<DISPLAYCONFIG_PATH_INFO>& paths;std::vector<DISPLAYCONFIG_MODE_INFO>& modes;bool armed=true;
        void run() {
            const auto result=SetDisplayConfig(static_cast<UINT32>(paths.size()),paths.data(),
                static_cast<UINT32>(modes.size()),modes.data(),SDC_APPLY|SDC_USE_SUPPLIED_DISPLAY_CONFIG);
            if(result!=ERROR_SUCCESS) throw std::runtime_error(windowsError("Restore saved display configuration",result));
            armed=false;
        }
        ~Restore(){if(armed)try{run();}catch(const std::exception& e){std::cerr<<e.what()<<'\n';}}
    } restore{paths,modes};
    result=SetDisplayConfig(pc,proposedPaths.data(),mc,proposedModes.data(),SDC_APPLY|SDC_USE_SUPPLIED_DISPLAY_CONFIG);
    if(result!=ERROR_SUCCESS) throw std::runtime_error(windowsError("Apply GPU-scaled1080p",result));
    std::cout<<"1080p desktop with saved HDMI timing; restoring in10 seconds.\n"<<activeDisplaySignals()<<std::flush;
    Sleep(10000);
    restore.run();
    std::cout<<"Restored saved display configuration.\n"<<activeDisplaySignals()<<std::flush;
}
// Manual hardware diagnostic. No game, calibration or persistent display change.
int main(int argc,char** argv) {
    try {
        std::cout<<bone_eater::enablePhysicalPixelDpi()<<'\n'<<activeDisplaySignals()<<std::flush;
        if(argc==1) return 0;
        if(argc!=2) return 2;
        const std::string_view option=argv[1];
        if(option!="--test-1080p" && option!="--list-modes" && option!="--test-1080p-59" &&
                option!="--test-1080p-gpu-scaled" && option!="--test-1080p-scaled-session") return 2;
        auto backend=primaryDisplayBackend(option=="--test-1080p-scaled-session");
        if(option=="--test-1080p-gpu-scaled") {testScaled1080p(backend->current());return 0;}
        if(option=="--list-modes") {
            for(const auto& m:backend->modes()) if(m.dmPelsWidth==1920 && m.dmPelsHeight==1080)
                std::cout<<"1080p candidate: Hz="<<m.dmDisplayFrequency<<" bits="<<m.dmBitsPerPel
                    <<" flags="<<m.dmDisplayFlags<<" fixedOutput="<<m.dmDisplayFixedOutput
                    <<" fields="<<m.dmFields<<'\n';
            return 0;
        }
        struct Filtered final:DisplayBackend {
            DisplayBackend& real;bool use59;
            Filtered(DisplayBackend& b,bool f):real(b),use59(f){}
            DisplayMode current() override{return real.current();}
            std::vector<DEVMODEW> modes() override {
                auto values=real.modes();
                if(use59) std::erase_if(values,[](const auto& m){return m.dmDisplayFrequency!=59;});
                return values;
            }
            bool apply(const DisplayMode& m,bool test) override{return real.apply(m,test);}
        } filtered(*backend,option=="--test-1080p-59");
        DisplaySession session(filtered,[](const std::string& line){std::cout<<line<<'\n'<<std::flush;});
        std::cout<<"1080p desktop only; restoring automatically after 10 seconds.\n"<<std::flush;
        Sleep(10000);
        session.restore();
        return 0;
    } catch(const std::exception& e) {std::cerr<<e.what()<<'\n';return 1;}
}
