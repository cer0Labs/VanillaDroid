#include "AppPackInstall.h" // phase 22 (apppack)
#include "app/Game.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "platform/Haptics.h" // phase 20 (assists)

#include <android/configuration.h>
#include <android/input.h>
#include <android/keycodes.h>
#include <android/native_window.h>
#include <game-activity/native_app_glue/android_native_app_glue.h>
#include <paddleboat/paddleboat.h>
#include <pthread.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <climits>
#include <cmath> // phase 20 (edit)
#include <cstdlib>
#include <memory>
#include <mutex> // phase 22 (pairing)
#include <string>
#include <string_view>

namespace {

// Audio focus (phase 9): MainActivity's listener runs on the Java UI thread and leaves the last change here
// (AudioManager.AUDIOFOCUS_*: 1 gained, -1 lost, -2 lost for a while, -3 another app talks over us); the game thread
// takes it each frame.
constexpr int kNoFocusChange = INT_MIN;
std::atomic<int> g_audioFocusChange{kNoFocusChange};

// Phase 10: MainActivity.onTrimMemory's level (ComponentCallbacks2.TRIM_MEMORY_*), left here on the Java UI thread just
// before GameActivity sends APP_CMD_LOW_MEMORY (which carries no level); -1 when none.
std::atomic<int> g_trimMemoryLevel{-1};

// Phase 22 (pairing): the vanilladroid://pair link MainActivity received (the app opened by the PC page's button, or
// brought back by it while running), left here on the Java UI thread; the game thread takes it each frame.
std::mutex g_linkMutex;
std::string g_pendingLink;

std::string takePendingLink() {
    std::lock_guard<std::mutex> lock(g_linkMutex);
    std::string link;
    link.swap(g_pendingLink);
    return link;
}

struct AndroidState {
    vd::Game game;
    JNIEnv* env = nullptr; // attached to the game thread
    bool hasWindow = false;
    // Soft keyboard (GameTextInput, inside GameActivity).
    bool keyboardRequested = false; // shown on our request and not hidden since
    bool keyboardVisible = false;   // on screen, as the system last said
    vd::u32 imeVersion = ~0u;       // the TextInput::version() whose text the IME holds
    vd::u64 imeField = 0;           // the field whose kind of keyboard the IME was set up for
    // Modified UTF-8 of the field: TextInput's bytes, codepoints above U+FFFF taking 6 instead of 4.
    std::array<char, vd::ui::TextInput::kCapacity * 2> imeText{};
    // Game controllers (Paddleboat).
    bool paddleboat = false;
    uint64_t controllerAxes = 0; // axes enabled in GameActivity's motion events for the controllers
    bool finishing = false;      // parity (popups): Exit Game asked the activity to finish
};

vd::NativeSurface makeSurface(ANativeWindow* window) {
    return {window, nullptr, static_cast<vd::u32>(ANativeWindow_getWidth(window)),
            static_cast<vd::u32>(ANativeWindow_getHeight(window))};
}

// Physical pixels per dp: densityDpi / 160. The Fold5's cover and inner screens differ, so it is read again on every
// configuration change.
float densityScale(android_app* app) {
    int32_t dpi = GameActivity_getDensityDpi(app->activity);
    if (dpi <= 0 && app->config) {
        dpi = AConfiguration_getDensity(app->config);
    }
    if (dpi <= 0 || dpi == ACONFIGURATION_DENSITY_ANY || dpi == ACONFIGURATION_DENSITY_NONE) {
        return 0.0f;
    }
    return static_cast<float>(dpi) / static_cast<float>(ACONFIGURATION_DENSITY_MEDIUM);
}

// Phase 20 (edit, docs/MOBILE_CONTROLS.md §3.4): the screen's real pixels per millimetre, for the touch cluster's sizes
// in mm (16 mm must be 16 mm on both of the Fold5's screens and on a 20:9 phone). densityDpi is a bucket (420, 480...),
// not the panel's; the display's real metrics (WindowManager.getDefaultDisplay().getRealMetrics: xdpi, ydpi, read when
// asked, after a fold too) are. 0 when they are missing or absurd (not square pixels, or more than twice off the
// bucket's): the density's then.
float pixelsPerMm(JNIEnv* env, jobject activity, float dpScale) {
    if (!env || !activity) {
        return 0.0f;
    }
    float xdpi = 0.0f, ydpi = 0.0f;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getWindowManager = env->GetMethodID(activityClass, "getWindowManager", "()Landroid/view/WindowManager;");
    jobject manager = getWindowManager ? env->CallObjectMethod(activity, getWindowManager) : nullptr;
    jclass managerClass = manager ? env->GetObjectClass(manager) : nullptr;
    jmethodID getDisplay =
        managerClass ? env->GetMethodID(managerClass, "getDefaultDisplay", "()Landroid/view/Display;") : nullptr;
    jobject display = getDisplay ? env->CallObjectMethod(manager, getDisplay) : nullptr;
    jclass metricsClass = env->FindClass("android/util/DisplayMetrics");
    jmethodID metricsInit = metricsClass ? env->GetMethodID(metricsClass, "<init>", "()V") : nullptr;
    jobject metrics = metricsInit ? env->NewObject(metricsClass, metricsInit) : nullptr;
    jclass displayClass = display ? env->GetObjectClass(display) : nullptr;
    jmethodID getRealMetrics =
        displayClass ? env->GetMethodID(displayClass, "getRealMetrics", "(Landroid/util/DisplayMetrics;)V") : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    } else if (display && metrics && getRealMetrics) {
        env->CallVoidMethod(display, getRealMetrics, metrics);
        jfieldID x = env->GetFieldID(metricsClass, "xdpi", "F");
        jfieldID y = env->GetFieldID(metricsClass, "ydpi", "F");
        if (!env->ExceptionCheck() && x && y) {
            xdpi = env->GetFloatField(metrics, x);
            ydpi = env->GetFloatField(metrics, y);
        }
    }
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    }
    for (jobject ref : {static_cast<jobject>(activityClass), manager, static_cast<jobject>(managerClass), display,
                        static_cast<jobject>(metricsClass), metrics, static_cast<jobject>(displayClass)}) {
        if (ref) {
            env->DeleteLocalRef(ref);
        }
    }
    const float dpi = 0.5f * (xdpi + ydpi);
    const float bucket = dpScale * static_cast<float>(ACONFIGURATION_DENSITY_MEDIUM);
    const bool sane = xdpi > 0.0f && ydpi > 0.0f && std::fabs(xdpi - ydpi) <= 0.15f * dpi && dpi >= 100.0f &&
                      dpi <= 1000.0f && (bucket <= 0.0f || (dpi >= 0.5f * bucket && dpi <= 2.0f * bucket));
    VD_LOG_INFO("Screen: xdpi %.1f ydpi %.1f (density bucket %.0f)%s", xdpi, ydpi, bucket,
                sane ? "" : ": not used, the bucket's millimetres");
    return sane ? dpi / 25.4f : 0.0f;
}

