#include "player_data.h"
#include "launcher.h"
#include "../../src/platform/private_storage.h"
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace bone_eater::launcher;
void require(bool b){if(!b)throw std::runtime_error("player data assertion");}
template<class F> void rejects(F f){bool b=false;try{f();}catch(...){b=true;}require(b);}
std::string read(const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);return {std::istreambuf_iterator<char>(f),{}};}
void write(const std::filesystem::path& p,const std::string& s){std::filesystem::create_directories(p.parent_path());std::ofstream f(p,std::ios::binary);f<<s;}
int main(){try{
    require(bone_eater::isCabinetBookkeepingMount("/dev/raw/bookkeeping", "./CONF/RAW/BOOKKEEPING", "nvram"));
    require(!bone_eater::isCabinetBookkeepingMount("/dev/raw", "./CONF/RAW/BOOKKEEPING", "nvram"));
    require(!bone_eater::isCabinetBookkeepingMount("/dev/raw/bookkeeping", "custom/save", "fs"));
    require(!bone_eater::isCabinetBookkeepingMount("/dev/raw/bookkeeping", "./CONF/RAW/BOOKKEEPING", "fs"));
    require(!bone_eater::isCabinetBookkeepingMount(nullptr, nullptr, nullptr));
    const std::string source="<config><fs><root><device>.</device></root><nvram><device type=\"str\">.\\conf\\nvram</device><fstype>fs</fstype></nvram><raw><device>.\\conf\\raw</device></raw></fs><!-- retain bytes --></config>";
    auto mapped=redirectedAvsConfig(source);require(mapped.find("../user/conf/nvram")!=std::string::npos);require(mapped.find("<root><device>.</device></root>")!=std::string::npos);require(mapped.find("<!-- retain bytes -->")!=std::string::npos);
    rejects([&]{redirectedAvsConfig("<config/>");});rejects([&]{redirectedAvsConfig(source+source);});
    auto root=std::filesystem::temp_directory_path()/("be-user-test-"+std::to_string(GetCurrentProcessId()));auto game=root/"game/BoneEater.exe";
    write(game,"fixture");write(root/"game/prop/avs-config.xml",source);write(root/"game/conf/nvram/keep","original");write(root/"game/desktop/bone-eater-controls.xml","controls");
    preparePlayerData(game);require(read(root/"user/conf/nvram/keep")=="original");require(read(root/"user/bone-eater-controls.xml")=="controls");require(read(root/"user/avs-config.xml")==mapped);
    write(root/"user/conf/nvram/keep","saved score");preparePlayerData(game);require(read(root/"user/conf/nvram/keep")=="saved score");require(read(root/"game/conf/nvram/keep")=="original");require(read(root/"game/prop/avs-config.xml")==source);
    write(root/"user/avs-config.xml",source);rejects([&]{preparePlayerData(game);});
    auto fresh=root/"fresh/game/BoneEater.exe";write(fresh,"fixture");write(fresh.parent_path()/"prop/avs-config.xml",source);preparePlayerData(fresh);require(std::filesystem::is_directory(root/"fresh/user/conf/raw/bookkeeping"));require(!std::filesystem::exists(fresh.parent_path()/"conf"));
    std::filesystem::remove_all(root);std::cout<<"Private mounts, fresh install, one-time migration and source preservation passed\n";
}catch(const std::exception& e){std::cerr<<e.what();return 1;}}
