#include "silentscope.h"

#include "cfg/configurator.h"
#include "hooks/devicehook.h"
#include "util/libutils.h"

#include "projector.h"

#ifdef BONE_EATER_STANDALONE
#include "camera/native_camera_hook.h"
#include "input/native_aim.h"
#include "input/selected_hid_bridge.h"
#include "util/logging.h"
#include "render/native_display.h"
#include "render/native_movie_fit.h"
#include "render/native_hud.h"
#include "render/native_rear_hud.h"
#include "render/native_reticle.h"
#include "render/scope_compositor.h"
#endif

namespace games::silentscope {

    SilentScopeGame::SilentScopeGame() : Game("Silent Scope") {
    }

    void SilentScopeGame::attach() {
        Game::attach();

        // load the game DLL so hooks apply
        libutils::try_library("gamendd.dll");
#ifdef BONE_EATER_STANDALONE
        // No config means Legacy. A failed selected open remains selected and
        // disarmed; it cannot silently switch to cursor/API gun input.
        const auto selected_start = bone_eater::input::startSelectedHidBridge();
        if (selected_start != bone_eater::input::SelectedHidRuntimeCommand::Legacy) {
            log_info("selected-input", "selected HID start result {}", static_cast<int>(selected_start));
        }
        bone_eater::render::installNativeDisplayExperiment(GetModuleHandleW(L"gamendd.dll"));
        bone_eater::render::installNativeMovieFit(GetModuleHandleW(L"gamendd.dll"));
        bone_eater::input::installNativeAimObserver(
            GetModuleHandleW(L"gamendd.dll"), GetModuleHandleW(L"arkndd.dll"));
        bone_eater::camera::installNativeCameraObserver(GetModuleHandleW(L"gamendd.dll"));
        bone_eater::render::installNativeHudFit(GetModuleHandleW(L"gamendd.dll"));
        bone_eater::render::installNativeRearHudFit(GetModuleHandleW(L"gamendd.dll"));
        bone_eater::render::installNativeReticleSuppression(
            GetModuleHandleW(L"gamendd.dll"), &bone_eater::render::desktopReticleReady);
#endif

        devicehook_init();
        devicehook_add(new ProjectorHandle());
    }

    void SilentScopeGame::detach() {
#ifdef BONE_EATER_STANDALONE
        bone_eater::input::stopSelectedHidBridge();
#endif
        Game::detach();
    }
}
