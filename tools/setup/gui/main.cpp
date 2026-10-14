// VanillaDroid Setup, the window (phase 22, ADR-0005 §4): Dear ImGui over SDL3 with its own renderer
// (Direct3D 11 on Windows, Metal on macOS: nothing to install). It sleeps while nothing changes
// (SDL_WaitEventTimeout): its threads wake it with an SDL event when a task reports progress or the server
// hears from a phone.
//
//   VanillaDroidSetup [--data-dir <folder>] [--lang enUS|esES] [--page
//   home|language|wow|bake|phone|adb|about|
//                     wizard[-wow|-bake|-phone]] [--wow <folder>] [--locale-client <folder>] [--loopback]
//                     [--no-focus]
//
// The options are for tests: a folder of settings of their own, a page to open, a language not saved, a WoW
// or a second client checked as if chosen in the folder window, a server only this PC reaches (127.0.0.1),
// the window shown without taking the focus. A folder dropped on the WoW page is checked as the WoW.
#include "GuiKit.h"
#include "GuiTask.h"
#include "SetupCommon.h"
#include "SetupWindow.h"
#include "core/Log.h"
#include "core/Version.h"
#include "vd_setup_gui_assets.gen.h" // the font and the logo, built in

#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>
#include <imgui.h>
#include <imgui_impl_sdl3.h>
#include <imgui_impl_sdlrenderer3.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <string>

#if defined(_WIN32)
#include <windows.h>

#include <windowsx.h> // GET_X_LPARAM, after windows.h
#endif

namespace {

using namespace vd;
using namespace vd::setup;

Uint32 g_wakeEvent = 0;
std::atomic<bool> g_wakePending{false};

// From any thread: one wake event in the queue at most (the loop redraws once for any number of reports).
void wake() {
    if (!g_wakePending.exchange(true)) {
        SDL_Event event{};
        event.type = g_wakeEvent;
        SDL_PushEvent(&event);
    }
}

#if defined(_WIN32)
// Development builds: VD_SETUP_POSTED_INPUT=1 gives the mouse and keyboard messages another program posts to the window
// (scripts/setup-window-shots.ps1) straight to Dear ImGui. SDL follows the real cursor and the keyboard focus, which an
// automated run must not take from whoever uses the computer.
std::atomic<bool> g_postedInput{false};

bool SDLCALL postedInput(void* userdata, MSG* msg) {
    ImGuiIO& io = ImGui::GetIO();
    const f32 density = std::max(1.0f, SDL_GetWindowPixelDensity(static_cast<SDL_Window*>(userdata)));
    const f32 x = static_cast<f32>(GET_X_LPARAM(msg->lParam)) / density;
    const f32 y = static_cast<f32>(GET_Y_LPARAM(msg->lParam)) / density;
    ImGuiKey key = ImGuiKey_None;
    switch (msg->message) {
    case WM_MOUSEMOVE: io.AddMousePosEvent(x, y); break;
    case WM_LBUTTONDOWN:
    case WM_LBUTTONUP:
        io.AddMousePosEvent(x, y);
        io.AddMouseButtonEvent(ImGuiMouseButton_Left, msg->message == WM_LBUTTONDOWN);
        break;
    case WM_MOUSEWHEEL:
        io.AddMouseWheelEvent(0.0f, static_cast<f32>(GET_WHEEL_DELTA_WPARAM(msg->wParam)) / WHEEL_DELTA);
        break;
    case WM_CHAR: io.AddInputCharacterUTF16(static_cast<ImWchar16>(msg->wParam)); break;
    case WM_KEYDOWN:
    case WM_KEYUP:
        key = msg->wParam == VK_RETURN   ? ImGuiKey_Enter
              : msg->wParam == VK_BACK   ? ImGuiKey_Backspace
              : msg->wParam == VK_TAB    ? ImGuiKey_Tab
              : msg->wParam == VK_ESCAPE ? ImGuiKey_Escape
                                         : ImGuiKey_None;
        if (key == ImGuiKey_None) {
            return true;
        }
        io.AddKeyEvent(key, msg->message == WM_KEYDOWN);
        break;
    default: return true; // everything else as SDL handles it
    }
    g_postedInput = true;
    wake();
    return false;
}
#endif

struct Arguments {
    gui::WindowOptions window;
    bool noFocus = false;
};

Arguments parseArguments(int argc, char** argv) {
    // SDL_main hands the arguments over in UTF-8 on every system.
    Arguments arguments;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const bool hasValue = i + 1 < argc;
        if (arg == "--data-dir" && hasValue) {
            std::error_code error;
            arguments.window.dataDir = std::filesystem::absolute(pathFromUtf8(argv[++i]), error);
        } else if (arg == "--page" && hasValue) {
            arguments.window.page = argv[++i];
        } else if (arg == "--wow" && hasValue) {
            arguments.window.wow = pathFromUtf8(argv[++i]);
        } else if (arg == "--locale-client" && hasValue) {
            arguments.window.localeClient = pathFromUtf8(argv[++i]);
        } else if (arg == "--lang" && hasValue) {
            arguments.window.language = argv[++i];
        } else if (arg == "--loopback") {
            arguments.window.loopback = true;
        } else if (arg == "--no-focus") {
            arguments.noFocus = true;
        }
    }
    return arguments;
}

