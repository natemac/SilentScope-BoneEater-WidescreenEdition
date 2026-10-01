# Injected into the upstream CMake project through CMAKE_PROJECT_spicetools_INCLUDE.
# Keep the upstream entry implementation; provide our own direct application entry.
enable_language(ASM_MASM)
function(bone_eater_configure_runtime)
    get_filename_component(bone_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)
    set_property(SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/launcher/launcher.cpp"
        APPEND PROPERTY COMPILE_DEFINITIONS "main=bone_eater_unused_upstream_entry")
    target_sources(spicetools_spice64 PRIVATE
        "${bone_root}/src/standalone/main.cpp"
        "${bone_root}/src/input/aim_state.cpp"
        "${bone_root}/src/input/scope_control.cpp"
        "${bone_root}/src/input/scope_button_events.cpp"
        "${bone_root}/src/input/source_policy.cpp"
        "${bone_root}/src/input/selected_hid.cpp"
        "${bone_root}/src/input/selected_hid_config.cpp"
        "${bone_root}/src/input/selected_hid_adapter.cpp"
        "${bone_root}/src/input/selected_hid_runtime.cpp"
        "${bone_root}/src/input/selected_hid_bridge.cpp"
        "${bone_root}/src/input/native_aim.cpp"
        "${bone_root}/src/input/native_wide_input.cpp"
        "${bone_root}/src/input/native_input_ownership.cpp"
        "${bone_root}/src/input/native_precision_bypass.cpp"
        "${bone_root}/src/camera/camera_diagnostics.cpp"
        "${bone_root}/src/camera/native_camera_hook.cpp"
        "${bone_root}/src/camera/native_framing_observer.cpp"
        "${bone_root}/src/camera/vertical_pan_policy.cpp"
        "${bone_root}/src/render/d3d11_diagnostics.cpp"
        "${bone_root}/src/render/capture_alpha.cpp"
        "${bone_root}/src/render/native_viewport.cpp"
        "${bone_root}/src/render/native_display.cpp"
        "${bone_root}/src/render/native_movie_fit.cpp"
        "${bone_root}/src/render/native_window_exit.cpp"
        "${bone_root}/src/render/native_hud.cpp"
        "${bone_root}/src/render/native_boot_layout.cpp"
        "${bone_root}/src/render/native_front_observer.cpp"
        "${bone_root}/src/render/wide_hud_compositor.cpp"
        "${bone_root}/src/render/native_dof_probe.cpp"
        "${bone_root}/src/render/native_dof_experiment.cpp"
        "${bone_root}/src/render/native_dof_stubs.asm"
        "${bone_root}/src/render/native_rear_hud.cpp"
        "${bone_root}/src/render/native_reticle.cpp"
        "${bone_root}/src/render/native_replay_alignment.cpp"
        "${bone_root}/src/render/native_achievement_alignment.cpp"
        "${bone_root}/src/render/native_menu_backing.cpp"
        "${bone_root}/src/render/native_menu_margin.cpp"
        "${bone_root}/src/render/native_normal_start_backing.cpp"
        "${bone_root}/src/render/native_options_backing.cpp"
        "${bone_root}/src/render/native_battle_background.cpp"
        "${bone_root}/src/render/scope_compositor.cpp"
        "${bone_root}/src/render/lobby_impact.cpp"
        "${bone_root}/src/render/title_compositor.cpp"
        "${bone_root}/src/render/auxiliary_windows.cpp"
        "${bone_root}/src/render/window_layout.cpp")
    target_compile_definitions(spicetools_spice64 PRIVATE BONE_EATER_STANDALONE=1)
    target_include_directories(spicetools_spice64 PRIVATE "${bone_root}/src")
    target_link_libraries(spicetools_spice64 PRIVATE bcrypt hid comctl32 windowscodecs ole32 gdi32)
    set_target_properties(spicetools_spice64 PROPERTIES OUTPUT_NAME "BoneEater")
endfunction()
cmake_language(DEFER CALL bone_eater_configure_runtime)
