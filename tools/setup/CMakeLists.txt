# VanillaDroid Setup (phase 22, ADR-0005 §4-§5): the program a player runs next to the APK. vd_setup is its core
# (check the WoW, bake with vd_baker as a child process, the local server for the phone, adb), shared by the console
# tool vdsetup and, later, the window (22.6). Windows now; the code keeps to POSIX elsewhere for macOS.

# qrcodegen's C version (cmake/Dependencies.cmake).
add_library(vd_qrcodegen STATIC "${qrcodegen_SOURCE_DIR}/c/qrcodegen.c")
target_include_directories(vd_qrcodegen SYSTEM PUBLIC "${qrcodegen_SOURCE_DIR}/c")

# The tool's texts, locale/<locale>/setup.lang, built in (EmbedLang.cmake); phase 22 (gui): and the window's,
# setupwindow.lang.
file(GLOB VD_SETUP_LANG_FILES CONFIGURE_DEPENDS "${PROJECT_SOURCE_DIR}/locale/*/setup.lang"
     "${PROJECT_SOURCE_DIR}/locale/*/setupwindow.lang")
string(REPLACE ";" "|" VD_SETUP_LANG_LIST "${VD_SETUP_LANG_FILES}")
set(VD_SETUP_LANG_HEADER "${CMAKE_BINARY_DIR}/generated/vd_setup_lang.gen.h")
add_custom_command(
    OUTPUT "${VD_SETUP_LANG_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${VD_SETUP_LANG_HEADER}" "-DINPUTS=${VD_SETUP_LANG_LIST}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedLang.cmake"
    DEPENDS ${VD_SETUP_LANG_FILES} "${CMAKE_CURRENT_SOURCE_DIR}/EmbedLang.cmake"
    COMMENT "VanillaDroid Setup: embedding setup.lang"
    VERBATIM)

add_library(vd_setup STATIC
    Adb.cpp
    ApkInfo.cpp
    BakeRunner.cpp
    ChildProcess.cpp
    HashCache.cpp
    HttpProtocol.cpp
    HttpServer.cpp
    NetInterfaces.cpp
    PeVersion.cpp
    PhoneServer.cpp
    QrText.cpp
    SetupCommon.cpp
    SetupFlow.cpp # phase 22 (gui): the steps above the core, shared with the window
    SetupSettings.cpp
    SetupText.cpp
    SetupUpdate.cpp # phase 22 (updates): vdsetup update, the Setup replacing itself
    WowCheck.cpp
    "${VD_SETUP_LANG_HEADER}")
target_include_directories(vd_setup PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/generated")
target_link_libraries(vd_setup PUBLIC vd_core vd_formats PRIVATE vd_qrcodegen vd_zlib)
if(WIN32)
    target_link_libraries(vd_setup PUBLIC ws2_32 iphlpapi bcrypt)
endif()
vd_set_target_options(vd_setup)

# The console tool. utf8.manifest: the process's code page is UTF-8 (Windows 10 1903+), so the C runtime and
# std::filesystem read and write paths in any language.
add_executable(vdsetup main.cpp $<$<BOOL:${WIN32}>:${CMAKE_CURRENT_SOURCE_DIR}/utf8.manifest>)
target_link_libraries(vdsetup PRIVATE vd_setup)
if(WIN32)
    target_link_libraries(vdsetup PRIVATE shell32) # CommandLineToArgvW: the arguments in UTF-16
endif()
vd_set_target_options(vdsetup)
# vdsetup runs vd_baker from its own folder (or the development build's): build them together.
add_dependencies(vdsetup vd_baker)

# Phase 22 (gui): the window, VanillaDroid Setup (Windows and macOS: SDL3 is only fetched there).
if(TARGET SDL3::SDL3-static)
    add_subdirectory(gui)
endif()
