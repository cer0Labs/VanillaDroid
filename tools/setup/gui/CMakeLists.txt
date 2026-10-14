# Phase 22 (gui, ADR-0005 §4): VanillaDroid Setup's window. Dear ImGui over SDL3 and its 2D renderer (Direct3D 11 on
# Windows, Metal on macOS), on the same core as the console tool (vd_setup). Windows: VanillaDroidSetup.exe, a
# windowed program with the app's icon. macOS: "VanillaDroid Setup.app", with vd_baker and vdsetup inside
# Contents/MacOS (the window runs the baker from its own folder, as vdsetup does).

# Dear ImGui's core and its SDL3 backends (the game's vd_imgui carries the Vulkan backend: not this one).
add_library(vd_setup_imgui STATIC
    "${imgui_SOURCE_DIR}/imgui.cpp"
    "${imgui_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_SOURCE_DIR}/imgui_widgets.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdl3.cpp"
    "${imgui_SOURCE_DIR}/backends/imgui_impl_sdlrenderer3.cpp")
target_include_directories(vd_setup_imgui SYSTEM PUBLIC "${imgui_SOURCE_DIR}" "${imgui_SOURCE_DIR}/backends")
target_compile_definitions(vd_setup_imgui PUBLIC IMGUI_DISABLE_OBSOLETE_FUNCTIONS IMGUI_DISABLE_DEMO_WINDOWS
                                                 IMGUI_DISABLE_DEBUG_TOOLS)
target_link_libraries(vd_setup_imgui PUBLIC SDL3::SDL3-static)

# The font (art/fonts/DroidSans.ttf, Apache 2.0) and the logo, built into the program (EmbedFiles.cmake).
set(VD_SETUP_FONT "${PROJECT_SOURCE_DIR}/art/fonts/DroidSans.ttf")
set(VD_SETUP_LOGO "${PROJECT_SOURCE_DIR}/art/app-icon/VanillaDroid-transparent-512.png")
set(VD_SETUP_GUI_ASSETS "${CMAKE_BINARY_DIR}/generated/vd_setup_gui_assets.gen.h")
add_custom_command(
    OUTPUT "${VD_SETUP_GUI_ASSETS}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${VD_SETUP_GUI_ASSETS}"
            "-DINPUTS=kDroidSans=${VD_SETUP_FONT}|kLogoPng=${VD_SETUP_LOGO}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/EmbedFiles.cmake"
    DEPENDS "${VD_SETUP_FONT}" "${VD_SETUP_LOGO}" "${CMAKE_CURRENT_SOURCE_DIR}/EmbedFiles.cmake"
    COMMENT "VanillaDroid Setup: embedding the font and the logo"
    VERBATIM)

add_executable(VanillaDroidSetup WIN32 MACOSX_BUNDLE
    main.cpp
    GuiKit.cpp
    GuiTask.cpp
    PagesAdb.cpp
    PagesBake.cpp
    PagesHome.cpp
    PagesPhone.cpp
    PagesWow.cpp
    SetupWindow.cpp
    "${VD_SETUP_GUI_ASSETS}")
