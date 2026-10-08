#include "private_storage.h"
#include "avs/core.h"
#include "util/detour.h"
#include "util/logging.h"
#include <stdexcept>
namespace bone_eater {
namespace {
avs::core::AVS_FS_MOUNT_T originalMount = nullptr;
int privateMount(const char* mount, const char* root, const char* type, void* data) {
    log_info("bone-eater", "ARK storage mount: point={} root={} type={}",
        mount ? mount : "<null>", root ? root : "<null>", type ? type : "<null>");
    if (isCabinetBookkeepingMount(mount, root, type)) {
        root = "../user/conf/raw/bookkeeping";
        log_info("bone-eater", "Private bookkeeping mount: {} -> {}", mount, root);
    }
    return originalMount(mount, root, type, data);
}
}
void installPrivateBookkeeping(void* arkModule) {
    // Original-layout and explicit developer AVS overrides retain their own mounts.
    if (avs::core::CFG_PATH != "../user/avs-config.xml" || originalMount) return;
    if (!arkModule || !avs::core::avs_fs_mount) throw std::runtime_error("Cannot establish private bookkeeping mount.");
    originalMount = detour::iat_try_proc("libavs-win64.dll", avs::core::avs_fs_mount,
        &privateMount, static_cast<HMODULE>(arkModule));
    if (!originalMount) throw std::runtime_error("Private bookkeeping mount hook failed; no game started.");
    log_info("bone-eater", "Private bookkeeping mount redirect installed");
}
}