// The logo from the PNG built in, halved until it is near `side` pixels (a box filter: sharp without a
// mipmap).
SDL_Texture* loadLogo(SDL_Renderer* renderer, int side, f32& width, f32& height) {
    SDL_IOStream* stream = SDL_IOFromConstMem(gui::generated::kLogoPng, sizeof(gui::generated::kLogoPng));
    SDL_Surface* surface = stream ? SDL_LoadPNG_IO(stream, true) : nullptr;
    if (!surface) {
        VD_LOG_WARN("logo: %s", SDL_GetError());
        return nullptr;
    }
    while (surface->w / 2 >= side && surface->h / 2 >= side) {
        SDL_Surface* half = SDL_ScaleSurface(surface, surface->w / 2, surface->h / 2, SDL_SCALEMODE_LINEAR);
        if (!half) {
            break;
        }
        SDL_DestroySurface(surface);
        surface = half;
    }
#if !defined(_WIN32)
    // Windows takes the icon from the program's resources; elsewhere (a run outside the macOS bundle) from
    // here.
    if (SDL_Window* window = SDL_GetRenderWindow(renderer)) {
        SDL_SetWindowIcon(window, surface);
    }
#endif
    SDL_Texture* texture = SDL_CreateTextureFromSurface(renderer, surface);
    width = static_cast<f32>(surface->w);
    height = static_cast<f32>(surface->h);
    SDL_DestroySurface(surface);
    if (texture) {
        SDL_SetTextureScaleMode(texture, SDL_SCALEMODE_LINEAR);
    }
    return texture;
}

f32 displayScale(SDL_Window* window) {
    const f32 scale = window ? SDL_GetDisplayContentScale(SDL_GetDisplayForWindow(window))
                             : SDL_GetDisplayContentScale(SDL_GetPrimaryDisplay());
    return scale > 0.0f ? scale : 1.0f;
}