// Phase 20 (look): the display cutout's rectangles (WindowManager.getDefaultDisplay().getCutout().getBoundingRects(),
// API 29), in the display's current rotation: the window's coordinates for our full-screen window. False when they
// cannot be asked (`count` 0 with true: no cutout).
bool cutoutRects(JNIEnv* env, jobject activity, std::array<vd::ui::Rect, vd::ScreenEdges::kMaxCutouts>& out,
                 vd::u32& count) {
    count = 0;
    if (!env || !activity) {
        return false;
    }
    bool ok = false;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getWindowManager = env->GetMethodID(activityClass, "getWindowManager", "()Landroid/view/WindowManager;");
    jobject manager = getWindowManager ? env->CallObjectMethod(activity, getWindowManager) : nullptr;
    jclass managerClass = manager ? env->GetObjectClass(manager) : nullptr;
    jmethodID getDisplay =
        managerClass ? env->GetMethodID(managerClass, "getDefaultDisplay", "()Landroid/view/Display;") : nullptr;
    jobject display = getDisplay ? env->CallObjectMethod(manager, getDisplay) : nullptr;
    jclass displayClass = display ? env->GetObjectClass(display) : nullptr;
    jmethodID getCutout =
        displayClass ? env->GetMethodID(displayClass, "getCutout", "()Landroid/view/DisplayCutout;") : nullptr;
    jobject cutout = getCutout ? env->CallObjectMethod(display, getCutout) : nullptr;
    jclass cutoutClass = cutout ? env->GetObjectClass(cutout) : nullptr;
    jmethodID getRects = cutoutClass ? env->GetMethodID(cutoutClass, "getBoundingRects", "()Ljava/util/List;") : nullptr;
    jobject list = getRects ? env->CallObjectMethod(cutout, getRects) : nullptr;
    jclass listClass = list ? env->GetObjectClass(list) : nullptr;
    jclass rectClass = env->FindClass("android/graphics/Rect");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
    } else if (getCutout && !cutout) {
        ok = true; // no cutout on this display
    } else if (list && listClass && rectClass) {
        jmethodID size = env->GetMethodID(listClass, "size", "()I");
        jmethodID get = env->GetMethodID(listClass, "get", "(I)Ljava/lang/Object;");
        jfieldID left = env->GetFieldID(rectClass, "left", "I");
        jfieldID top = env->GetFieldID(rectClass, "top", "I");
        jfieldID right = env->GetFieldID(rectClass, "right", "I");
        jfieldID bottom = env->GetFieldID(rectClass, "bottom", "I");
        const jint n = size && get && left && top && right && bottom ? env->CallIntMethod(list, size) : 0;
        for (jint i = 0; i < n && count < out.size() && !env->ExceptionCheck(); ++i) {
            jobject rect = env->CallObjectMethod(list, get, i);
            if (rect && !env->ExceptionCheck()) {
                const float l = static_cast<float>(env->GetIntField(rect, left));
                const float t = static_cast<float>(env->GetIntField(rect, top));
                const float r = static_cast<float>(env->GetIntField(rect, right));
                const float b = static_cast<float>(env->GetIntField(rect, bottom));
                if (r > l && b > t) {
                    out[count++] = {l, t, r - l, b - t};
                }
            }
            if (rect) {
                env->DeleteLocalRef(rect);
            }
        }
        ok = !env->ExceptionCheck();
        if (!ok) {
            env->ExceptionClear();
        }
    }
    for (jobject ref : {static_cast<jobject>(activityClass), manager, static_cast<jobject>(managerClass), display,
                        static_cast<jobject>(displayClass), cutout, static_cast<jobject>(cutoutClass), list,
                        static_cast<jobject>(listClass), static_cast<jobject>(rectClass)}) {
        if (ref) {
            env->DeleteLocalRef(ref);
        }
    }
    return ok;
}

