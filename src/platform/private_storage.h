#pragma once
#include <string_view>
namespace bone_eater {
// ARK remounts this subdirectory after AVS boot, bypassing fs/raw/device.
inline bool isCabinetBookkeepingMount(const char* mount, const char* root, const char* type) {
    return mount && root && type && std::string_view(mount) == "/dev/raw/bookkeeping" &&
        std::string_view(root) == "./CONF/RAW/BOOKKEEPING" && std::string_view(type) == "nvram";
}
void installPrivateBookkeeping(void* arkModule);
}
