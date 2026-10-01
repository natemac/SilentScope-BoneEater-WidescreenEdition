#include "input/scoped_motion.h"
#include "input/scope_settings.h"
#include <cassert>
#include <iostream>
#include <map>

using namespace bone_eater::input;
bool close(double a, double b, double tolerance = .0001) { return std::abs(a-b) < tolerance; }
int main() {
    ScopedMotion motion;
    ScopedMotionSettings immediate {.25,.1,0,0};
    auto p = motion.update({960,540},false,false,100,immediate);
    assert(close(p.x,960));
    p = motion.update({960,540},true,false,116,immediate); assert(close(p.x,960));
    p = motion.update({1060,640},true,false,132,immediate); assert(close(p.x,985) && close(p.y,565));
    p = motion.update({1060,640},true,true,148,immediate); assert(close(p.x,985));
    p = motion.update({1160,740},true,true,164,immediate); assert(close(p.x,995) && close(p.y,575));
    p = motion.update({1160,740},true,false,180,immediate); assert(close(p.x,995));
    p = motion.update({1160,740},false,false,196,immediate); assert(close(p.x,995));
    p = motion.update({1160,740},false,false,316,immediate); assert(close(p.x,1160) && close(p.y,740));
    p = motion.update({500,200},false,false,332,immediate); assert(close(p.x,500) && close(p.y,200));
    // Same fixed target, different update cadences: time-based smoothing agrees.
    ScopedMotion fast,slow; ScopedMotionSettings smooth {1,1,40,40};
    for(auto* f:{&fast,&slow}) { f->update({900,500},false,false,100,smooth); f->update({900,500},true,false,100,smooth); }
    for(unsigned t=110;t<=300;t+=10) p=fast.update({1000,600},true,false,t,smooth);
    ScopedPoint q; for(unsigned t=120;t<=300;t+=20) q=slow.update({1000,600},true,false,t,smooth);
    assert(close(p.x,q.x) && close(p.y,q.y));
    // Alternating hand jitter is attenuated and does not accumulate drift.
    motion.reset(); motion.update({960,540},true,false,100,smooth);
    double maxJitter=0;
    for(unsigned i=1;i<=100;i++) { p=motion.update({960+(i%2?4.0:-4.0),540},true,false,100+i*10,smooth); maxJitter=std::max(maxJitter,std::abs(p.x-960)); }
    assert(maxJitter<2);
    motion.reset(); p=motion.update({0,1080},true,false,100,immediate);
    p=motion.update({1920,0},true,false,116,immediate); assert(close(p.x,480) && close(p.y,810));
    // Invalid points must not poison future state; next valid sample reanchors.
    motion.update({NAN,0},true,false,120,smooth);
    p=motion.update({200,300},false,false,130,smooth); assert(close(p.x,200));
    assert(parseScopeBindings("ENTER,RBUTTON,XBUTTON1,A,0").size()==5);
    for(const auto* bad:{"", "ENTER,", "ENTER,ENTER", "enter", "F999"}) {
        bool rejected=false; try{parseScopeBindings(bad);}catch(...){rejected=true;} assert(rejected);
    }
    for(const auto* bad:{"nan", "inf", "0", "1.1", "x"}) {
        bool rejected=false; try{validateScopeOption("--scope-low-gain",bad);}catch(...){rejected=true;} assert(rejected);
    }
    // Adaptive movement starts with precision gain, increases only after
    // sustained travel, and settles back without moving a stationary aim.
    ScopedMotion adaptive, fixed;
    ScopedMotionSettings adaptiveSettings {.25,.10,0,0};
    adaptiveSettings.adaptive.enabled = true;
    adaptiveSettings.adaptive.lowMaxGain = 1;
    adaptiveSettings.adaptive.highMaxGain = .7;
    for (auto* policy : {&adaptive, &fixed}) {
        policy->update({200,540},false,false,100,adaptiveSettings);
        policy->update({200,540},true,false,116,adaptiveSettings);
    }
    ScopedMotionSettings fixedSettings = adaptiveSettings;
    fixedSettings.adaptive.enabled = false;
    auto first = adaptive.update({210,540},true,false,132,adaptiveSettings);
    assert(first.x >= 202.5 && first.x < 205);
    fixed.update({210,540},true,false,132,fixedSettings);
    for (unsigned i=1;i<=10;i++) {
        const ScopedPoint raw {210.0+80*i,540};
        p=adaptive.update(raw,true,false,132+16*i,adaptiveSettings);
        q=fixed.update(raw,true,false,132+16*i,fixedSettings);
    }
    assert(p.x > q.x+80 && p.x < 1920);
    const auto held = p.x;
    for (unsigned i=1;i<=30;i++) p=adaptive.update({1010,540},true,false,292+16*i,adaptiveSettings);
    assert(close(p.x,held));
    p=adaptive.update({1015,540},true,false,788,adaptiveSettings);
    assert(p.x-held >= 1.25 && p.x-held < 2.0);
    const auto beforeZoom = p.x;
    p=adaptive.update({1015,540},true,true,804,adaptiveSettings);
    assert(close(p.x,beforeZoom));
    p=adaptive.update({1015,540},true,false,820,adaptiveSettings);
    assert(close(p.x,beforeZoom));
    p=adaptive.update({NAN,540},true,false,836,adaptiveSettings);
    assert(close(p.x,beforeZoom));
    p=adaptive.update({1400,540},true,false,852,adaptiveSettings);
    assert(close(p.x,beforeZoom)); // Reacquire after invalid input without a jump.
    p=adaptive.update({1410,540},true,false,868,adaptiveSettings);
    assert(p.x>beforeZoom);

    ScopedMotion edge;
    ScopedMotionSettings edgeSettings {.25,.10,0,0};
    edgeSettings.adaptive.enabled = true;
    edgeSettings.adaptive.edgePan = true;
    edgeSettings.adaptive.edgeDwellMs = 100;
    edgeSettings.adaptive.edgeMaxSpeed = .5;
    edge.update({960,540},false,false,100,edgeSettings);
    edge.update({960,540},true,false,116,edgeSettings);
    for (unsigned i=1;i<=5;i++) p=edge.update({960.0+188*i,540},true,false,116+16*i,edgeSettings);
    const auto beforeDwell=p.x;
    for (unsigned i=1;i<=20;i++) p=edge.update({1900,540},true,false,196+16*i,edgeSettings);
    assert(p.x>beforeDwell+100 && p.x<=1920);
    p=edge.update({1500,540},true,false,532,edgeSettings);
    const auto afterInward=p.x;
    for (unsigned i=1;i<=10;i++) p=edge.update({1500,540},true,false,532+16*i,edgeSettings);
    assert(close(p.x,afterInward)); // Edge motion stops on inward movement.
    for(const auto* bad:{"-1","2","true"}) {
        bool rejected=false; try{validateScopeOption("--scope-adaptive-enabled",bad);}catch(...){rejected=true;} assert(rejected);
    }
    // Dwell crossing contributes only the elapsed time after the threshold.
    edge.reset();
    edge.update({960,540},true,false,100,edgeSettings);
    edge.update({1900,540},true,false,116,edgeSettings); // Reanchor spike.
    p=edge.update({1900,540},true,false,200,edgeSettings);
    assert(close(p.x,960));
    p=edge.update({1900,540},true,false,220,edgeSettings);
    assert(close(p.x,963.84)); // Four milliseconds at .5 widths/second.
    // All four edges reach their native boundary, discard excess, and respond
    // immediately inward. Repeat at high zoom and with different entry points.
    for (bool higher : {false,true}) for (double entry : {.2,.5,.8})
        for (int side=0;side<4;++side) {
            edge.reset();
            const ScopedPoint start {entry*1920,entry*1080};
            ScopedPoint boundary = start;
            if(side<2) boundary.x=side?1920:0; else boundary.y=side==3?1080:0;
            edge.update(start,true,higher,100,edgeSettings);
            for(unsigned t=116;t<=20116;t+=16) p=edge.update(boundary,true,higher,t,edgeSettings);
            assert(close(side<2?p.x:p.y,side<2?boundary.x:boundary.y));
            if(side<2) boundary.x+=side?-20:20; else boundary.y+=side==3?-20:20;
            p=edge.update(boundary,true,higher,20132,edgeSettings);
            assert(side<2?(p.x>0 && p.x<1920):(p.y>0 && p.y<1080));
        }
    // Missing timestamps, backwards clocks, off-screen input and spikes hold
    // aim, then resume using only newly acquired movement.
    for (int loss=0;loss<4;++loss) {
        adaptive.reset();
        adaptive.update({500,500},true,false,100,adaptiveSettings);
        const unsigned resume = loss==0?500:132;
        if(loss==1) adaptive.update({-1,500},true,false,116,adaptiveSettings);
        if(loss==2) adaptive.update({1700,500},true,false,116,adaptiveSettings);
        if(loss==3) adaptive.update({700,500},true,false,90,adaptiveSettings);
        const double reacquiredX = loss==3?700:500;
        p=adaptive.update({reacquiredX,500},true,false,resume,adaptiveSettings);
        assert(close(p.x,500));
        p=adaptive.update({reacquiredX+10,500},true,false,resume+16,adaptiveSettings);
        assert(p.x>500 && p.x<510);
    }
    // Exiting after invalid input must clear suspension, including reentry at
    // the same timestamp followed by a valid movement.
    adaptive.reset();
    adaptive.update({500,500},true,false,100,adaptiveSettings);
    adaptive.update({NAN,500},true,false,116,adaptiveSettings);
    adaptive.update({600,500},false,false,132,adaptiveSettings);
    adaptive.update({600,500},true,false,132,adaptiveSettings);
    p=adaptive.update({610,500},true,false,148,adaptiveSettings);
    assert(p.x>500);
    // Accepted thresholds above two widths/second must activate travel gain.
    auto fastSettings=adaptiveSettings;
    fastSettings.adaptive.speedStart=2.1; fastSettings.adaptive.speedFull=3;
    fastSettings.adaptive.rampUpMs=10;
    adaptive.reset(); adaptive.update({0,540},true,false,100,fastSettings);
    for(unsigned i=1;i<=20;++i) p=adaptive.update({i*90.0,540},true,false,100+i*16,fastSettings);
    assert(p.x>700); // Fixed precision would end at 450.
    // A constant-speed path produces comparable travel across common cadences.
    double cadenceReference=0;
    for(unsigned step : {4u,8u,16u,20u}) {
        adaptive.reset(); adaptive.update({200,540},true,false,100,adaptiveSettings);
        for(unsigned t=step;t<=800;t+=step)
            p=adaptive.update({200+1.2*t,540},true,false,100+t,adaptiveSettings);
        if(!cadenceReference) cadenceReference=p.x;
        assert(std::abs(p.x-cadenceReference)<15);
    }
    // Duplicate timestamps do not amplify motion; next timed sample includes
    // the latest physical point once.
    adaptive.reset(); fixed.reset();
    for(auto* policy : {&adaptive,&fixed}) policy->update({500,540},true,false,100,adaptiveSettings);
    adaptive.update({550,540},true,false,100,adaptiveSettings);
    adaptive.update({560,540},true,false,100,adaptiveSettings);
    p=adaptive.update({560,540},true,false,116,adaptiveSettings);
    q=fixed.update({560,540},true,false,116,adaptiveSettings);
    assert(close(p.x,q.x));
    // Master disabled retains fixed behavior even when the edge switch is set.
    auto disabled=edgeSettings; disabled.adaptive.enabled=false;
    edge.reset(); fixed.reset();
    for(unsigned i=0;i<100;++i) {
        const ScopedPoint point {1800.0+i,540};
        p=edge.update(point,true,false,100+i*16,disabled);
        q=fixed.update(point,true,false,100+i*16,fixedSettings);
        assert(close(p.x,q.x) && close(p.y,q.y));
    }
    validateScopeOption("--scope-adaptive-low-max-gain","0.000001");
    bool rejected=false; try{validateScopeOption("--scope-adaptive-low-max-gain","0");}catch(...){rejected=true;} assert(rejected);
    // Signed filtering and activation dwell keep alternating hand tremor at
    // precision gain even though its unsigned speed exceeds speed_start.
    adaptive.reset(); fixed.reset();
    for(unsigned i=0;i<100;++i) {
        const ScopedPoint point {500+(i%2?6.0:0.0),540};
        p=adaptive.update(point,true,false,100+i*10,adaptiveSettings);
        q=fixed.update(point,true,false,100+i*10,fixedSettings);
        assert(close(p.x,q.x));
    }
    // A single fast sample must not activate acceleration on its own.
    adaptive.reset(); adaptive.update({500,540},true,false,100,adaptiveSettings);
    p=adaptive.update({700,540},true,false,116,adaptiveSettings);
    assert(close(p.x,550));
    for(unsigned t=132;t<=196;t+=16) p=adaptive.update({700,540},true,false,t,adaptiveSettings);
    p=adaptive.update({701,540},true,false,212,adaptiveSettings);
    assert(close(p.x,550.25));
    // Runtime/CLI preflight rejects cross-field errors even with master off,
    // matching JSON; omitted adaptive preserves all legal fixed gains.
    std::map<std::string,std::string> settingsEnvironment;
    const auto readSettings = [&] {
        return readScopedMotionSettings([&](const char* name,const char* fallback) {
            const auto found=settingsEnvironment.find(name);
            return found==settingsEnvironment.end()?std::string(fallback):found->second;
        });
    };
    settingsEnvironment["BONE_EATER_SCOPE_HIGH_GAIN"]=".9";
    assert(close(readSettings().highGain,.9));
    settingsEnvironment["BONE_EATER_SCOPE_ADAPTIVE_ENABLED"]="0";
    rejected=false; try{readSettings();}catch(const std::exception& e){rejected=std::string(e.what()).find("max gains")!=std::string::npos;} assert(rejected);
    settingsEnvironment.clear();
    settingsEnvironment["BONE_EATER_SCOPE_ADAPTIVE_SPEED_START"]=".9";
    rejected=false; try{readSettings();}catch(const std::exception& e){rejected=std::string(e.what()).find("speed_full")!=std::string::npos;} assert(rejected);
    settingsEnvironment.clear();
    settingsEnvironment["BONE_EATER_SCOPE_ADAPTIVE_EDGE_ENABLED"]="1";
    assert(!readSettings().adaptive.edgePan);
    settingsEnvironment["BONE_EATER_SCOPE_ADAPTIVE_ENABLED"]="1";
    assert(readSettings().adaptive.edgePan);
    settingsEnvironment["BONE_EATER_SCOPE_LOW_GAIN"]=".000001";
    settingsEnvironment["BONE_EATER_SCOPE_ADAPTIVE_LOW_MAX_GAIN"]=".000001";
    assert(close(readSettings().adaptive.lowMaxGain,.000001));
    std::cout << "scoped motion, adaptive travel, edge and binding checks passed\n";
}
