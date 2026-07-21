cmake_minimum_required(VERSION 3.21)

# "Menu Studio" - world stays paused in menus, the player's animation
# graph + HDT/SMP physics keep ticking. Skyrim SE 1.5.97 only.
# (Internal name renamed from MenuTimeBubble -> MenuStudio; the C++ MTB::
# namespace is kept.)
project(MenuStudio VERSION 0.7.2 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/PCH.h
        src/plugin.cpp
        src/Offsets.h
        src/EquipSoundMute.h
        src/VersionCheck.h
        src/VersionCheck.cpp
        src/Backdrop.h
        src/Backdrop.cpp
        src/BackdropManifest.h
        src/BackdropManifest.cpp
        src/BackdropPacks.h
        src/BackdropPacks.cpp
        src/FaceNeutral.h
        src/FaceNeutral.cpp
        src/AnimEventProbe.h
        src/AnimEventProbe.cpp
        src/ClipProbe.h
        src/ClipProbe.cpp
        src/ActorTickProbe.h
        src/ActorTickProbe.cpp
        src/EquipNotifyGate.h
        src/EquipNotifyGate.cpp
        src/ForcePause.h
        src/ForcePause.cpp
        src/ShadowPause.h
        src/ShadowPause.cpp
        src/Settings.h
        src/Settings.cpp
        src/SettingsUI.h
        src/SettingsUI.cpp
        src/SceneTint.h
        src/SceneTint.cpp
        src/StudioLight.h
        src/StudioLight.cpp
        src/StudioRig.h
        src/StudioRig.cpp
        src/Transition.h
        src/Transition.cpp
        src/Bubble.h
        src/Bubble.cpp
        src/CameraGate.h
        src/CameraGate.cpp
        src/CbpcDrive.h
        src/CbpcDrive.cpp
        src/FootIkGate.h
        src/FootIkGate.cpp
        src/Declutter.h
        src/Declutter.cpp
        src/FsmpDrive.h
        src/FsmpDrive.cpp
        src/MenuInputGate.h
        src/MenuInputGate.cpp
        src/OwnView.h
        src/OwnView.cpp
        src/WeaponPreviewGate.h
        src/WeaponPreview.h
        src/WeaponPreview.cpp)

target_compile_features(${PROJECT_NAME} PRIVATE cxx_std_23)
target_precompile_headers(${PROJECT_NAME} PRIVATE src/PCH.h)
target_include_directories(${PROJECT_NAME} PRIVATE extern)

# FLICK / FUCK (Fuzz's Legally Intelligible Core Kit) is consumed at runtime via
# its C-ABI (extern/FUCK_API.h + FUCK.dll's RequestFUCK export); we only need
# imgui's headers/types for the API surface - FUCK.dll does the rendering.
target_link_libraries(${PROJECT_NAME} PRIVATE imgui::imgui)
target_compile_definitions(${PROJECT_NAME} PRIVATE IMGUI_DEFINE_MATH_OPERATORS)

if(NOT DEFINED OUTPUT_FOLDER)
    set(OUTPUT_FOLDER "C:/Games/Nolvus/Instances/Nolvus Awakening/MODS/mods/Menu Studio")
endif()
set(DLL_FOLDER "${OUTPUT_FOLDER}/SKSE/Plugins")
message(STATUS "MenuStudio deploy folder: ${DLL_FOLDER}")
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory "${DLL_FOLDER}"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:${PROJECT_NAME}>" "${DLL_FOLDER}/$<TARGET_FILE_NAME:${PROJECT_NAME}>"
    COMMAND "${CMAKE_COMMAND}" -E copy_directory "${CMAKE_CURRENT_SOURCE_DIR}/dist" "${OUTPUT_FOLDER}"
    VERBATIM)

# Optional standalone tests for the engine-free pieces - the backdrop-manifest
# parser and the weapon-preview decision gate. Configure with
# -DMENUSTUDIO_TESTS=ON to build them; they are never part of the plugin.
# NOTE: vcpkg's simpleini port is header-only and ships no CMake config package
# (no unofficial::simpleini::simpleini target to find_package() against) - confirmed
# by inspecting its installed file list, which contains only headers + copyright/spdx
# metadata under share/simpleini. So the backdrop test adds its vcpkg include dir
# directly instead.
option(MENUSTUDIO_TESTS "Build the standalone engine-free tests" OFF)
if(MENUSTUDIO_TESTS)
    add_executable(backdrop_manifest_test
        tests/backdrop_manifest_test.cpp
        src/BackdropManifest.cpp)
    target_include_directories(backdrop_manifest_test PRIVATE src)
    target_include_directories(backdrop_manifest_test PRIVATE "${CMAKE_BINARY_DIR}/vcpkg_installed/x64-windows-static-md/include")
    target_compile_features(backdrop_manifest_test PRIVATE cxx_std_23)

    add_executable(weapon_preview_gate_test
        tests/weapon_preview_gate_test.cpp)
    target_include_directories(weapon_preview_gate_test PRIVATE src)
    target_compile_features(weapon_preview_gate_test PRIVATE cxx_std_23)

    add_executable(equip_sound_mute_test
        tests/equip_sound_mute_test.cpp)
    target_include_directories(equip_sound_mute_test PRIVATE src)
    target_compile_features(equip_sound_mute_test PRIVATE cxx_std_23)
endif()
