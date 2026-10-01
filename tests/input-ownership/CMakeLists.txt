cmake_minimum_required(VERSION 3.20)
project(BoneEaterInputOwnershipTests LANGUAGES CXX)
enable_testing()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME precision_pinned_guards COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/test_precision_guards.py")
add_executable(input_ownership_state_tests input_ownership_state_tests.cpp)
target_include_directories(input_ownership_state_tests PRIVATE ../../src)
target_compile_features(input_ownership_state_tests PRIVATE cxx_std_20)
add_test(NAME input_ownership_state_tests COMMAND input_ownership_state_tests)
if(MSVC)
    add_executable(native_precision_tests native_precision_tests.cpp precision_ownership_driver.cpp ../../src/input/scope_control.cpp)
    target_include_directories(native_precision_tests BEFORE PRIVATE ../render/reticle_stubs ../../src)
    target_compile_features(native_precision_tests PRIVATE cxx_std_20)
    target_compile_options(native_precision_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    add_test(NAME native_precision_tests COMMAND native_precision_tests)
    add_test(NAME native_precision_quiet COMMAND native_precision_tests --quiet)
    add_test(NAME native_precision_disabled COMMAND native_precision_tests --disabled)
    add_executable(native_input_ownership_tests native_input_ownership_tests.cpp ../../src/input/scope_control.cpp)
    target_include_directories(native_input_ownership_tests BEFORE PRIVATE ../render/reticle_stubs ../../src)
    target_compile_features(native_input_ownership_tests PRIVATE cxx_std_20)
    target_compile_options(native_input_ownership_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    add_test(NAME native_input_ownership_tests COMMAND native_input_ownership_tests)
    add_test(NAME native_input_ownership_quiet COMMAND native_input_ownership_tests --quiet)
    add_executable(native_wide_ownership_tests native_wide_ownership_tests.cpp
        ../../src/input/scope_control.cpp ../../src/input/scope_button_events.cpp ../../src/input/aim_state.cpp)
    target_include_directories(native_wide_ownership_tests BEFORE PRIVATE ../render/reticle_stubs ../../src)
    target_compile_features(native_wide_ownership_tests PRIVATE cxx_std_20)
    target_compile_options(native_wide_ownership_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    add_test(NAME native_wide_ownership_tests COMMAND native_wide_ownership_tests)
    add_executable(ownership_cli_tests ownership_cli_tests.cpp
        ../../src/input/selected_hid_config.cpp ../../src/input/source_policy.cpp)
    target_include_directories(ownership_cli_tests PRIVATE ../../src)
    target_include_directories(ownership_cli_tests SYSTEM PRIVATE ../../vendor/spice2x/src/spice2x)
    target_compile_features(ownership_cli_tests PRIVATE cxx_std_20)
    target_compile_options(ownership_cli_tests PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(ownership_cli_tests PRIVATE NOMINMAX= WIN32_LEAN_AND_MEAN=)
    target_link_libraries(ownership_cli_tests PRIVATE shell32)
    foreach(mode desktop missing wide quiet precision-full precision-missing precision-wide precision-quiet precision-inherited)
        add_test(NAME ownership_cli_${mode} COMMAND ownership_cli_tests ${mode})
        set_tests_properties(ownership_cli_${mode} PROPERTIES ENVIRONMENT
            "BONE_EATER_BATTLE_BACKGROUND_ALIGN=1;BONE_EATER_BATTLE_BACKGROUND_FILTER=1")
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/precision-selected.json" [=[
{"schema_version":1,"mode":"selected_hid","device":{"vid":"1209","pid":"0001","interface_path":"\\\\?\\hid#fixture","contract":"xgunner_report3_v1"},"coordinate_space":"calibrated_game_content","buttons":{"trigger_usage":4,"scope_right_usage":16,"scope_left_usage":null},"max_report_age_ms":1234}
]=])
    add_test(NAME precision_cli_selected COMMAND ownership_cli_tests --native-precision-bypass
        -2Display --native-wide --native-hud-fit --native-input-desktop --diagnose
        --input-config "${CMAKE_CURRENT_BINARY_DIR}/precision-selected.json")
    # Compile the real integration units with vendor headers, without linking
    # or starting a game and without using the shared runtime build directory.
    add_library(ownership_production_compile OBJECT
        ../../src/input/native_input_ownership.cpp ../../src/input/native_wide_input.cpp
        ../../src/input/native_precision_bypass.cpp
        ../../src/input/native_aim.cpp ../../src/standalone/main.cpp)
    target_include_directories(ownership_production_compile PRIVATE ../../vendor/spice2x/src/spice2x ../../src)
    target_compile_features(ownership_production_compile PRIVATE cxx_std_20)
    target_compile_options(ownership_production_compile PRIVATE /W4 /permissive- /EHsc /utf-8)
    target_compile_definitions(ownership_production_compile PRIVATE NOMINMAX= WIN32_LEAN_AND_MEAN=)
endif()
