#include "calibration.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <windows.h>
using namespace bone_eater::launcher;
void require(bool x) { if (!x) throw std::runtime_error("calibration assertion failed"); }
template<class F> void rejects(F f) { bool bad=false; try { f(); } catch(...) {bad=true;} require(bad); }
std::string read(const std::filesystem::path& p) {std::ifstream f(p,std::ios::binary); return {std::istreambuf_iterator<char>(f),{}};}
void write(const std::filesystem::path& p,const std::string& s) {std::ofstream f(p,std::ios::binary);f<<s;}
int main(int argc, char** argv) { try {
    if (argc == 3) { // Read-only validation of a real XML/CRC pair.
        auto result=fullRangeCalibration(read(argv[1]),read(argv[2]));
        std::cout<<result.first; return 0;
    }
    require(calibrationCrc("123456789")==std::string("\x26\x39\xf4\xcb",4));
    std::string xml="<testModeValue><other>keep me</other><guncontrolleCheck><calibrated><current>1</current></calibrated>";
    std::string expected=xml;
    for(const auto* side:{"left","right"}) for(const auto* field:{"Min","Max","Nut"}) for(const auto* axis:{"X","Y"}) {
        std::string name=std::string(side)+"Calibration"+field+axis;
        std::string value=std::string(field)=="Min"?"0":std::string(field)=="Max"?"4095":"2047";
        std::string pre="<"+name+"><factory>"+value+"</factory><current type=\"s32\"> ",post=" </current></"+name+">";
        xml+=pre+"3000"+post;expected+=pre+value+post;
    }
    xml+="</guncontrolleCheck></testModeValue>";expected+="</guncontrolleCheck></testModeValue>";
    auto result=fullRangeCalibration(xml,calibrationCrc(xml)); require(result.first==expected);require(result.second==calibrationCrc(expected));
    require(fullRangeCalibration(expected,result.second).first==expected);
    rejects([&]{fullRangeCalibration(xml,"bad");});
    rejects([&]{fullRangeCalibration(xml+xml,calibrationCrc(xml+xml));});
    auto invalid=xml; invalid.replace(invalid.find("<factory>0"),10,"<factory>9");
    rejects([&]{fullRangeCalibration(invalid,calibrationCrc(invalid));});
    auto root=std::filesystem::temp_directory_path()/("be-cal-test-"+std::to_string(GetCurrentProcessId()));
    auto dir=root/"game"; std::filesystem::create_directories(dir/"conf/nvram");
    write(dir/"ADD GAME FILES HERE.md","fixture");write(dir/"BoneEater.exe","not executable");
    auto xp=dir/"conf/nvram/testmode-v.xml", cp=dir/"conf/nvram/testmode-v.crc";
    write(xp,xml);write(cp,calibrationCrc(xml));
    prepareBetaCalibration(dir/"BoneEater.exe");require(read(xp)==expected);require(read(cp)==calibrationCrc(expected));
    require(read(dir/"desktop/calibration-initialization-r3/testmode-v.xml")==xml);
    require(read(dir/"desktop/calibration-initialization-r3/testmode-v.crc")==calibrationCrc(xml));
    // Later intentional calibration must not be reset on subsequent launches.
    write(xp,xml);write(cp,calibrationCrc(xml));prepareBetaCalibration(dir/"BoneEater.exe");require(read(xp)==xml);
    std::filesystem::remove(dir/"desktop/calibration-initialization-r3/complete.txt");
    rejects([&]{prepareBetaCalibration(dir/"BoneEater.exe");});require(read(xp)==xml);
    std::filesystem::remove_all(root);
    std::cout<<"Calibration CRC, byte preservation, rejection, backup, migration and idempotence passed\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what();return 1;} }
