#include "calibration.h"
#include "player_data.h"
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
    const std::string existingClock="<clock><clock_offset><is_set>0</is_set><current>321</current></clock_offset></clock>";
    xml.insert(xml.find("<other>"),existingClock);
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
    auto dir=root/"game"; auto user=root/"user"; std::filesystem::create_directories(user/"conf/nvram"); std::filesystem::create_directories(dir);
    write(dir/"ADD GAME FILES HERE.md","fixture");write(dir/"BoneEater.exe","not executable");
    auto xp=user/"conf/nvram/testmode-v.xml", cp=user/"conf/nvram/testmode-v.crc";
    write(xp,xml);write(cp,calibrationCrc(xml));
    prepareBetaCalibration(dir/"BoneEater.exe");require(read(xp)==expected);require(read(cp)==calibrationCrc(expected));
    require(read(user/"calibration-initialization-r4/1/source.xml")==xml);
    require(read(user/"calibration-initialization-r4/1/source.crc")==calibrationCrc(xml));
    require(std::filesystem::file_size(user/"conf/raw/dx")==0);
    // Validate even with marker; do not silently reset later custom calibration.
    write(xp,xml);write(cp,calibrationCrc(xml));rejects([&]{prepareBetaCalibration(dir/"BoneEater.exe");});require(read(xp)==xml);
    prepareBetaCalibration(dir/"BoneEater.exe",true);require(read(xp)==expected);
    write(user/"conf/raw/dx","preserve marker contents");
    prepareBetaCalibration(dir/"BoneEater.exe");require(read(user/"conf/raw/dx")=="preserve marker contents");
    // User regression: renaming dx after successful setup must recover even
    // when the completion marker exists. Preserve calibration and renamed file.
    const auto dx=user/"conf/raw/dx", savedDx=user/"conf/raw/dx.renamed";
    std::filesystem::rename(dx,savedDx);
    prepareBetaCalibration(dir/"BoneEater.exe");
    require(std::filesystem::is_regular_file(dx) && std::filesystem::file_size(dx)==0);
    require(read(savedDx)=="preserve marker contents");
    require(read(xp)==expected && read(cp)==calibrationCrc(expected));
    prepareBetaCalibration(dir/"BoneEater.exe"); // Zero-byte marker is valid.
    require(std::filesystem::file_size(dx)==0);
    // An existing marker that ARK cannot open must stop launch with dx context.
    HANDLE held=CreateFileW(dx.c_str(),GENERIC_READ,0,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    require(held!=INVALID_HANDLE_VALUE);
    std::string markerError;
    try {prepareBetaCalibration(dir/"BoneEater.exe");} catch(const std::exception& e){markerError=e.what();}
    CloseHandle(held);
    require(markerError.find("required cabinet marker")!=std::string::npos);
    require(markerError.find("No game launched")!=std::string::npos);
    std::filesystem::remove(dx);std::filesystem::create_directory(dx);
    rejects([&]{prepareBetaCalibration(dir/"BoneEater.exe");});
    std::filesystem::remove(dx);
    prepareBetaCalibration(dir/"BoneEater.exe");
    require(read(xp)==expected && read(cp)==calibrationCrc(expected));
    auto zero=expected; zero.replace(zero.find("<current>1"),10,"<current>0");
    require(fullRangeCalibration(zero,calibrationCrc(zero)).first==expected);
    // Completely missing conf uses the supplied template and calibrated flag 1.
    auto fresh=root/"fresh/game";std::filesystem::create_directories(fresh/"prop");
    auto templ=zero;templ.replace(templ.find("<current>321"),12,"<current>0");
    templ.insert(templ.find("<other>"),"<version __type=\"s32\">9</version>");
    write(fresh/"prop/testmode-v.xml",templ);write(fresh/"BoneEater.exe","fixture");
    prepareBetaCalibration(fresh/"BoneEater.exe");
    auto freshXml=read(root/"fresh/user/conf/nvram/testmode-v.xml");
    require(freshXml.find("<calibrated><current>1")!=std::string::npos);
    require(freshXml.find("<clock_offset><is_set>1</is_set><current>0</current>")!=std::string::npos);
    require(read(fresh/"prop/testmode-v.xml")==templ);
    require(read(root/"fresh/user/conf/nvram/testmode-v.crc")==calibrationCrc(freshXml));
    require(!std::filesystem::exists(fresh/"conf"));
    std::filesystem::remove(root/"fresh/user/conf/nvram/testmode-v.crc");
    rejects([&]{prepareBetaCalibration(fresh/"BoneEater.exe");});
    std::filesystem::create_directory(user/"calibration-initialization-r4/incomplete");
    rejects([&]{prepareBetaCalibration(dir/"BoneEater.exe");});
    std::filesystem::remove_all(root);
    std::cout<<"Calibration CRC, byte preservation, rejection, backup, migration and idempotence passed\n";
    return 0;
} catch(const std::exception& e){std::cerr<<e.what();return 1;} }
