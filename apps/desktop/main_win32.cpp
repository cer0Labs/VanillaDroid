#include "app/Game.h"
#include "core/Log.h"

#include <windows.h>
#include <timeapi.h>
#include <windowsx.h>
#include <Xinput.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <optional>
#include <string>

namespace {

// XInput: the first connected controller, polled once per frame while the window has the focus. Empty slots are
// probed only every 2 s: XInputGetState on a slot without a pad is known to be slow.
struct GamepadPoller {
    DWORD slot = XUSER_MAX_COUNT; // XUSER_MAX_COUNT: none
    ULONGLONG nextProbeMs = 0;
};

struct DesktopState {
    vd::Game game;
    bool running = true;
    bool minimized = false;
    bool looking = false; // right mouse button held: mouse look
    POINT lastLook{};
    wchar_t highSurrogate = 0; // WM_CHAR delivers characters above U+FFFF as two UTF-16 halves
    GamepadPoller gamepad;
};

float thumb(SHORT value) {
    return std::clamp(static_cast<float>(value) / 32767.0f, -1.0f, 1.0f);
}

vd::GamepadState pollGamepad(GamepadPoller& poller) {
    XINPUT_STATE xinput{};
    if (poller.slot < XUSER_MAX_COUNT && XInputGetState(poller.slot, &xinput) != ERROR_SUCCESS) {
        poller.slot = XUSER_MAX_COUNT;
    }
    if (poller.slot >= XUSER_MAX_COUNT) {
        const ULONGLONG now = GetTickCount64();
        if (now < poller.nextProbeMs) {
            return {};
        }
        poller.nextProbeMs = now + 2000;
        for (DWORD i = 0; i < XUSER_MAX_COUNT && poller.slot >= XUSER_MAX_COUNT; ++i) {
            if (XInputGetState(i, &xinput) == ERROR_SUCCESS) {
                poller.slot = i;
            }
        }
        if (poller.slot >= XUSER_MAX_COUNT) {
            return {};
        }
    }
    const XINPUT_GAMEPAD& pad = xinput.Gamepad;
    vd::GamepadState state;
    state.connected = true;
    state.leftX = thumb(pad.sThumbLX); // XInput: y up, like GamepadState
    state.leftY = thumb(pad.sThumbLY);
    state.rightX = thumb(pad.sThumbRX);
    state.rightY = thumb(pad.sThumbRY);
    state.leftTrigger = static_cast<float>(pad.bLeftTrigger) / 255.0f;
    state.rightTrigger = static_cast<float>(pad.bRightTrigger) / 255.0f;
    static constexpr struct {
        WORD xinput;
        vd::u32 button;
    } kButtons[] = {{XINPUT_GAMEPAD_A, vd::gamepad::kA},
                    {XINPUT_GAMEPAD_B, vd::gamepad::kB},
                    {XINPUT_GAMEPAD_X, vd::gamepad::kX},
                    {XINPUT_GAMEPAD_Y, vd::gamepad::kY},
                    {XINPUT_GAMEPAD_LEFT_SHOULDER, vd::gamepad::kLB},
                    {XINPUT_GAMEPAD_RIGHT_SHOULDER, vd::gamepad::kRB},
                    {XINPUT_GAMEPAD_BACK, vd::gamepad::kBack},
                    {XINPUT_GAMEPAD_START, vd::gamepad::kStart},
                    {XINPUT_GAMEPAD_DPAD_UP, vd::gamepad::kDpadUp},
                    {XINPUT_GAMEPAD_DPAD_DOWN, vd::gamepad::kDpadDown},
                    {XINPUT_GAMEPAD_DPAD_LEFT, vd::gamepad::kDpadLeft},
                    {XINPUT_GAMEPAD_DPAD_RIGHT, vd::gamepad::kDpadRight},
                    {XINPUT_GAMEPAD_LEFT_THUMB, vd::gamepad::kL3},
                    {XINPUT_GAMEPAD_RIGHT_THUMB, vd::gamepad::kR3}};
    for (const auto& b : kButtons) {
        if (pad.wButtons & b.xinput) {
            state.buttons |= b.button;
        }
    }
    return state;
}

bool mapKey(WPARAM key, vd::Key& out) {
    switch (key) {
    case 'W': out = vd::Key::Forward; return true;
    case 'S': out = vd::Key::Back; return true;
    case 'A': out = vd::Key::Left; return true;
    case 'D': out = vd::Key::Right; return true;
    case 'Q': out = vd::Key::Down; return true;
    case 'E': out = vd::Key::Up; return true;
    case VK_SHIFT: out = vd::Key::Fast; return true;
    case VK_SPACE: out = vd::Key::Jump; return true;
    case VK_DIVIDE:
    case VK_OEM_2: out = vd::Key::Walk; return true;
    case 'F': out = vd::Key::Mode; return true;
    default: return false;
    }
}

// Half of each Galaxy Z Fold5 screen in landscape, to test both UI layouts on the PC, and half of a usual
// 20:9 phone (the Xiaomi Mi 11X).
struct ScreenPreset {
    const char* name;
    LONG width;
    LONG height;
};
constexpr ScreenPreset kInnerScreen{"inner", 2176 / 2, 1812 / 2};
constexpr ScreenPreset kCoverScreen{"cover", 2316 / 2, 904 / 2};
constexpr ScreenPreset kPhoneScreen{"phone", 2400 / 2, 1080 / 2};

ScreenPreset parseScreenPreset(int argc, char** argv) {
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--screen") == 0) {
            for (const ScreenPreset& preset : {kCoverScreen, kPhoneScreen}) {
                if (std::strcmp(argv[i + 1], preset.name) == 0) {
                    return preset;
                }
            }
        }
    }
    return kInnerScreen;
}