int run(int argc, char** argv) {
    const Arguments arguments = parseArguments(argc, argv);
    log::setMinimumLevel(log::Level::Warn);
    if (arguments.noFocus) {
        SDL_SetHint(SDL_HINT_WINDOW_ACTIVATE_WHEN_SHOWN, "0");
    }
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        VD_LOG_ERROR("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    g_wakeEvent = SDL_RegisterEvents(1);
    gui::setWakeFunction(wake);

    f32 scale = displayScale(nullptr);
    SDL_Window* window =
        SDL_CreateWindow("", static_cast<int>(1060 * scale), static_cast<int>(740 * scale),
                         SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (!window) {
        VD_LOG_ERROR("SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    SDL_SetWindowMinimumSize(window, static_cast<int>(780 * scale), static_cast<int>(560 * scale));
    SDL_Renderer* renderer = SDL_CreateRenderer(window, nullptr);
    if (!renderer) {
        VD_LOG_ERROR("SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return 1;
    }
    SDL_SetRenderVSync(renderer, 1);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // nothing written next to the program
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    ImFontConfig font;
    font.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(gui::generated::kDroidSans),
                                   static_cast<int>(sizeof(gui::generated::kDroidSans)), gui::kFontSize,
                                   &font);
    gui::applyStyle(scale);
    ImGui_ImplSDL3_InitForSDLRenderer(window, renderer);
    ImGui_ImplSDLRenderer3_Init(renderer);
#if defined(_WIN32)
    if (!kDistBuild && std::getenv("VD_SETUP_POSTED_INPUT")) {
        SDL_SetWindowsMessageHook(postedInput, window);
    }
#endif

    {
        gui::SetupWindow setup(window, arguments.window);
        f32 logoWidth = 0.0f;
        f32 logoHeight = 0.0f;
        SDL_Texture* logo = loadLogo(renderer, static_cast<int>(96 * scale * 2), logoWidth, logoHeight);
        setup.setLogo(logo, logoWidth, logoHeight);

        SDL_SetWindowPosition(window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowWindow(window);

        bool quit = false;
        int framesLeft = 3;                // ImGui settles over a few frames after a change
        Uint64 lastInput = SDL_GetTicks(); // tooltips appear a moment after the mouse stops
        Uint64 lastFrame = 0;
        while (!quit) {
            // Asleep until something happens: input, a task's progress, a phone, a timer while something
            // moves.
            Sint32 timeout = -1;
            if (framesLeft > 0) {
                timeout = 0;
            } else if (setup.animating()) {
                timeout = 100;
            } else if (SDL_GetTicks() - lastInput < 2000) {
                timeout = 250;
            }
            SDL_Event event;
            bool got = SDL_WaitEventTimeout(&event, timeout);
            bool input = false;
            bool woken = false;
            while (got) {
                if (event.type == g_wakeEvent) {
                    g_wakePending = false;
                    woken = true;
                } else {
                    ImGui_ImplSDL3_ProcessEvent(&event);
                    input = true;
                    if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED &&
                                                         event.window.windowID == SDL_GetWindowID(window))) {
                        quit = setup.requestClose();
                    } else if (event.type == SDL_EVENT_DROP_FILE && event.drop.data) {
                        setup.dropped(pathFromUtf8(event.drop.data));
                    } else if (event.type == SDL_EVENT_WINDOW_DISPLAY_SCALE_CHANGED) {
                        scale = displayScale(window);
                        gui::applyStyle(scale);
                    }
                }
                got = SDL_PollEvent(&event);
            }
#if defined(_WIN32)
            input = g_postedInput.exchange(false) || input;
#endif
            if (input) {
                lastInput = SDL_GetTicks();
                framesLeft = std::max(framesLeft, 3);
            }
            if (quit || setup.closeConfirmed()) {
                break;
            }
            if (SDL_GetWindowFlags(window) & SDL_WINDOW_MINIMIZED) {
                setup.update(); // tasks still end and the server's events are still counted
                framesLeft = 0; // restoring it is an event: it draws then
                continue;
            }
            // Progress alone redraws at most 30 times a second.
            if (woken && !input && framesLeft == 0 && SDL_GetTicks() - lastFrame < 33) {
                SDL_Delay(static_cast<Uint32>(33 - (SDL_GetTicks() - lastFrame)));
            }

            if (setup.update()) {
                framesLeft = std::max(framesLeft, 2);
            }
            ImGui_ImplSDLRenderer3_NewFrame();
            ImGui_ImplSDL3_NewFrame();
            ImGui::NewFrame();
            setup.draw();
            ImGui::Render();
            SDL_SetRenderScale(renderer, io.DisplayFramebufferScale.x, io.DisplayFramebufferScale.y);
            SDL_SetRenderDrawColorFloat(renderer, gui::kBackground.x, gui::kBackground.y, gui::kBackground.z,
                                        1.0f);
            SDL_RenderClear(renderer);
            ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), renderer);
            SDL_RenderPresent(renderer);
            lastFrame = SDL_GetTicks();
            framesLeft = std::max(0, framesLeft - 1);
        }

        SDL_HideWindow(window);
        setup.shutdown(); // the server, a bake, adb: stopped and waited for
        if (logo) {
            SDL_DestroyTexture(logo);
        }
    }
    gui::setWakeFunction(nullptr);
    ImGui_ImplSDLRenderer3_Shutdown();
    ImGui_ImplSDL3_Shutdown();
    ImGui::DestroyContext();
    SDL_DestroyRenderer(renderer);
    SDL_DestroyWindow(window);
    SDL_Quit();
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    return run(argc, argv);
}
