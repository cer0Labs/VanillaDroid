// Phase 22 (pairing, ROADMAP 22.4): the game on a device without its data: the first-run screen alone
// (app/FirstRunScreens), drawn with app.vdpk; nothing else is opened. Once the data is there the platform
// starts the game over (Game::wantsRestart), and Game::init opens the packs as always.
#include "app/Game.h"

#include "app/Pairing.h"
#include "core/Config.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "pack/AppPack.h"

#include <chrono>
#include <thread>

namespace vd {

bool Game::initFirstRun(const PlatformInfo& platform) {
    m_firstRunMode = true;
    m_fakeKeyboard =
        platform.fakeKeyboard; // development (desktop): the page makes room for a phone's keyboard
    // The language: the configured one, else the system's (a new phone has no client.cfg yet).
    const Config config = platform.configPath.empty() ? Config{} : Config::load(platform.configPath);
    const char* localeOverride = VD_DEV_ENV("VD_LOCALE");
    m_locale = firstRunLocale(localeOverride ? std::string(localeOverride) : config.get("locale"),
                              platform.systemLanguage);
    VD_LOG_INFO("First run: no game data in %s: the pairing screen (%s)", m_packsDir.string().c_str(),
                m_locale.c_str());
    m_monitor.start(platform); // writes pairing.json off this thread
    if (Result<void> result = m_renderer.init(platform); !result) {
        VD_LOG_ERROR("Renderer init failed: %s", result.error().message.c_str());
        return false;
    }
    const std::filesystem::path appPack =
        platform.appPack.empty() ? m_packsDir / pack::kAppPackFile : std::filesystem::path(platform.appPack);
    if (!m_uiResources.init(m_renderer, nullptr, {}, appPack)) {
        VD_LOG_ERROR("First run: %s is missing: nothing to draw the screen with", appPack.string().c_str());
        return false;
    }
    m_gameHud.init(&m_uiResources, m_renderer.uiQuadCapacity());
    text::load({}, appPack, m_locale);
    FirstRunScreens::Config screens;
    screens.packsDir = m_packsDir;
    // The WoW's archives where `vdsetup adb send-wow` leaves them: <the app's files>/wow/Data, next to
    // packs/.
    screens.wowDir = m_packsDir.parent_path() / "wow" / "Data";
    screens.storageDir = platform.cacheDir;
    screens.writeFile = [](void* self, const std::filesystem::path& path, std::string text) {
        static_cast<Game*>(self)->m_monitor.writeFile(path, std::move(text));
    };
    screens.writer = this;
    screens.phoneBake = &m_phoneBake; // phase 22 (phonebake): "Prepare the data on this phone"
    if (const char* demo = VD_DEV_ENV("VD_PAIRING_DEMO")) {
        screens.demo = demo;
    }
    m_firstRun.init(screens);
    m_startTime = Clock::now();
    m_lastTickStart = m_startTime;
    return true;
}

void Game::tickFirstRun() {
    VD_PROFILE_ZONE("Game::tickFirstRun");
    const Clock::time_point now = Clock::now();
    m_lastTickStart = now;
    m_textInput.newFrame();
    m_uiResources.update(m_renderer);
    const RenderStats& render = m_renderer.stats();
    HudFrame hud;
    hud.width = static_cast<f32>(render.width);
    hud.height = static_cast<f32>(render.height);
    hud.dp = m_contentScale * m_uiScale;
    hud.safeArea = safeArea();
    hud.nowMs = static_cast<u32>(secondsBetween(m_startTime, now) * 1000.0);
    m_gameHud.begin(hud);
    DialogContext c{m_gameHud.context(), m_gameHud.drawList(), m_uiResources,
                    hud.width >= hud.height * 1.6f ? ScreenProfile::Wide : ScreenProfile::Square, uiArea()};
    m_firstRun.draw(c, m_textInput);
    m_gameHud.end();
    FrameParams params;
    params.timeSeconds = static_cast<f32>(secondsBetween(m_startTime, now));
    params.uiVertices = m_gameHud.vertices();
    params.uiCommands = m_gameHud.commands();
    if (!m_renderer.renderFrame(params)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(8)); // no surface yet, or being rebuilt
    }
    m_lastFrame = Clock::now();
    VD_PROFILE_FRAME();
}

void Game::onPairingLink(std::string_view uri) {
    if (m_firstRunMode) {
        m_firstRun.onLink(uri);
    } else {
        VD_LOG_INFO("Pairing link ignored: the game already has its data");
    }
}

} // namespace vd
