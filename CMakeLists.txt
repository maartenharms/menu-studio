cmake_minimum_required(VERSION 3.21)

# "Menu Studio" - world stays paused in menus, the player's animation
# graph + HDT/SMP physics keep ticking. Skyrim SE 1.5.97 only.
# (Internal name renamed from MenuTimeBubble -> MenuStudio; the C++ MTB::
# namespace is kept.)
project(MenuStudio VERSION 0.4.0 LANGUAGES CXX)

find_package(CommonLibSSE CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)

add_commonlibsse_plugin(${PROJECT_NAME}
    SOURCES
        src/PCH.h
        src/plugin.cpp
        src/Offsets.h
        src/Backdrop.h
        src/Backdrop.cpp
        src/FaceNeutral.h
        src/FaceNeutral.cpp
        src/ForcePause.h
        src/ForcePause.cpp
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
        src/OwnView.cpp)

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