target_include_directories(VanillaDroidSetup PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(VanillaDroidSetup PRIVATE vd_setup vd_setup_imgui)
vd_set_target_options(VanillaDroidSetup)
# Next to vdsetup (build/<preset>/tools/setup/<config>/): findBaker() finds the development build's vd_baker from
# there; the window runs it, so they are built together.
set_target_properties(VanillaDroidSetup PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/tools/setup")
add_dependencies(VanillaDroidSetup vd_baker vdsetup)

# The version for the program's resources and the bundle (VD_VERSION, X.Y.Z).
string(REGEX MATCH "^([0-9]+)\\.([0-9]+)\\.([0-9]+)$" vd_setup_version "${VD_VERSION}")
set(VD_SETUP_VERSION_MAJOR "${CMAKE_MATCH_1}")
set(VD_SETUP_VERSION_MINOR "${CMAKE_MATCH_2}")
set(VD_SETUP_VERSION_PATCH "${CMAKE_MATCH_3}")

if(WIN32)
    # The icon (art/app-icon/VanillaDroid.ico, from VanillaDroid-transparent.png) and the version resource; the
    # manifests: UTF-8 code page (paths in any language) and per-monitor DPI awareness.
    set(VD_SETUP_ICON "${PROJECT_SOURCE_DIR}/art/app-icon/VanillaDroid.ico")
    configure_file(VanillaDroidSetup.rc.in "${CMAKE_CURRENT_BINARY_DIR}/VanillaDroidSetup.rc" @ONLY)
    target_sources(VanillaDroidSetup PRIVATE
        "${CMAKE_CURRENT_BINARY_DIR}/VanillaDroidSetup.rc"
        "${PROJECT_SOURCE_DIR}/tools/setup/utf8.manifest"
        window.manifest)
    set_property(SOURCE "${CMAKE_CURRENT_BINARY_DIR}/VanillaDroidSetup.rc" APPEND PROPERTY OBJECT_DEPENDS
                 "${VD_SETUP_ICON}")
endif()

if(APPLE)
    # "VanillaDroid Setup.app": its Info.plist, the icon made here from the PNG with the system's sips and iconutil
    # (no .icns in the repository), the licenses in Resources, and vd_baker and vdsetup next to the window's program.
    set(VD_SETUP_ICON_SOURCE "${PROJECT_SOURCE_DIR}/art/app-icon/VanillaDroid-transparent.png")
    set(VD_SETUP_ICONSET "${CMAKE_CURRENT_BINARY_DIR}/VanillaDroid.iconset")
    set(VD_SETUP_ICNS "${CMAKE_CURRENT_BINARY_DIR}/VanillaDroid.icns")
    set(vd_icon_commands "")
    foreach(vd_size 16 32 128 256 512)
        math(EXPR vd_double "${vd_size} * 2")
        list(APPEND vd_icon_commands
             COMMAND sips -z ${vd_size} ${vd_size} "${VD_SETUP_ICON_SOURCE}" --out
                     "${VD_SETUP_ICONSET}/icon_${vd_size}x${vd_size}.png"
             COMMAND sips -z ${vd_double} ${vd_double} "${VD_SETUP_ICON_SOURCE}" --out
                     "${VD_SETUP_ICONSET}/icon_${vd_size}x${vd_size}@2x.png")
    endforeach()
    add_custom_command(
        OUTPUT "${VD_SETUP_ICNS}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${VD_SETUP_ICONSET}"
        ${vd_icon_commands}
        COMMAND iconutil -c icns -o "${VD_SETUP_ICNS}" "${VD_SETUP_ICONSET}"
        DEPENDS "${VD_SETUP_ICON_SOURCE}"
        COMMENT "VanillaDroid Setup: the macOS icon"
        VERBATIM)
    # The licence and the one notices file of everything published (also the release's).
    set(VD_SETUP_NOTICES "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${PROJECT_SOURCE_DIR}/LICENSE")
    target_sources(VanillaDroidSetup PRIVATE "${VD_SETUP_ICNS}" ${VD_SETUP_NOTICES})
    set_source_files_properties("${VD_SETUP_ICNS}" ${VD_SETUP_NOTICES} PROPERTIES MACOSX_PACKAGE_LOCATION Resources)
    set_target_properties(VanillaDroidSetup PROPERTIES
        OUTPUT_NAME "VanillaDroid Setup"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/macos/Info.plist.in"
        MACOSX_BUNDLE_BUNDLE_NAME "VanillaDroid Setup"
        MACOSX_BUNDLE_GUI_IDENTIFIER "com.cer0labs.vanilladroid.setup"
        MACOSX_BUNDLE_SHORT_VERSION_STRING "${VD_VERSION}"
        MACOSX_BUNDLE_BUNDLE_VERSION "${VD_VERSION}"
        MACOSX_BUNDLE_ICON_FILE "VanillaDroid.icns")
    add_custom_command(TARGET VanillaDroidSetup POST_BUILD
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "$<TARGET_FILE:vd_baker>" "$<TARGET_FILE:vdsetup>"
                "$<TARGET_BUNDLE_CONTENT_DIR:VanillaDroidSetup>/MacOS/"
        VERBATIM)
endif()

# What a release ships (the CI zips it, adding the APK): cmake --install <build> --config Release --component setup
# --prefix <folder>. The programs and the licenses; never a PDB (install(TARGETS) does not copy them).
if(APPLE)
    install(TARGETS VanillaDroidSetup BUNDLE DESTINATION . COMPONENT setup) # vd_baker, vdsetup and the licenses inside
else()
    install(TARGETS VanillaDroidSetup vdsetup vd_baker RUNTIME DESTINATION . COMPONENT setup)
    install(FILES "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${PROJECT_SOURCE_DIR}/LICENSE" DESTINATION .
            COMPONENT setup)
endif()
