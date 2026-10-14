# Development executable for Windows. Same engine as Android; never used to judge performance.
if(NOT VD_BUILD_GAME) # phase 22 (gui): the tools alone
    return()
endif()
add_executable(vanilladroid_desktop main_win32.cpp)
# xinput9_1_0: XInputGetState for game controllers (present on every Windows since Vista, no redistributable).
target_link_libraries(vanilladroid_desktop PRIVATE vd_engine winmm xinput9_1_0)
target_compile_definitions(vanilladroid_desktop PRIVATE VD_DEFAULT_PACKS_DIR="${PROJECT_SOURCE_DIR}/packs"
                                                     VD_DEFAULT_CONFIG="${PROJECT_SOURCE_DIR}/config/client.cfg")
vd_set_target_options(vanilladroid_desktop)