// Safe area: what the system bars (hidden in immersive mode, so usually 0), the display cutout (the camera hole in
// landscape) and curved edges take from each side, plus the soft keyboard's height apart. Window insets are in the
// window's coordinates, as the player sees the screen: the same space as the touches, before any pre-rotation.
// Phase 20 (look): the cutout's rectangles apart too, and the insets that are not its.
void updateInsets(android_app* app, vd::Game& game, JNIEnv* env) {
    ARect safe{0, 0, 0, 0};
    ARect other{0, 0, 0, 0}; // without the cutout's
    for (const GameCommonInsetsType type : {GAMECOMMON_INSETS_TYPE_SYSTEM_BARS, GAMECOMMON_INSETS_TYPE_DISPLAY_CUTOUT,
                                            GAMECOMMON_INSETS_TYPE_WATERFALL}) {
        ARect insets{0, 0, 0, 0};
        GameActivity_getWindowInsets(app->activity, type, &insets);
        safe.left = std::max(safe.left, insets.left);
        safe.top = std::max(safe.top, insets.top);
        safe.right = std::max(safe.right, insets.right);
        safe.bottom = std::max(safe.bottom, insets.bottom);
        if (type != GAMECOMMON_INSETS_TYPE_DISPLAY_CUTOUT) {
            other.left = std::max(other.left, insets.left);
            other.top = std::max(other.top, insets.top);
            other.right = std::max(other.right, insets.right);
            other.bottom = std::max(other.bottom, insets.bottom);
        }
    }
    game.onSafeArea(static_cast<float>(safe.left), static_cast<float>(safe.top), static_cast<float>(safe.right),
                    static_cast<float>(safe.bottom));
    std::array<vd::ui::Rect, vd::ScreenEdges::kMaxCutouts> rects{};
    vd::u32 count = 0;
    if (cutoutRects(env, app->activity->javaGameActivity, rects, count)) {
        const float insets[4] = {static_cast<float>(other.left), static_cast<float>(other.top),
                                 static_cast<float>(other.right), static_cast<float>(other.bottom)};
        game.onDisplayCutouts(insets, {rects.data(), count});
    }
    ARect ime{0, 0, 0, 0};
    GameActivity_getWindowInsets(app->activity, GAMECOMMON_INSETS_TYPE_IME, &ime);
    game.onKeyboardInset(static_cast<float>(ime.bottom));
}

void handleAppCmd(android_app* app, int32_t cmd) {
    auto* state = static_cast<AndroidState*>(app->userData);
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (app->window) {
            state->game.onSurfaceCreated(makeSurface(app->window));
            state->hasWindow = true;
            updateInsets(app, state->game, state->env);
        }
        break;
    case APP_CMD_TERM_WINDOW:
        state->game.onSurfaceDestroyed();
        state->hasWindow = false;
        break;
    case APP_CMD_WINDOW_RESIZED:
    case APP_CMD_CONFIG_CHANGED:
        // Fold/unfold and rotation land here when the surface survives the change.
        if (cmd == APP_CMD_CONFIG_CHANGED) {
            const float scale = densityScale(app);
            state->game.onContentScale(scale);
            state->game.onPixelsPerMm(pixelsPerMm(state->env, app->activity->javaGameActivity, scale)); // phase 20
        }
        if (app->window) {
            const vd::NativeSurface surface = makeSurface(app->window);
            state->game.onSurfaceResized(surface.width, surface.height);
        }
        break;
    case APP_CMD_WINDOW_INSETS_CHANGED:
        updateInsets(app, state->game, state->env);
        break;
    case APP_CMD_EDITOR_ACTION:
        // The soft keyboard's action key (Done / Enter), whatever action it shows (app->editorAction).
        if (state->game.wantsKeyboard()) {
            state->game.onTextKey(vd::TextKey::Enter);
        }
        break;
    case APP_CMD_SOFTWARE_KB_VIS_CHANGED:
        // Back while the soft keyboard is up only closes the keyboard (the IME takes it): the player hid the keyboard
        // of a field still being edited, which is how Escape looks on a phone.
        if (!app->softwareKeyboardVisible && state->keyboardVisible && state->keyboardRequested &&
            state->game.wantsKeyboard()) {
            state->game.onTextKey(vd::TextKey::Escape);
        }
        state->keyboardVisible = app->softwareKeyboardVisible;
        break;
    case APP_CMD_START:
        if (state->paddleboat) {
            Paddleboat_onStart(state->env);
        }
        break;
    case APP_CMD_STOP:
        if (state->paddleboat) {
            Paddleboat_onStop(state->env);
        }
        break;
    case APP_CMD_LOW_MEMORY:
        // Every onTrimMemory arrives as this command; its level comes through MainActivity.
        if (const int level = g_trimMemoryLevel.exchange(-1); level >= 0) {
            state->game.onTrimMemory(level);
        } else {
            state->game.onLowMemory();
        }
        break;
    case APP_CMD_PAUSE:
        state->game.onPause(); // the sound stops in the background
        break;
    case APP_CMD_RESUME:
        state->game.onResume();
        break;
    case APP_CMD_LOST_FOCUS:
        state->game.onFocusLost();
        break;
    default:
        break;
    }
}

