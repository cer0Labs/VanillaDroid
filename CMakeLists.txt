cmake_minimum_required(VERSION 3.22)

project(VanillaDroid VERSION 0.1.0 LANGUAGES C CXX)

# URL downloads get extraction-time timestamps (CMake >= 3.24; Android's CMake 3.22 lacks the policy).
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VD_BUILD_TOOLS "Build desktop tools (asset baker)" ON)
option(VD_BUILD_TESTS "Build unit tests" ON)
option(VD_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)
option(VD_ENABLE_TRACY "Build with the Tracy profiler client (on-demand, port 8086)" OFF)
set(VD_WOW_DIR "C:/World of Warcraft 1.12" CACHE PATH
    "Original 1.12.1 client (read-only). Tests that need real data are skipped if it does not exist")
set(VD_WOW_LOCALE_DIRS "C:/World of Warcraft 1.12 esES" CACHE STRING
    "Phase 21: clients of the same build in other languages, ';'-separated (read-only; missing folders are skipped)")
# Phase 22 (distribution, ADR-0005): the release version (the git tag vX.Y.Z; core/Version.h) and the hardened
# distribution build (VD_DIST: no ImGui panels, galleries or VD_* debug switches).
set(VD_VERSION "${PROJECT_VERSION}" CACHE STRING "Phase 22: release version X.Y.Z")
option(VD_DIST "Phase 22: distribution build (no debug panels, galleries or VD_* switches)" OFF)
# Phase 22 (gui): OFF builds the libraries the tools need and the tools alone, without the game's renderer, shaders
# or desktop client (VanillaDroid Setup and the baker on a Mac, where there is no glslc nor a game to run).
option(VD_BUILD_GAME "Build the game (renderer, shaders, desktop client); OFF: the tools alone" ON)

# Tools and tests only make sense on the development PC.
if(ANDROID)
    set(VD_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
    set(VD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
endif()
# Phase 22 (22.7, ADR-0005 §8): the asset baker alone, as a command-line program for Android
# (scripts/android-baker.ps1): no game, no tests. The bake is built without fused multiply-add, as MSVC builds it on
# x64 (bake/Portable.h: the same packs on every platform).
option(VD_ANDROID_BAKER "Phase 22: Android build of the asset baker only" OFF)
if(VD_ANDROID_BAKER)
    if(NOT ANDROID)
        message(FATAL_ERROR "VD_ANDROID_BAKER needs the NDK's toolchain (scripts/android-baker.ps1)")
    endif()
    add_compile_options(-ffp-contract=off)
endif()

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerOptions)
if(VD_DIST) # phase 22 (polish): no path of the building machine in what is published, also in the dependencies
    vd_map_build_paths()
endif()
include(Dependencies)
include(Shaders)

add_subdirectory(engine)

if(ANDROID AND NOT VD_ANDROID_BAKER)
    add_subdirectory(apps/android/native)
elseif(WIN32)
    add_subdirectory(apps/desktop)
endif()

if(VD_BUILD_TOOLS)
    add_subdirectory(tools/formats)
    add_subdirectory(tools/baker)
    add_subdirectory(tools/netcheck)
    # phase 22 (setup): VanillaDroid Setup here
    add_subdirectory(tools/setup)
endif()
if(VD_ANDROID_BAKER) # phase 22 (phonebaker)
    add_subdirectory(tools/formats)
    add_subdirectory(tools/baker)
endif()

if(VD_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
