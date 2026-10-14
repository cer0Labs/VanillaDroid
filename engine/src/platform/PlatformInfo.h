#pragma once

#include <string>

namespace vd {

// OS handles the engine needs beyond the window. Opaque so that engine headers stay free of
// <jni.h>/<windows.h>.
struct PlatformInfo {
    void* javaVM = nullptr;   // Android: JavaVM*
    void* jniEnv = nullptr;   // Android: JNIEnv* attached to the engine thread
    void* activity = nullptr; // Android: jobject of the GameActivity
    float contentScale = 1.0f; // physical pixels per density-independent pixel (Android dpi / 160)
    float uiScale = 1.0f;      // the touch UI's size on top of the density (desktop: to look like the phone)
    // Phase 20 (edit, docs/MOBILE_CONTROLS.md §3.4): the screen's real pixels per millimetre (Android: the display's
    // xdpi / ydpi; densityDpi is a bucket). 0: unknown, millimetres come from contentScale (desktop).
    float pixelsPerMm = 0.0f;
    // Directory holding the baked packs (*.vdpk).
    //   Android: <getExternalFilesDir()>/packs  (filled with scripts/push-data.ps1)
    //   Desktop: --packs <dir>, default <repo>/packs
    std::string packsDir;
    // Phase 22 (apppack): app.vdpk, ours (texts, art, font: pack/AppPack.h). Android: copied from the APK's assets into
    // the internal storage; empty: <packsDir>/app.vdpk (desktop).
    std::string appPack;
    // Writable directory for caches (pipeline cache...). Android: internal app storage; desktop: packs/.cache.
    std::string cacheDir;
    // Phase 22 (pairing): the system's language ("es", "es-ES"), for the first-run screen while no client.cfg says
    // one. Empty: unknown.
    std::string systemLanguage;
    // Benchmark route to fly at startup (app/Benchmark.h ids), from `--bench` or the launch intent.
    std::string benchmark;
    bool exitAfterBenchmark = false;
    // client.cfg (config/client.example.cfg). Desktop: <repo>/config/client.cfg; Android: next to packs/.
    std::string configPath;
    // Server session (phase 4), overriding client.cfg: from the command line or the launch intent. The
    // password never comes from disk; without it there is no automatic login.
    std::string realmd;    // "host:port"
    std::string account;
    std::string password;
    std::string character; // logged in automatically once the character list arrives
    // Development: actors placed in front of the camera once the ground is known, comma-separated:
    // "crowd:40", "display:4", "character:7" (random look from that seed).
    std::string spawn;
    bool hideDebugUi = false; // development captures: only the frame statistics, no panels
    bool walk = false;        // development: a character of its own, offline, on the ground under the camera
    // Development: chat lines said in the world, ';'-separated, one every few seconds (GM commands to test
    // teleports and speed changes: ".tele goldshire;.tele orgrimmar").
    std::string commands;
    // Development: a walking route (WalkTest) done by the server's character once the commands are said.
    std::string walkRoute;
    bool traceMovement = false; // development: every movement packet sent, in the log (anticheat checks)
    // Development (desktop): while a text field is edited, the bottom `fakeKeyboard` of the window counts as covered
    // by a soft keyboard, to see the touch UI make room for it as on the phone. 0 = off.
    float fakeKeyboard = 0.0f;
    // Phase 9: where the sound goes: "off", "null", "device" or "wav:<file>" (audio/AudioDevice.h). Empty: the
    // platform's default (desktop: off, so the development PC stays silent; Android: device).
    std::string audio;
    // Phase 10 (benchmarks, development): a quality preset ("low", "medium", "high", "ultra") instead of the saved
    // settings, a fixed world resolution (0 = the settings and the dynamic quality decide), the dynamic quality "on"
    // or "off", and how long a looping benchmark route lasts (minutes; 0 = its default).
    std::string quality;
    float renderScale = 0.0f;
    std::string dynamicQuality;
    float benchMinutes = 0.0f;
    // Phase 11 (benchmarks, development): the weather shown instead of the server's, as VD_WEATHER
    // ("rain:0.8", "snow:1:instant"...), and the ghost's world offline (as VD_GHOST=1).
    std::string weather;
    bool ghost = false;
};

} // namespace vd