// --- Soft keyboard -------------------------------------------------------------------------------------------------

// The IME changed the text (typing, autocorrection, a paste): the whole field, in modified UTF-8.
void onImeState(void* context, const GameTextInputState* ime) {
    auto* state = static_cast<AndroidState*>(context);
    if (!ime || !ime->text_UTF8 || ime->text_length < 0) {
        return;
    }
    const std::string_view text(ime->text_UTF8, static_cast<size_t>(ime->text_length));
    state->game.onTextInputState(text);
    // The engine may have cut (maxBytes) or filtered it: only then is the IME re-seeded with what the field kept.
    const vd::ui::TextInput& input = state->game.textInput();
    const size_t kept = vd::ui::toModifiedUtf8(input.text(), state->imeText);
    if (std::string_view(state->imeText.data(), kept) == text) {
        state->imeVersion = input.version();
    }
}

// After each frame: the keyboard follows Game::wantsKeyboard(), and the IME is seeded with the field's text whenever the
// engine (not the IME) changed it (a field starts, the chat line is cleared after sending).
void syncKeyboard(android_app* app, AndroidState& state) {
    const bool wanted = state.game.wantsKeyboard();
    const vd::ui::TextInput& input = state.game.textInput();
    if (wanted && input.field() != state.imeField) {
        // The keyboard for this kind of field: passwords masked and never learnt, names without suggestions, chat
        // with them and a "send" key. A keyboard already up picks the change when the input restarts.
        uint32_t type = TYPE_CLASS_TEXT;
        uint32_t options = IME_FLAG_NO_EXTRACT_UI;
        GameTextInputActionType action = IME_ACTION_DONE;
        switch (input.kind()) {
        case vd::ui::TextKind::Secret:
            type |= TYPE_TEXT_VARIATION_PASSWORD;
            options |= IME_FLAG_NO_PERSONALIZED_LEARNING;
            break;
        case vd::ui::TextKind::Plain: type |= TYPE_TEXT_FLAG_NO_SUGGESTIONS; break;
        case vd::ui::TextKind::Message: action = IME_ACTION_SEND; break;
        case vd::ui::TextKind::MultiLine: // phase 15 (options): a macro's body, Enter types a line break
            type |= TYPE_TEXT_FLAG_MULTI_LINE;
            action = IME_ACTION_NONE;
            options |= IME_FLAG_NO_ENTER_ACTION;
            break;
        case vd::ui::TextKind::Digits: type = TYPE_CLASS_NUMBER; break; // phase 22 (pairing): the pairing code
        }
        GameActivity_setImeEditorInfo(app->activity, static_cast<GameTextInputType>(type), action,
                                      static_cast<GameTextInputImeOptions>(options));
        if (state.keyboardRequested) {
            GameActivity_restartInput(app->activity);
        }
        state.imeField = input.field();
    }
    if (wanted && input.version() != state.imeVersion) {
        const size_t length = vd::ui::toModifiedUtf8(input.text(), state.imeText);
        const int32_t cursor = static_cast<int32_t>(vd::ui::utf16Length(input.text())); // Java string indices
        GameTextInputState ime{};
        ime.text_UTF8 = state.imeText.data();
        ime.text_length = static_cast<int32_t>(length);
        ime.selection = {cursor, cursor};
        ime.composingRegion = {SPAN_UNDEFINED, SPAN_UNDEFINED};
        GameActivity_setTextInputState(app->activity, &ime);
        state.imeVersion = input.version();
    }
    if (wanted != state.keyboardRequested) {
        if (wanted) {
            GameActivity_showSoftInput(app->activity, 0);
        } else {
            GameActivity_hideSoftInput(app->activity, 0);
            state.imeField = 0; // the next field sets its kind of keyboard again
        }
        state.keyboardRequested = wanted;
    }
}

// --- Game controllers ----------------------------------------------------------------------------------------------