LRESULT CALLBACK windowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
    }
    auto* state = reinterpret_cast<DesktopState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    switch (msg) {
    case WM_SIZE:
        if (state) {
            // Minimized: like an app in the background (the sound stops).
            if ((wParam == SIZE_MINIMIZED) != state->minimized) {
                if (wParam == SIZE_MINIMIZED) {
                    state->game.onPause();
                } else {
                    state->game.onResume();
                }
            }
            state->minimized = (wParam == SIZE_MINIMIZED);
            if (!state->minimized) {
                state->game.onSurfaceResized(LOWORD(lParam), HIWORD(lParam));
            }
        }
        return 0;
    case WM_KEYDOWN:
    case WM_KEYUP:
        // While a text field is being edited, Backspace / Enter / Escape edit it (the characters come as WM_CHAR).
        if (msg == WM_KEYDOWN && state && state->game.wantsKeyboard() &&
            (wParam == VK_BACK || wParam == VK_RETURN || wParam == VK_ESCAPE)) {
            state->game.onTextKey(wParam == VK_BACK     ? vd::TextKey::Backspace
                                  : wParam == VK_RETURN ? vd::TextKey::Enter
                                                        : vd::TextKey::Escape);
            return 0;
        }
        if (msg == WM_KEYDOWN && wParam == VK_ESCAPE && !(state && state->game.onEscape())) {
            PostMessageW(hwnd, WM_CLOSE, 0, 0); // development: Escape closes, unless the game used it (a cinematic)
        }
        // Phase 18 (extras): Enter and the arrows (their first press, not the repeats): the glue screens' OnKeyDown, in
        // the world OPENCHAT; the arrows only before the world.
        if (msg == WM_KEYDOWN && state && (lParam & (1 << 30)) == 0) {
            if (wParam == VK_RETURN) {
                state->game.onEnter();
            } else if (wParam == VK_UP || wParam == VK_LEFT || wParam == VK_DOWN || wParam == VK_RIGHT) {
                state->game.onArrow(wParam == VK_DOWN || wParam == VK_RIGHT);
            }
        }
        if (vd::Key key; state && mapKey(wParam, key)) {
            state->game.onKey(key, msg == WM_KEYDOWN);
        }
        return 0;
    case WM_CHAR:
        if (state) {
            // UTF-16 code unit (the window is Unicode): join surrogate pairs; control characters are dropped by the
            // text field (Backspace/Enter/Escape arrive as WM_KEYDOWN).
            const auto unit = static_cast<wchar_t>(wParam);
            if (unit >= 0xD800 && unit < 0xDC00) {
                state->highSurrogate = unit;
                return 0;
            }
            vd::u32 codepoint = unit;
            if (unit >= 0xDC00 && unit < 0xE000) {
                codepoint = state->highSurrogate
                                ? 0x10000u + ((static_cast<vd::u32>(state->highSurrogate) - 0xD800u) << 10) +
                                      (static_cast<vd::u32>(unit) - 0xDC00u)
                                : 0u;
            }
            state->highSurrogate = 0;
            if (codepoint != 0 && state->game.wantsKeyboard()) {
                state->game.onTextChar(codepoint);
            }
        }
        return 0;
    case WM_KILLFOCUS:
        if (state) {
            state->game.onFocusLost();
            state->looking = false;
        }
        return 0;
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
        if (state) {
            state->looking = msg == WM_RBUTTONDOWN;
            state->lastLook = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (state->looking) {
                SetCapture(hwnd);
            } else {
                ReleaseCapture();
            }
        }
        return 0;
    case WM_MOUSEWHEEL:
        if (state) {
            state->game.onMouseWheel(static_cast<float>(GET_WHEEL_DELTA_WPARAM(wParam)) / WHEEL_DELTA);
        }
        return 0;
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        if (state && msg == WM_MOUSEMOVE && state->looking) {
            const POINT now{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            state->game.onMouseLook(static_cast<float>(now.x - state->lastLook.x),
                                    static_cast<float>(now.y - state->lastLook.y));
            state->lastLook = now;
        }
        if (state) {
            const bool down = msg == WM_LBUTTONDOWN || (msg == WM_MOUSEMOVE && (wParam & MK_LBUTTON));
            if (msg == WM_LBUTTONDOWN) {
                SetCapture(hwnd);
            } else if (msg == WM_LBUTTONUP) {
                ReleaseCapture();
            }
            state->game.onPointer(static_cast<float>(GET_X_LPARAM(lParam)), static_cast<float>(GET_Y_LPARAM(lParam)),
                                  down, false);
        }
        return 0;
    case WM_CLOSE:
        // The swapchain must be destroyed before the window, so the main loop owns teardown.
        if (state) {
            state->running = false;
        }
        return 0;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    default:
        break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

int main(int argc, char** argv) {
    SetProcessDPIAware();
    // 1 ms scheduler granularity; the default ~15.6 ms makes frame sleeps overshoot badly.
    timeBeginPeriod(1);
    HINSTANCE instance = GetModuleHandleW(nullptr);

    WNDCLASSEXW windowClass{};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.style = CS_HREDRAW | CS_VREDRAW;
    windowClass.lpfnWndProc = windowProc;
    windowClass.hInstance = instance;
    windowClass.hCursor = LoadCursorW(nullptr, reinterpret_cast<LPCWSTR>(IDC_ARROW));
    windowClass.lpszClassName = L"VanillaDroidWindow";
    if (!RegisterClassExW(&windowClass)) {
        VD_LOG_ERROR("RegisterClassExW failed (%lu)", GetLastError());
        return 1;
    }

    // On the heap: the game's state (its fixed arrays of windows, tables and frames) outgrew the main thread's stack.
    const std::unique_ptr<DesktopState> owned = std::make_unique<DesktopState>();
    DesktopState& state = *owned;
    vd::PlatformInfo platform;
    platform.contentScale = static_cast<float>(GetDpiForSystem()) / USER_DEFAULT_SCREEN_DPI;
    // The window presets are the Fold5's screens at half size: the touch UI at the phone's size in dp.
    platform.uiScale = 1.3f;
    platform.packsDir = VD_DEFAULT_PACKS_DIR;
    platform.configPath = VD_DEFAULT_CONFIG;
    bool noFocus = false; // --no-focus: shown behind the other windows, never activated (unattended runs)
    // --safe-area l,t,r,b: insets in window pixels, to see the touch UI around a notch or rounded corners.
    float safeInsets[4] = {};
    // Phase 20 (look): --cutout x,y,w,h: that safe area is a camera hole's there (window pixels), as Android says it:
    // what lies along the edge clear of it may use the screen (the Mi 11X in landscape, at half size: --safe-area
    // 40,0,0,0 --cutout 0,253,40,40).
    float cutout[4] = {};
    bool hasCutout = false;
    // Server: --account NAME --password PASS (or VD_PASSWORD in the environment) [--realmd host:port]
    // [--character NAME]; the rest comes from config/client.cfg.
    for (int i = 1; i < argc; ++i) {
        const bool hasValue = i + 1 < argc;
        if (std::strcmp(argv[i], "--packs") == 0 && hasValue) {
            platform.packsDir = argv[i + 1];
        } else if (std::strcmp(argv[i], "--bench") == 0 && hasValue) {
            platform.benchmark = argv[i + 1];
        } else if (std::strcmp(argv[i], "--bench-exit") == 0) {
            platform.exitAfterBenchmark = true;
        } else if (std::strcmp(argv[i], "--account") == 0 && hasValue) {
            platform.account = argv[i + 1];
        } else if (std::strcmp(argv[i], "--password") == 0 && hasValue) {
            platform.password = argv[i + 1];
        } else if (std::strcmp(argv[i], "--realmd") == 0 && hasValue) {
            platform.realmd = argv[i + 1];
        } else if (std::strcmp(argv[i], "--character") == 0 && hasValue) {
            platform.character = argv[i + 1];
        } else if (std::strcmp(argv[i], "--walk-route") == 0 && hasValue) {
            platform.walkRoute = argv[i + 1];
        } else if (std::strcmp(argv[i], "--commands") == 0 && hasValue) {
            platform.commands = argv[i + 1];
        } else if (std::strcmp(argv[i], "--walk") == 0) {
            platform.walk = true;
        } else if (std::strcmp(argv[i], "--spawn") == 0 && hasValue) {
            platform.spawn = argv[i + 1];
        } else if (std::strcmp(argv[i], "--hide-ui") == 0) {
            platform.hideDebugUi = true;
        } else if (std::strcmp(argv[i], "--no-focus") == 0) {
            noFocus = true;
        } else if (std::strcmp(argv[i], "--ui-scale") == 0 && hasValue) {
            platform.uiScale = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--trace-movement") == 0) {
            platform.traceMovement = true;
        } else if (std::strcmp(argv[i], "--audio") == 0 && hasValue) {
            platform.audio = argv[i + 1]; // off (default) | null | device | wav:<file>
        } else if (std::strcmp(argv[i], "--quality") == 0 && hasValue) {
            platform.quality = argv[i + 1]; // phase 10: low | medium | high | ultra
        } else if (std::strcmp(argv[i], "--render-scale") == 0 && hasValue) {
            platform.renderScale = static_cast<float>(std::atof(argv[i + 1])); // fixed, no dynamic quality
        } else if (std::strcmp(argv[i], "--weather") == 0 && hasValue) {
            platform.weather = argv[i + 1]; // phase 11: rain|snow|sand|fine[:grade][:instant], as VD_WEATHER
        } else if (std::strcmp(argv[i], "--ghost") == 0) {
            platform.ghost = true; // phase 11: the ghost's world offline, as VD_GHOST=1
        } else if (std::strcmp(argv[i], "--dynamic-quality") == 0 && hasValue) {
            platform.dynamicQuality = argv[i + 1]; // on | off
        } else if (std::strcmp(argv[i], "--bench-minutes") == 0 && hasValue) {
            platform.benchMinutes = static_cast<float>(std::atof(argv[i + 1])); // a looping route's length
        } else if (std::strcmp(argv[i], "--fake-keyboard") == 0 && hasValue) {
            platform.fakeKeyboard = static_cast<float>(std::atof(argv[i + 1]));
        } else if (std::strcmp(argv[i], "--safe-area") == 0 && hasValue) {
            if (std::sscanf(argv[i + 1], "%f,%f,%f,%f", &safeInsets[0], &safeInsets[1], &safeInsets[2],
                            &safeInsets[3]) != 4) {
                VD_LOG_WARN("--safe-area wants left,top,right,bottom (window pixels): %s", argv[i + 1]);
            }
        } else if (std::strcmp(argv[i], "--cutout") == 0 && hasValue) {
            hasCutout =
                std::sscanf(argv[i + 1], "%f,%f,%f,%f", &cutout[0], &cutout[1], &cutout[2], &cutout[3]) == 4;
            if (!hasCutout) {
                VD_LOG_WARN("--cutout wants x,y,width,height (window pixels): %s", argv[i + 1]);
            }
        }
    }
    if (platform.password.empty()) {
        char* password = nullptr;
        size_t length = 0;
        if (_dupenv_s(&password, &length, "VD_PASSWORD") == 0 && password) {
            platform.password = password;
            free(password);
        }
    }
    platform.cacheDir = platform.packsDir + "/.cache";
    CreateDirectoryA(platform.cacheDir.c_str(), nullptr);
    {
        // Phase 22 (pairing): the system's language ("es-ES"), for the first-run screen.
        wchar_t locale[LOCALE_NAME_MAX_LENGTH] = {};
        if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) > 0) {
            for (const wchar_t* c = locale; *c != L'\0' && *c < 128; ++c) {
                platform.systemLanguage.push_back(static_cast<char>(*c));
            }
        }
    }
    const bool initialized = state.game.init(platform);
    platform.password.assign(platform.password.size(), '\0'); // only its SRP6 hash lives on, in memory
    if (!initialized) {
        return 1;
    }
    const auto screenInsets = [&] {
        state.game.onSafeArea(safeInsets[0], safeInsets[1], safeInsets[2], safeInsets[3]);
        if (hasCutout) {
            const float none[4] = {};
            const vd::ui::Rect hole{cutout[0], cutout[1], cutout[2], cutout[3]};
            state.game.onDisplayCutouts(none, {&hole, 1});
        }
    };
    screenInsets();
    // Phase 22 (pairing): --link <vanilladroid://pair?h=...&k=...>, as the PC page's button opens the app on a phone.
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::strcmp(argv[i], "--link") == 0) {
            state.game.onPairingLink(argv[i + 1]);
        }
    }

    // Development views: --map Azeroth --pos -9464,62[,z] [--yaw deg] [--pitch deg] [--height yd] [--hour h]
    // (with z the camera is placed there instead of `height` yards over the terrain)
    {
        std::string map = "Azeroth";
        double x = 0.0, y = 0.0, z = 0.0;
        int posComponents = 0;
        float yaw = 0.0f, pitch = 0.0f, height = 25.0f;
        bool hasPos = false;
        for (int i = 1; i + 1 < argc; ++i) {
            if (std::strcmp(argv[i], "--map") == 0) {
                map = argv[i + 1];
            } else if (std::strcmp(argv[i], "--pos") == 0) {
                posComponents = std::sscanf(argv[i + 1], "%lf,%lf,%lf", &x, &y, &z);
                hasPos = posComponents >= 2;
            } else if (std::strcmp(argv[i], "--yaw") == 0) {
                yaw = static_cast<float>(std::atof(argv[i + 1]));
            } else if (std::strcmp(argv[i], "--pitch") == 0) {
                pitch = static_cast<float>(std::atof(argv[i + 1]));
            } else if (std::strcmp(argv[i], "--height") == 0) {
                height = static_cast<float>(std::atof(argv[i + 1]));
            } else if (std::strcmp(argv[i], "--hour") == 0) {
                state.game.setTimeOfDay(static_cast<float>(std::atof(argv[i + 1])));
            }
        }
        if (hasPos) {
            state.game.setView(map, x, y, yaw, pitch, height,
                               posComponents == 3 ? std::optional<double>(z) : std::nullopt);
        }
    }

    const ScreenPreset preset = parseScreenPreset(argc, argv);
    RECT rect{0, 0, preset.width, preset.height};
    AdjustWindowRect(&rect, WS_OVERLAPPEDWINDOW, FALSE);
    HWND hwnd = CreateWindowExW(0, windowClass.lpszClassName, L"VanillaDroid (desktop)", WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, rect.right - rect.left, rect.bottom - rect.top,
                                nullptr, nullptr, instance, &state);
    if (!hwnd) {
        VD_LOG_ERROR("CreateWindowExW failed (%lu)", GetLastError());
        state.game.shutdown();
        return 1;
    }
    if (noFocus) {
        ShowWindow(hwnd, SW_SHOWNOACTIVATE);
        SetWindowPos(hwnd, HWND_BOTTOM, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }
    VD_LOG_INFO("Desktop window preset: %s screen", preset.name);

    RECT client{};
    GetClientRect(hwnd, &client);
    state.game.onSurfaceCreated(
        {hwnd, instance, static_cast<vd::u32>(client.right), static_cast<vd::u32>(client.bottom)});

    while (state.running) {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!state.running) {
            break;
        }
        if (state.minimized) {
            WaitMessage();
            continue;
        }
        // The pad only drives the window the player is looking at.
        state.game.onGamepad(GetForegroundWindow() == hwnd ? pollGamepad(state.gamepad) : vd::GamepadState{});
        state.game.tick();
        if (state.game.wantsExit()) {
            state.running = false;
        }
        if (state.game.wantsRestart()) {
            // Phase 22 (pairing): the data arrived: the game starts over in place, in the same window (init opens
            // the packs).
            state.game.onSurfaceDestroyed();
            state.game.shutdown();
            std::destroy_at(&state.game);
            std::construct_at(&state.game);
            if (!state.game.init(platform)) {
                state.running = false;
                break;
            }
            screenInsets();
            GetClientRect(hwnd, &client);
            state.game.onSurfaceCreated(
                {hwnd, instance, static_cast<vd::u32>(client.right), static_cast<vd::u32>(client.bottom)});
        }
    }

    state.game.onSurfaceDestroyed();
    DestroyWindow(hwnd);
    state.game.shutdown();
    timeEndPeriod(1);
    return 0;
}
