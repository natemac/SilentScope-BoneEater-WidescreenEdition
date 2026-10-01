// included early to avoid warning
#include <winsock2.h>

#include "shutdown.h"

#include "api/controller.h"
#include "easrv/easrv.h"
#include "rawinput/rawinput.h"
#include "hooks/audio/audio.h"
#include "hooks/graphics/graphics.h"
#include "hooks/graphics/backends/d3d11/d3d11_backend.h"
#include "util/deferlog.h"
#include "util/logging.h"

#include "launcher.h"
#include "logger.h"
#include "nvapi/nvapi.h"
#include "sdk/sdk.h"
#ifdef BONE_EATER_STANDALONE
#include "input/selected_hid_bridge.h"
#endif

namespace launcher {

    void stop_subsystems() {
        // note that it is possible for stop_subsystems to be called multiple times
        // (e.g., crashing, and then closing the window)
        // therefore, subsystems need to be guarded against multiple unload attempts
        log_info("launcher", "stopping subsystems");

#ifdef BONE_EATER_STANDALONE
        // Shutdown can use TerminateProcess, bypassing atexit. Disarm first and
        // request cancellation without delaying the forced-exit path.
        bone_eater::input::stopSelectedHidBridge(std::chrono::milliseconds(0));
#endif
        sdk::fini_sdk_modules();

        // stop dx11 background workers (poll thread, LDR notification)
        // before anything else, so they can't race against the teardown.
        graphics_d3d11_shutdown();

        // reset monitor settings
        reset_monitor_on_exit();

        // before shutting down logger, dump any deferred log messages
        deferredlogs::dump_to_logger();

        // flush/stop logger
        logger::stop();

        // stop ea server
        easrv_shutdown();

        // free api sockets
        if (API_CONTROLLER) {
            API_CONTROLLER->free_socket();
        }

        // notify audio hook
        hooks::audio::stop();

        // stop raw input
        if (RI_MGR) {
            RI_MGR->stop();
        }

        // unload nvapi and free library (if loaded)
        nvapi::unload();
    }

    void kill(UINT exit_code) {

        // terminate
        TerminateProcess(GetCurrentProcess(), exit_code);
    }

    void shutdown(UINT exit_code) {

        // force exit after 1s
        std::thread force_thread([exit_code] {
            Sleep(1000);
            log_info("launcher", "force shutdown");
            kill(exit_code);
            return nullptr;
        });
        force_thread.detach();

        // stop all subsystems
        stop_subsystems();

        // terminate
        kill(exit_code);
    }

#ifndef BONE_EATER_STANDALONE
    static void restart_spawn() {

        // never do this twice
        static bool already_done = false;
        if (already_done) {
            return;
        } else {
            already_done = true;
        }

        // start the process using the same args
        if (LAUNCHER_ARGC > 0) {

            // build cmd line
            std::string cmd_line = "START \"\" ";
            for (int i = 0; i < LAUNCHER_ARGC; i++)
                cmd_line += " \"" + std::string(LAUNCHER_ARGV[i]) + "\"";

            // run command
            system(cmd_line.c_str());
        }
    }

#endif

    void restart() {
#ifdef BONE_EATER_STANDALONE
        // Upstream reconstructs only LAUNCHER_ARGV, losing consumed standalone
        // flags. Spawning the full command before teardown also races the old
        // process's API/EASRV ports. Until a parent-exit handoff is implemented,
        // retain the current process/configuration instead of partially restarting.
        log_warning("launcher", "standalone restart is unavailable; exit and relaunch with the same settings");
        return;
#else

        // force restart after 1s
        std::thread force_thread([] {
            Sleep(1000);
            log_info("launcher", "force restart");
            restart_spawn();
            launcher::kill(0);
            return nullptr;
        });
        force_thread.detach();

        // clean up before restart so resources can be reclaimed
        stop_subsystems();

        // spawn new and terminate this process
        restart_spawn();
        launcher::kill(0);
#endif
    }
}