// Motion events kept by the glue: the touchscreen (fingers, the S Pen) and controllers' sticks and triggers.
bool motionFilter(const GameActivityMotionEvent* event) {
    const int32_t source = event->source;
    return (source & AINPUT_SOURCE_TOUCHSCREEN) == AINPUT_SOURCE_TOUCHSCREEN ||
           (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK ||
           (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD;
}

// GameActivity only copies the axes it is told about into its motion events: those the connected controllers use.
void enableControllerAxes(AndroidState& state) {
    const uint64_t active = Paddleboat_getActiveAxisMask();
    uint64_t added = active & ~state.controllerAxes;
    state.controllerAxes = active;
    for (int32_t axis = 0; added != 0; ++axis, added >>= 1) {
        if (added & 1) {
            GameActivityPointerAxes_enableAxis(axis);
        }
    }
}

// The first active controller, in the engine's terms (y up).
vd::GamepadState readGamepad() {
    for (int32_t i = 0; i < PADDLEBOAT_MAX_CONTROLLERS; ++i) {
        Paddleboat_Controller_Data data{};
        if (Paddleboat_getControllerStatus(i) != PADDLEBOAT_CONTROLLER_ACTIVE ||
            Paddleboat_getControllerData(i, &data) != PADDLEBOAT_NO_ERROR) {
            continue;
        }
        vd::GamepadState pad;
        pad.connected = true;
        pad.leftX = data.leftStick.stickX;
        pad.leftY = -data.leftStick.stickY; // Android axes point down
        pad.rightX = data.rightStick.stickX;
        pad.rightY = -data.rightStick.stickY;
        pad.leftTrigger = data.triggerL2;
        pad.rightTrigger = data.triggerR2;
        static constexpr struct {
            uint32_t paddleboat;
            vd::u32 button;
        } kButtons[] = {{PADDLEBOAT_BUTTON_A, vd::gamepad::kA},
                        {PADDLEBOAT_BUTTON_B, vd::gamepad::kB},
                        {PADDLEBOAT_BUTTON_X, vd::gamepad::kX},
                        {PADDLEBOAT_BUTTON_Y, vd::gamepad::kY},
                        {PADDLEBOAT_BUTTON_L1, vd::gamepad::kLB},
                        {PADDLEBOAT_BUTTON_R1, vd::gamepad::kRB},
                        {PADDLEBOAT_BUTTON_SELECT, vd::gamepad::kBack},
                        {PADDLEBOAT_BUTTON_START, vd::gamepad::kStart},
                        {PADDLEBOAT_BUTTON_DPAD_UP, vd::gamepad::kDpadUp},
                        {PADDLEBOAT_BUTTON_DPAD_DOWN, vd::gamepad::kDpadDown},
                        {PADDLEBOAT_BUTTON_DPAD_LEFT, vd::gamepad::kDpadLeft},
                        {PADDLEBOAT_BUTTON_DPAD_RIGHT, vd::gamepad::kDpadRight},
                        {PADDLEBOAT_BUTTON_L3, vd::gamepad::kL3},
                        {PADDLEBOAT_BUTTON_R3, vd::gamepad::kR3}};
        for (const auto& b : kButtons) {
            if (data.buttonsDown & b.paddleboat) {
                pad.buttons |= b.button;
            }
        }
        return pad;
    }
    return {};
}

// --- Key events ----------------------------------------------------------------------------------------------------

void dispatchKey(AndroidState& state, const GameActivityKeyEvent& event) {
    if (state.paddleboat && Paddleboat_processGameActivityKeyInputEvent(&event, sizeof(event))) {
        return; // a controller's button
    }
    // While typing, the soft keyboard (or a hardware keyboard, through the same InputConnection) edits the text
    // itself; what reaches here is Back or Escape with no keyboard on screen.
    if (event.action == AKEY_EVENT_ACTION_DOWN && state.game.wantsKeyboard() &&
        (event.keyCode == AKEYCODE_BACK || event.keyCode == AKEYCODE_ESCAPE)) {
        state.game.onTextKey(vd::TextKey::Escape);
    } else if (event.action == AKEY_EVENT_ACTION_DOWN &&
               (event.keyCode == AKEYCODE_BACK || event.keyCode == AKEYCODE_ESCAPE)) {
        state.game.onEscape(); // phase 14: stops a cinematic; phase 18 (extras): the glue screens, ToggleGameMenu
    } else if (event.action == AKEY_EVENT_ACTION_DOWN && event.repeatCount == 0 && !state.game.wantsKeyboard()) {
        // Phase 18 (extras): a physical keyboard's Enter and arrows (the glue screens' OnKeyDown, OPENCHAT).
        if (event.keyCode == AKEYCODE_ENTER || event.keyCode == AKEYCODE_NUMPAD_ENTER) {
            state.game.onEnter();
        } else if (event.keyCode == AKEYCODE_DPAD_UP || event.keyCode == AKEYCODE_DPAD_LEFT ||
                   event.keyCode == AKEYCODE_DPAD_DOWN || event.keyCode == AKEYCODE_DPAD_RIGHT) {
            state.game.onArrow(event.keyCode == AKEYCODE_DPAD_DOWN || event.keyCode == AKEYCODE_DPAD_RIGHT);
        }
    }
}

#if !defined(VD_DIST)
// String extra of the launch intent ("" if absent): `adb shell am start ... --es bench elwynn`.
std::string intentExtra(JNIEnv* env, jobject activity, const char* name) {
    std::string result;
    jclass activityClass = env->GetObjectClass(activity);
    jmethodID getIntent = env->GetMethodID(activityClass, "getIntent", "()Landroid/content/Intent;");
    jobject intent = getIntent ? env->CallObjectMethod(activity, getIntent) : nullptr;
    if (intent) {
        jclass intentClass = env->GetObjectClass(intent);
        jmethodID getStringExtra =
            env->GetMethodID(intentClass, "getStringExtra", "(Ljava/lang/String;)Ljava/lang/String;");
        jstring key = env->NewStringUTF(name);
        auto value = static_cast<jstring>(getStringExtra ? env->CallObjectMethod(intent, getStringExtra, key) : nullptr);
        if (value) {
            const char* chars = env->GetStringUTFChars(value, nullptr);
            result = chars;
            env->ReleaseStringUTFChars(value, chars);
            env->DeleteLocalRef(value);
        }
        env->DeleteLocalRef(key);
        env->DeleteLocalRef(intentClass);
        env->DeleteLocalRef(intent);
    }
    env->DeleteLocalRef(activityClass);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return {};
    }
    return result;
}
#endif

// Every pointer of a multi-touch event, with its stable id. Gestures are interpreted by the engine. Controllers' motion
// events go to Paddleboat.
void dispatchMotion(AndroidState& state, const GameActivityMotionEvent& event) {
    if (state.paddleboat && Paddleboat_processGameActivityMotionInputEvent(&event, sizeof(event))) {
        return;
    }
    if ((event.source & AINPUT_SOURCE_TOUCHSCREEN) != AINPUT_SOURCE_TOUCHSCREEN) {
        return;
    }
    vd::Game& game = state.game;
    const int32_t action = event.action & AMOTION_EVENT_ACTION_MASK;
    const uint32_t actionIndex =
        (event.action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    for (uint32_t p = 0; p < event.pointerCount; ++p) {
        const GameActivityPointerAxes& pointer = event.pointers[p];
        const float x = GameActivityPointerAxes_getX(&pointer);
        const float y = GameActivityPointerAxes_getY(&pointer);
        vd::TouchPhase phase = vd::TouchPhase::Move;
        switch (action) {
        case AMOTION_EVENT_ACTION_DOWN:
        case AMOTION_EVENT_ACTION_POINTER_DOWN:
            if (p != actionIndex) {
                continue;
            }
            phase = vd::TouchPhase::Down;
            break;
        case AMOTION_EVENT_ACTION_UP:
        case AMOTION_EVENT_ACTION_POINTER_UP:
            if (p != actionIndex) {
                continue;
            }
            phase = vd::TouchPhase::Up;
            break;
        case AMOTION_EVENT_ACTION_CANCEL:
            phase = vd::TouchPhase::Cancel;
            break;
        case AMOTION_EVENT_ACTION_MOVE:
            break;
        default:
            continue;
        }
        game.onTouch(phase, pointer.id, x, y);
    }
}

// Phase 22 (pairing): the data arrived (Game::wantsRestart): the game starts over in place, on the same activity and
// window, as when Android brings the activity back in a process it kept (Game::init opens the packs then). The screen's
// density may have changed since the start (a fold).
bool restartGame(android_app* app, AndroidState& state, vd::PlatformInfo& platform) {
    GameActivity_hideSoftInput(app->activity, 0);
    state.keyboardRequested = false;
    state.imeVersion = ~0u;
    state.imeField = 0;
    state.game.onSurfaceDestroyed();
    state.game.shutdown();
    std::destroy_at(&state.game);
    std::construct_at(&state.game);
    if (const float scale = densityScale(app); scale > 0.0f) {
        platform.contentScale = scale;
    }
    platform.pixelsPerMm = pixelsPerMm(state.env, app->activity->javaGameActivity, platform.contentScale);
    if (!state.game.init(platform)) {
        return false;
    }
    if (state.hasWindow && app->window) {
        state.game.onSurfaceCreated(makeSurface(app->window));
        updateInsets(app, state.game, state.env);
    }
    return true;
}

// Phase 22 (pairing): the system's language, "es-ES" (for the first-run screen while no client.cfg says one).
std::string systemLanguage(android_app* app) {
    if (!app->config) {
        return {};
    }
    char language[2] = {};
    char country[2] = {};
    AConfiguration_getLanguage(app->config, language);
    AConfiguration_getCountry(app->config, country);
    std::string out;
    if (language[0] != '\0') {
        out.assign(language, language[1] != '\0' ? 2 : 1);
        if (country[0] != '\0' && country[1] != '\0') {
            out += '-';
            out.append(country, 2);
        }
    }
    return out;
}

} // namespace

extern "C" JNIEXPORT void JNICALL Java_dev_cer_vanilladroid_MainActivity_nativeOnAudioFocusChange(JNIEnv*, jclass,
                                                                                                 jint focusChange) {
    g_audioFocusChange.store(focusChange);
}

extern "C" JNIEXPORT void JNICALL Java_dev_cer_vanilladroid_MainActivity_nativeOnTrimMemory(JNIEnv*, jclass,
                                                                                           jint level) {
    // The deepest level asked since the game thread last looked.
    int current = g_trimMemoryLevel.load();
    while (level > current && !g_trimMemoryLevel.compare_exchange_weak(current, level)) {
    }
}

extern "C" JNIEXPORT void JNICALL Java_dev_cer_vanilladroid_MainActivity_nativeOnLink(JNIEnv* env, jclass,
                                                                                      jstring uri) {
    if (!uri) {
        return;
    }
    const char* chars = env->GetStringUTFChars(uri, nullptr);
    if (chars) {
        std::lock_guard<std::mutex> lock(g_linkMutex);
        g_pendingLink = chars; // the newest one: a link tapped twice pairs once
        env->ReleaseStringUTFChars(uri, chars);
    }
}

extern "C" void android_main(android_app* app) {
    // Swappy needs a JNIEnv valid on this (the engine) thread.
    JavaVM* vm = app->activity->vm;
    JNIEnv* env = nullptr;
    vm->AttachCurrentThread(&env, nullptr);
    pthread_setname_np(pthread_self(), "vd-game"); // readable in Perfetto / AGI / Tracy
    VD_PROFILE_THREAD_NAME("vd-game");

    // On the heap: the game's state (its fixed arrays of windows, tables and frames) outgrew the thread's stack.
    const std::unique_ptr<AndroidState> owned = std::make_unique<AndroidState>();
    AndroidState& state = *owned;
    state.env = env;
    app->userData = &state;
    app->onAppCmd = handleAppCmd;
    // A game is played without touching the screen for minutes (benchmarks, autorun): never dim it.
    GameActivity_setWindowFlags(app->activity, GAMEACTIVITY_FLAG_KEEP_SCREEN_ON, 0);

    vd::PlatformInfo platform{vm, env, app->activity->javaGameActivity};
    if (const float scale = densityScale(app); scale > 0.0f) {
        platform.contentScale = scale;
    }
    platform.pixelsPerMm = pixelsPerMm(env, app->activity->javaGameActivity, platform.contentScale); // phase 20 (edit)
    // App-specific external storage: no permissions needed, reachable with `adb push`.
    if (app->activity->externalDataPath) {
        platform.packsDir = std::string(app->activity->externalDataPath) + "/packs";
    }
    if (app->activity->internalDataPath) {
        platform.cacheDir = app->activity->internalDataPath;
    }
    platform.appPack = installAppPack(app, env); // phase 22 (apppack): app.vdpk, from the APK
    platform.systemLanguage = systemLanguage(app); // phase 22 (pairing): the first-run screen's
    // Server session: client.cfg sits next to packs/.
    if (app->activity->externalDataPath) {
        platform.configPath = std::string(app->activity->externalDataPath) + "/client.cfg";
    }
#if !defined(VD_DIST)
    // Development launches (scripts/android.ps1, bench.ps1, soak.ps1): the intent's extras. The published build
    // (phase 22, VD_DIST) has none of them. The password only ever comes through the intent.
    platform.benchmark = intentExtra(env, app->activity->javaGameActivity, "bench");
    platform.account = intentExtra(env, app->activity->javaGameActivity, "account");
    platform.password = intentExtra(env, app->activity->javaGameActivity, "password");
    platform.realmd = intentExtra(env, app->activity->javaGameActivity, "realmd");
    platform.character = intentExtra(env, app->activity->javaGameActivity, "character");
    // Development (online tests): chat lines said in the world, then a walking route.
    platform.commands = intentExtra(env, app->activity->javaGameActivity, "commands");
    platform.walkRoute = intentExtra(env, app->activity->javaGameActivity, "walkroute");
    // Phase 9: "off" | "null" | "wav:<file>" to test without sound (default: the device).
    platform.audio = intentExtra(env, app->activity->javaGameActivity, "audio");
    // Phase 10 (benchmarks): a quality preset, a fixed world resolution, the dynamic quality on/off, the minutes of
    // a looping route.
    platform.quality = intentExtra(env, app->activity->javaGameActivity, "quality");
    platform.dynamicQuality = intentExtra(env, app->activity->javaGameActivity, "dynamicquality");
    // Phase 11 (benchmarks): a forced weather ("rain:1") and the ghost's world ("1").
    platform.weather = intentExtra(env, app->activity->javaGameActivity, "weather");
    platform.ghost = intentExtra(env, app->activity->javaGameActivity, "ghost") == "1";
    // Phase 14 (benchmarks): the ground clutter, "0" off (VD_GRASS=0) or a frillDensity (VD_GRASS_DENSITY).
    if (const std::string grass = intentExtra(env, app->activity->javaGameActivity, "grass"); !grass.empty()) {
        setenv(grass == "0" ? "VD_GRASS" : "VD_GRASS_DENSITY", grass.c_str(), 1);
    }
    if (const std::string scale = intentExtra(env, app->activity->javaGameActivity, "renderscale"); !scale.empty()) {
        platform.renderScale = std::strtof(scale.c_str(), nullptr);
    }
    if (const std::string minutes = intentExtra(env, app->activity->javaGameActivity, "benchminutes");
        !minutes.empty()) {
        platform.benchMinutes = std::strtof(minutes.c_str(), nullptr);
    }
    // Phase 14 (development): the environment switches the desktop takes from its environment, as one extra
    // ("VD_DOODAD_ANIM=0,VD_DOODAD_FADE=0": scripts/bench.ps1 -EnvSwitches).
    const std::string switches = intentExtra(env, app->activity->javaGameActivity, "env");
    for (std::string::size_type start = 0; start < switches.size();) {
        const std::string::size_type end = std::min(switches.find(',', start), switches.size());
        const std::string item = switches.substr(start, end - start);
        if (const std::string::size_type equals = item.find('='); equals != std::string::npos) {
            setenv(item.substr(0, equals).c_str(), item.substr(equals + 1).c_str(), 1);
            VD_LOG_INFO("Environment switch %s", item.c_str());
        }
        start = end + 1;
    }
#endif
    vd::haptics::start(platform); // phase 20 (assists): the touch controls' vibrations, on their own thread
    const bool initialized = state.game.init(platform);
    platform.password.assign(platform.password.size(), '\0'); // only its SRP6 hash lives on, in memory
    if (!initialized) {
        VD_LOG_ERROR("Engine init failed");
        vd::haptics::stop(); // phase 20 (assists)
        vm->DetachCurrentThread();
        return;
    }

    // Game controllers. Before the loop, so the activity's APP_CMD_START reaches Paddleboat_onStart.
    if (const Paddleboat_ErrorCode pad = Paddleboat_init(env, app->activity->javaGameActivity);
        pad == PADDLEBOAT_NO_ERROR) {
        state.paddleboat = true;
    } else {
        VD_LOG_WARN("Paddleboat_init failed (%d): no game controllers", static_cast<int>(pad));
    }
    // The glue's default filter keeps only the touchscreen; controllers' sticks must come through as well.
    android_app_set_motion_event_filter(app, motionFilter);

    while (!app->destroyRequested) {
        // Without a surface, block until the OS sends an event: no CPU/battery burned in background.
        for (;;) {
            const int timeoutMs = state.hasWindow ? 0 : -1;
            android_poll_source* source = nullptr;
            const int result = ALooper_pollOnce(timeoutMs, nullptr, nullptr, reinterpret_cast<void**>(&source));
            if (result == ALOOPER_POLL_TIMEOUT || result == ALOOPER_POLL_ERROR) {
                break;
            }
            if (source) {
                source->process(app, source);
            }
            if (app->destroyRequested) {
                break;
            }
        }
        if (app->destroyRequested) {
            break;
        }

        // Paddleboat first (connections, disconnections), then this frame's events.
        if (state.paddleboat) {
            Paddleboat_update(env);
            enableControllerAxes(state);
        }
        if (android_input_buffer* input = android_app_swap_input_buffers(app)) {
            for (uint64_t i = 0; i < input->keyEventsCount; ++i) {
                dispatchKey(state, input->keyEvents[i]);
            }
            for (uint64_t i = 0; i < input->motionEventsCount; ++i) {
                dispatchMotion(state, input->motionEvents[i]);
            }
            android_app_clear_motion_events(input);
            android_app_clear_key_events(input);
        }
        if (state.paddleboat) {
            state.game.onGamepad(readGamepad());
        }
        if (const int focus = g_audioFocusChange.exchange(kNoFocusChange); focus != kNoFocusChange) {
            // Ducked under another app's voice (navigation, a call coming in), muted when the focus is lost.
            state.game.onAudioFocus(focus > 0 ? 1.0f : (focus == -3 ? 0.3f : 0.0f));
        }
        // What the soft keyboard typed since the last frame (the glue raises the flag, the state is read here).
        if (app->textInputState) {
            app->textInputState = 0;
            GameActivity_getTextInputState(app->activity, onImeState, &state);
        }
        if (std::string link = takePendingLink(); !link.empty()) {
            state.game.onPairingLink(link); // phase 22 (pairing)
        }

        if (state.hasWindow) {
            state.game.tick();
            syncKeyboard(app, state);
        }
        if (state.game.wantsExit() && !state.finishing) {
            // Parity (popups): Exit Game, once the server logged the character out. The activity ends as if
            // the player had closed it (destroyRequested follows).
            state.finishing = true;
            GameActivity_finish(app->activity);
        }
        if (state.game.wantsRestart() && !restartGame(app, state, platform)) {
            // Phase 22 (pairing): as a start that fails (the log says why), but the activity closes.
            VD_LOG_ERROR("Engine init failed after the data arrived");
            GameActivity_finish(app->activity);
            if (state.paddleboat) {
                Paddleboat_destroy(env);
            }
            vd::haptics::stop();
            vm->DetachCurrentThread();
            return;
        }
    }

    if (state.paddleboat) {
        Paddleboat_destroy(env);
    }
    state.game.onSurfaceDestroyed();
    state.game.shutdown();
    vd::haptics::stop(); // phase 20 (assists)
    vm->DetachCurrentThread();
}
