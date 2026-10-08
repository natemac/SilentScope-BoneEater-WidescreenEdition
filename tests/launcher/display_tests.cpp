#include "display.h"
#include <iostream>
#include <stdexcept>
using namespace bone_eater::launcher;
void require(bool b) { if (!b) throw std::runtime_error("display assertion"); }
template<class F> void rejects(F f) { bool bad=false; try { f(); } catch (...) { bad=true; } require(bad); }
DEVMODEW mode(DWORD x, DWORD y, DWORD hz=60) { DEVMODEW m {};m.dmSize=sizeof(m);m.dmPelsWidth=x;m.dmPelsHeight=y;m.dmBitsPerPel=32;m.dmDisplayFrequency=hz;return m; }
struct Fake : DisplayBackend {
    DisplayMode active {L"primary", mode(3840,2160,120)};
    std::vector<DEVMODEW> available {mode(1920,1080,60),mode(1920,1080,120)};
    bool testOkay=true, applyOkay=true, restoreOkay=true, ignoreRestore=false;
    int changes=0, tests=0;
    DisplayMode current() override {return active;}
    std::vector<DEVMODEW> modes() override {return available;}
    bool apply(const DisplayMode& m, bool test) override {
        if(test) {++tests;return testOkay;}
        ++changes;
        if(!applyOkay || (m.mode.dmPelsWidth==3840 && !restoreOkay)) return false;
        if(m.mode.dmPelsWidth==3840 && ignoreRestore) return true;
        active=m;return true;
    }
};
int main() {try {
    auto log=[](const std::string&){};
    DisplayConfiguration config;
    config.paths.resize(2);config.modes.resize(4);
    for(UINT32 i=0;i<2;++i) {
        auto& p=config.paths[i];p.flags=DISPLAYCONFIG_PATH_ACTIVE;
        p.sourceInfo.modeInfoIdx=i*2;p.targetInfo.modeInfoIdx=i*2+1;
        p.targetInfo.scaling=DISPLAYCONFIG_SCALING_IDENTITY;
        auto& s=config.modes[i*2];s.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
        s.sourceMode.width=3840;s.sourceMode.height=2160;s.sourceMode.position.x=static_cast<LONG>(i*3840);
        auto& t=config.modes[i*2+1];t.infoType=DISPLAYCONFIG_MODE_INFO_TYPE_TARGET;
        t.targetMode.targetVideoSignalInfo.activeSize={3840,2160};
        t.targetMode.targetVideoSignalInfo.pixelRate=594000000;
        t.targetMode.targetVideoSignalInfo.vSyncFreq={60,1};
    }
    const auto scaled=gpuScaled1080p(config,0);
    require(scaled.modes[0].sourceMode.width==1920 && scaled.modes[0].sourceMode.height==1080);
    require(config.modes[0].sourceMode.width==3840); // Saved restoration snapshot unchanged.
    require(scaled.paths[0].targetInfo.scaling==DISPLAYCONFIG_SCALING_STRETCHED);
    for(std::size_t i=1;i<4;++i)require(std::memcmp(&config.modes[i],&scaled.modes[i],sizeof(DISPLAYCONFIG_MODE_INFO))==0);
    require(std::memcmp(&config.paths[1],&scaled.paths[1],sizeof(DISPLAYCONFIG_PATH_INFO))==0);
    auto invalid=config;invalid.paths[0].sourceInfo.modeInfoIdx=DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    rejects([&]{gpuScaled1080p(invalid,0);});
    invalid=config;invalid.paths[1].sourceInfo.modeInfoIdx=0;
    rejects([&]{gpuScaled1080p(invalid,0);});
    invalid=config;invalid.modes[0].sourceMode.width=1080;invalid.modes[0].sourceMode.height=1920;
    rejects([&]{gpuScaled1080p(invalid,0);});
    Fake f; f.active.mode.dmPosition.x=42;
    {DisplaySession s(f,log);require(f.active.mode.dmPelsWidth==1920);require(f.active.mode.dmDisplayFrequency==120);require(f.active.mode.dmPosition.x==42);}
    require(f.active.mode.dmPelsWidth==3840 && f.changes==2);
    rejects([&]{DisplaySession s(f,log);throw std::runtime_error("child crash");});
    require(f.active.mode.dmPelsWidth==3840);
    Fake same;same.active.mode=mode(1920,1080);{DisplaySession s(same,log);}require(same.changes==0 && same.tests==0);
    Fake unsupported;unsupported.available.clear();rejects([&]{DisplaySession s(unsupported,log);});require(unsupported.changes==0);
    Fake rejected;rejected.testOkay=false;rejects([&]{DisplaySession s(rejected,log);});require(rejected.changes==0);
    Fake failed;failed.applyOkay=false;rejects([&]{DisplaySession s(failed,log);});require(failed.active.mode.dmPelsWidth==3840);
    Fake restore;{DisplaySession s(restore,log);restore.restoreOkay=false;rejects([&]{s.restore();});restore.restoreOkay=true;}require(restore.active.mode.dmPelsWidth==3840);
    Fake silent;{DisplaySession s(silent,log);silent.ignoreRestore=true;rejects([&]{s.restore();});silent.ignoreRestore=false;}require(silent.active.mode.dmPelsWidth==3840);
    const std::string json=R"({"schema_version":1,"view":"balanced125","main_dof_off":false,"input_profile":null)";
    require(parseSettings(json+"}").force1080p);require(!parseSettings(json+",\"force_1080p\":false}").force1080p);
    rejects([&]{parseSettings(json+",\"force_1080p\":1}");});
    std::cout<<"Display selection, restoration, failures and settings passed\n";
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
