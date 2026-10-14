# Native library loaded by GameActivity (android.app.lib_name = "vanilladroid").
find_package(game-activity REQUIRED CONFIG)
# Paddleboat (androidx.games:games-controller): game controllers.
find_package(games-controller REQUIRED CONFIG)

add_library(vanilladroid SHARED main_android.cpp
    AppPackInstall.cpp) # phase 22 (apppack): app.vdpk from the APK's assets
target_link_libraries(vanilladroid PRIVATE
    vd_engine
    game-activity::game-activity_static
    games-controller::paddleboat_static
    android
    log)
vd_set_target_options(vanilladroid)

target_link_options(vanilladroid PRIVATE
    # GameActivity's Java side calls this JNI entry point; without -u the linker drops it from the
    # static library because nothing native references it.
    "-Wl,-u,Java_com_google_androidgamesdk_GameActivity_initializeNativeCode"
    # Same for the callbacks of Paddleboat's GameControllerManager (Java) into its native side.
    "-Wl,-u,Java_com_google_android_games_paddleboat_GameControllerManager_onControllerConnected"
    "-Wl,-u,Java_com_google_android_games_paddleboat_GameControllerManager_onControllerDisconnected"
    "-Wl,-u,Java_com_google_android_games_paddleboat_GameControllerManager_onKeyboardConnected"
    "-Wl,-u,Java_com_google_android_games_paddleboat_GameControllerManager_onKeyboardDisconnected"
    "-Wl,-u,Java_com_google_android_games_paddleboat_GameControllerManager_onMotionData"
    "-Wl,-u,Java_com_google_android_games_paddleboat_GameControllerManager_onMouseConnected"
    "-Wl,-u,Java_com_google_android_games_paddleboat_GameControllerManager_onMouseDisconnected"
    "-Wl,--gc-sections"
    # 16 KB page size compatibility (Android 15+).
    "-Wl,-z,max-page-size=16384")
