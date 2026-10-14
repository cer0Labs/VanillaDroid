#include "SetupWindow.h"

#include "SetupText.h"
#include "core/Version.h"
#include "pack/PackManifest.h"
#include "pack/ReleaseManifest.h" // phase 22 (polish)

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace vd::setup::gui {
namespace {

// The folder and file dialogs answer on another thread, maybe after the window is gone: their answers wait
// here, in storage that outlives it.
struct DialogMailbox {
    std::mutex mutex;
    bool open = false;
    std::vector<std::pair<int, std::filesystem::path>> answers; // purpose, path
    std::string error;
};
DialogMailbox& mailbox() {
    static DialogMailbox box;
    return box;
}

void SDLCALL dialogDone(void* userdata, const char* const* files, int) {
    const int purpose = static_cast<int>(reinterpret_cast<std::intptr_t>(userdata));
    {
        std::lock_guard<std::mutex> lock(mailbox().mutex);
        mailbox().open = false;
        if (!files) {
            mailbox().error = SDL_GetError();
        } else if (files[0]) {
            mailbox().answers.emplace_back(purpose, pathFromUtf8(files[0]));
        }
    }
    wakeWindow();
}

// The folder itself when it exists, else its nearest existing parent (where a dialog opens, where space is
// measured).
std::filesystem::path existingFolder(std::filesystem::path path) {
    std::error_code error;
    while (!path.empty() && !std::filesystem::is_directory(path, error)) {
        const std::filesystem::path parent = path.parent_path();
        if (parent == path) {
            return {};
        }
        path = parent;
    }
    return path;
}

} // namespace

SetupWindow::SetupWindow(SDL_Window* window, WindowOptions options)
    : m_window(window), m_options(std::move(options)) {
    m_dataDir = dataDir(m_options.dataDir);
    m_settingsFile = m_dataDir / kSettingsFile;
    if (Result<Settings> loaded = Settings::load(m_settingsFile); loaded) {
        m_settings = std::move(loaded.value());
    } else {
        m_settingsError = loaded.error().message;
    }
    m_locale = loadSetupTexts(m_options.language.empty() ? m_settings.language : m_options.language,
                              TextSurface::Window);
    if (!m_options.language.empty()) {
        m_languageOverride = m_locale;
    }
    SDL_SetWindowTitle(m_window, std::string(tr("VD_SETUP_GUI_WINDOW_TITLE")).c_str());

    const std::string wow = utf8FromPath(m_settings.wow);
    std::snprintf(m_wowInput, sizeof(m_wowInput), "%s", wow.c_str());
    const std::string locale = utf8FromPath(m_settings.localeClient);
    std::snprintf(m_localeInput, sizeof(m_localeInput), "%s", locale.c_str());

    m_wizard = wizardNeeded(m_settings);
    m_page = m_wizard ? Page::Language : Page::Home;
    // --page: a page to open (screenshots, tests); "wizard-<step>" opens the guide there.
    struct Named {
        const char* name;
        Page page;
    };
    static constexpr Named kPages[] = {
        {"home", Page::Home},   {"language", Page::Language}, {"wow", Page::Wow},    {"bake", Page::Bake},
        {"phone", Page::Phone}, {"adb", Page::Adb},           {"about", Page::About}};
    std::string page = m_options.page;
    if (page.rfind("wizard", 0) == 0) {
        m_wizard = true;
        page = page.size() > 7 ? page.substr(7) : "language";
    } else if (!page.empty()) {
        m_wizard = false;
    }
    for (const Named& named : kPages) {
        if (page == named.name) {
            m_page = named.page;
        }
    }
    if (m_wizard) {
        m_wizardStep = m_page == Page::Wow     ? WizardStep::Wow
                       : m_page == Page::Bake  ? WizardStep::Bake
                       : m_page == Page::Phone ? WizardStep::Phone
                                               : WizardStep::Language;
        goTo(m_page == Page::Home ? Page::Language : m_page);
    }

    m_addresses = localAddresses();
    if (const LocalAddress* best = pickLanAddress(m_addresses)) {
        m_addressChoice = static_cast<int>(best - m_addresses.data());
    }
    refresh(true);
    // Phase 22 (polish): updates, as `vdsetup status` looks for them: a distribution build by itself (at most
    // every few hours: update.json), a development one only with VD_UPDATE_URL; both when asked.
    m_updateUrl = updateManifestUrl({});
    m_updateAuto = kDistBuild || m_updateUrl != pack::kReleaseManifestUrl;
    m_updateCache = loadUpdateCache(m_dataDir);
    if (m_updateAuto && !m_updateCache.fresh(unixNow(), m_updateUrl)) {
        checkUpdates();
    }
    if (!m_options.wow.empty()) {
        dialogChosen(DialogPurpose::Wow, m_options.wow);
    } else if (!m_options.localeClient.empty()) {
        dialogChosen(DialogPurpose::LocaleClient, m_options.localeClient);
    }
}

SetupWindow::~SetupWindow() {
    shutdown();
}

void SetupWindow::setLogo(SDL_Texture* texture, f32 width, f32 height) {
    m_logo = texture;
    m_logoWidth = width;
    m_logoHeight = height;
}

void SetupWindow::shutdown() {
    if (m_server) {
        m_server->stop();
        m_server.reset();
    }
    m_bakeTask.stop();
    m_adbTask.stop();
    m_wowTask.stop();
    m_refreshTask.stop();
    m_updateTask.stop(); // phase 22 (polish)
}

void SetupWindow::saveSettings() {
    if (Result<void> saved = m_settings.save(m_settingsFile); !saved) {
        m_settingsError = saved.error().message;
    }
}

std::filesystem::path SetupWindow::packs() const {
    return packsDir(m_settings, m_dataDir);
}

bool SetupWindow::busy() const {
    return m_bakeTask.running() || m_wowTask.running() || m_adbTask.running();
}

bool SetupWindow::languageLocked() const {
    // Other threads format texts while these run: the table must not change under them.
    return busy() || m_refreshTask.running() || m_updateTask.running() || m_server != nullptr;
}

SetupFacts SetupWindow::facts() const {
    SetupFacts facts;
    facts.wowUsable = m_snapshot.clients.base.usable;
    facts.packsReady = facts.wowUsable && !m_snapshot.packs.empty() && m_snapshot.plan.jobs.empty();
    facts.busy = busy() || m_refreshTask.running();
    return facts;
}

std::string SetupWindow::runningSummary() const {
    std::vector<std::string> parts;
    if (m_bakeTask.running()) {
        parts.emplace_back(tr("VD_SETUP_GUI_RUNNING_BAKE"));
    }
    if (m_adbTask.running()) {
        parts.emplace_back(tr("VD_SETUP_GUI_RUNNING_ADB"));
    }
    if (m_server) {
        parts.emplace_back(tr("VD_SETUP_GUI_RUNNING_SERVER"));
    }
    std::string text;
    for (const std::string& part : parts) {
        text += (text.empty() ? "" : ", ") + part;
    }
    return text;
}

void SetupWindow::refresh(bool checkClientsAgain) {
    if (m_refreshTask.running()) {
        m_refreshAgain = true;
        m_refreshAgainClients = m_refreshAgainClients || checkClientsAgain;
        return;
    }
    const Settings settings = m_settings;
    const std::filesystem::path packsFolder = packs();
    const std::filesystem::path data = m_dataDir;
    const bool recheck = checkClientsAgain || !m_snapshot.clientsChecked;
    const Clients previous = m_snapshot.clients;
    m_refreshTask.start([=, this](const Operation&) -> Task::Apply {
        auto snapshot = std::make_shared<Snapshot>();
        snapshot->wow = settings.wow;
        snapshot->clients = recheck ? checkClients(settings) : previous;
        snapshot->clientsChecked = true;
        if (snapshot->clients.base.usable) {
            snapshot->plan = planFor(settings, packsFolder, snapshot->clients, false, {});
        }
        snapshot->packs = listPackFiles(packsFolder);
        for (const PackFile& file : snapshot->packs) {
            snapshot->packBytes += file.size;
        }
        if (Result<pack::PackManifest> manifest = pack::PackManifest::load(packsFolder);
            manifest && !manifest.value().packs.empty()) {
            snapshot->bakedBy = manifest.value().bakedBy;
            snapshot->bakedLocale = manifest.value().clientLocale;
            for (const std::string& overlay : manifest.value().overlays) {
                snapshot->bakedLocale += " + " + overlay;
            }
        }
        std::error_code error;
        if (const std::filesystem::path existing = existingFolder(packsFolder); !existing.empty()) {
            const std::filesystem::space_info space = std::filesystem::space(existing, error);
            snapshot->freeKnown = !error;
            snapshot->freeSpace = error ? 0 : space.available;
        }
        snapshot->apk = findApk(settings.apk, executableDir());
        if (!snapshot->apk.empty()) {
            if (Result<ApkInfo> info = readApkInfo(snapshot->apk); info) {
                snapshot->apkRead = true;
                snapshot->apkInfo = info.value();
            } else {
                snapshot->apkError = info.error().message;
            }
        }
        snapshot->adb = findAdb(settings.adb, data);
        snapshot->baker = findBaker(executableDir());
        snapshot->done = true;
        return [this, snapshot]() {
            m_snapshot = std::move(*snapshot);
            // A folder chosen by hand that is now the settings' WoW: the page shows the snapshot again.
            if (m_candidate && m_candidate->usable && m_candidate->folder == m_snapshot.wow) {
                m_candidate.reset();
            }
            updateOptionsPlan();
            if (m_refreshAgain) {
                const bool clients = m_refreshAgainClients;
                m_refreshAgain = false;
                m_refreshAgainClients = false;
                refresh(clients);
            }
        };
    });
}

bool SetupWindow::update() {
    bool changed = false;
    if (m_pendingLanguage && !languageLocked()) {
        m_settings.language = *m_pendingLanguage;
        saveSettings();
        m_locale = loadSetupTexts(*m_pendingLanguage, TextSurface::Window);
        m_pendingLanguage.reset();
        m_languageOverride.clear();
        SDL_SetWindowTitle(m_window, std::string(tr("VD_SETUP_GUI_WINDOW_TITLE")).c_str());
        refresh(true); // phase 22 (polish): the checks' lines hold sizes written in their language
        changed = true;
    }
    changed = m_refreshTask.poll() || changed;
    changed = m_wowTask.poll() || changed;
    changed = m_bakeTask.poll() || changed;
    changed = m_adbTask.poll() || changed;
    changed = m_updateTask.poll() || changed; // phase 22 (polish)

    std::vector<std::pair<int, std::filesystem::path>> answers;
    {
        std::lock_guard<std::mutex> lock(mailbox().mutex);
        answers.swap(mailbox().answers);
        m_dialogOpen = mailbox().open;
        if (!mailbox().error.empty()) {
            m_dialogError = mailbox().error;
            mailbox().error.clear();
        }
    }
    for (const auto& [purpose, path] : answers) {
        dialogChosen(static_cast<DialogPurpose>(purpose), path);
        changed = true;
    }

    std::vector<ServeEvent> events;
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        events.swap(m_events);
    }
    for (const ServeEvent& event : events) {
        serverEvent(event);
        changed = true;
    }
    m_phones.prune(secondsBetween(m_start, Clock::now()), 60.0);
    return changed;
}

bool SetupWindow::animating() const {
    // Spinners and the cursor of a text field; a download's progress wakes the window by itself.
    return busy() || m_refreshTask.running() || m_updateTask.running() || m_serverPreparing ||
           ImGui::GetIO().WantTextInput;
}

void SetupWindow::dropped(const std::filesystem::path& path) {
    if (m_page != Page::Wow || m_wowTask.running() || m_bakeTask.running()) {
        return;
    }
    dialogChosen(DialogPurpose::Wow, path);
}

bool SetupWindow::requestClose() {
    if (m_closeConfirmed) {
        return true;
    }
    const bool downloading = std::any_of(m_phones.phones().begin(), m_phones.phones().end(),
                                         [](const PhoneActivity& p) { return !p.transfers.empty(); });
    if (m_bakeTask.running() || m_adbTask.running() || downloading) {
        m_closeQuestion = true;
        return false;
    }
    return true;
}

void SetupWindow::openFolderDialog(DialogPurpose purpose, const std::filesystem::path& start) {
    {
        std::lock_guard<std::mutex> lock(mailbox().mutex);
        if (mailbox().open) {
            return;
        }
        mailbox().open = true;
    }
    m_dialogOpen = true;
    m_dialogError.clear();
    const std::string location = utf8FromPath(existingFolder(start));
    SDL_ShowOpenFolderDialog(dialogDone, reinterpret_cast<void*>(static_cast<std::intptr_t>(purpose)),
                             m_window, location.empty() ? nullptr : location.c_str(), false);
}

void SetupWindow::openFileDialog(DialogPurpose purpose, const std::filesystem::path& start) {
    {
        std::lock_guard<std::mutex> lock(mailbox().mutex);
        if (mailbox().open) {
            return;
        }
        mailbox().open = true;
    }
    m_dialogOpen = true;
    m_dialogError.clear();
    const std::string location = utf8FromPath(existingFolder(start));
    static const SDL_DialogFileFilter kApk[] = {{"APK", "apk"}};
#if defined(_WIN32)
    static const SDL_DialogFileFilter kProgram[] = {{"adb", "exe"}};
    const int programFilters = 1;
#else
    static const SDL_DialogFileFilter kProgram[] = {{"adb", "*"}};
    const int programFilters = 0;
#endif
    const bool apk = purpose == DialogPurpose::Apk;
    SDL_ShowOpenFileDialog(dialogDone, reinterpret_cast<void*>(static_cast<std::intptr_t>(purpose)), m_window,
                           apk ? kApk : kProgram, apk ? 1 : programFilters,
                           location.empty() ? nullptr : location.c_str(), false);
}

void SetupWindow::dialogChosen(DialogPurpose purpose, const std::filesystem::path& path) {
    const std::string text = utf8FromPath(path);
    switch (purpose) {
    case DialogPurpose::Wow:
        std::snprintf(m_wowInput, sizeof(m_wowInput), "%s", text.c_str());
        checkWow(path);
        break;
    case DialogPurpose::LocaleClient:
        std::snprintf(m_localeInput, sizeof(m_localeInput), "%s", text.c_str());
        checkLocaleClient(path);
        break;
    case DialogPurpose::Packs:
        m_settings.packs = path;
        saveSettings();
        m_bakeOutcome = {};
        refresh(false);
        break;
    case DialogPurpose::Adb:
        m_settings.adb = path;
        saveSettings();
        refresh(false);
        break;
    case DialogPurpose::Apk:
        m_settings.apk = path;
        saveSettings();
        refresh(false);
        break;
    }
}

// --- Frame
// -------------------------------------------------------------------------------------------------------

void SetupWindow::goTo(Page page) {
    m_page = page;
    if (m_wizard) {
        m_wizardStep = page == Page::Wow     ? WizardStep::Wow
                       : page == Page::Bake  ? WizardStep::Bake
                       : page == Page::Phone ? WizardStep::Phone
                                             : WizardStep::Language;
    }
    if (page == Page::Bake && !busy()) {
        refresh(false); // the packs may have changed outside (another bake, a copy)
    }
}

Status SetupWindow::pageStatus(Page page) const {
    const Snapshot& s = m_snapshot;
    switch (page) {
    case Page::Wow:
        if (m_wowTask.running() || (!s.done && !m_settings.wow.empty())) {
            return Status::Busy;
        }
        if (m_settings.wow.empty()) {
            return Status::None;
        }
        return s.clients.base.usable ? (s.clients.localeSkipped ? Status::Warning : Status::Ok)
                                     : Status::Error;
    case Page::Bake:
        if (m_bakeTask.running()) {
            return Status::Busy;
        }
        if (!s.clients.base.usable) {
            return Status::None;
        }
        if (!m_bakeOutcome.failed.empty() || !m_bakeOutcome.error.empty()) {
            return Status::Error;
        }
        return s.packs.empty() ? Status::None : s.plan.jobs.empty() ? Status::Ok : Status::Warning;
    case Page::Phone:
        if (!m_server) {
            return Status::None;
        }
        return m_serverPreparing ? Status::Busy : m_serverReady ? Status::Ok : Status::Warning;
    case Page::Adb:
        if (m_adbTask.running()) {
            return Status::Busy;
        }
        return s.adb.empty() ? Status::None : Status::Ok;
    case Page::About: // phase 22 (polish): a newer release waits there
        return updateStatusMessage(m_updateCache, m_updateUrl).level == Message::Level::Warning
                   ? Status::Warning
                   : Status::None;
    case Page::Home:
    case Page::Language: break;
    }
    return Status::None;
}

void SetupWindow::navItem(Page page, std::string_view key, Status status) {
    const f32 height = ImGui::GetTextLineHeight() * 2.0f;
    ImGui::PushID(static_cast<int>(page));
    const bool selected = m_page == page;
    if (ImGui::Selectable("##nav", selected, ImGuiSelectableFlags_None, ImVec2(0, height))) {
        goTo(page);
    }
    const ImVec2 min = ImGui::GetItemRectMin();
    const ImVec2 max = ImGui::GetItemRectMax();
    ImDrawList* draw = ImGui::GetWindowDrawList();
    const f32 line = ImGui::GetTextLineHeight();
    const f32 centreY = (min.y + max.y) * 0.5f;
    if (selected) {
        draw->AddRectFilled(min, ImVec2(min.x + 3.0f * ImGui::GetStyle().FontScaleDpi, max.y),
                            ImGui::GetColorU32(kGold));
    }
    if (status != Status::None) {
        const ImVec2 centre(min.x + line * 0.75f, centreY);
        const ImU32 colour = ImGui::GetColorU32(statusColour(status));
        if (status == Status::Busy) {
            const f32 start = static_cast<f32>(ImGui::GetTime()) * 6.0f;
            draw->PathArcTo(centre, line * 0.26f, start, start + 4.2f, 16);
            draw->PathStroke(colour, line * 0.12f);
        } else {
            draw->AddCircleFilled(centre, line * 0.26f, colour, 16);
        }
    }
    const std::string_view name = tr(key);
    draw->AddText(ImVec2(min.x + line * 1.5f, centreY - line * 0.5f),
                  ImGui::GetColorU32(selected ? kGold : kText), name.data(), name.data() + name.size());
    ImGui::PopID();
}

void SetupWindow::drawSidebar(f32 width) {
    const f32 scale = ImGui::GetStyle().FontScaleDpi;
    if (m_logo && m_logoWidth > 0) {
        const f32 side = 96.0f * scale;
        ImGui::SetCursorPosX((width - side) * 0.5f);
        ImGui::Image(ImTextureRef(static_cast<ImTextureID>(reinterpret_cast<std::intptr_t>(m_logo))),
                     ImVec2(side, side * m_logoHeight / m_logoWidth));
    }
    const std::string_view name = tr("VD_SETUP_GUI_WINDOW_TITLE");
    ImGui::PushFont(nullptr, kFontSize * 1.25f);
    const f32 nameWidth = ImGui::CalcTextSize(name.data(), name.data() + name.size()).x;
    ImGui::SetCursorPosX(std::max(ImGui::GetStyle().WindowPadding.x, (width - nameWidth) * 0.5f));
    textColored(kGold, name, false);
    ImGui::PopFont();
    const std::string version = trf("VD_SETUP_GUI_VERSION", {kVersion});
    ImGui::SetCursorPosX(
        std::max(ImGui::GetStyle().WindowPadding.x, (width - ImGui::CalcTextSize(version.c_str()).x) * 0.5f));
    textColored(kMuted, version, false);
    gap(0.5f);
    ImGui::Separator();
    gap(0.3f);

    if (m_wizard) {
        static constexpr const char* kSteps[] = {"VD_SETUP_GUI_NAV_LANGUAGE", "VD_SETUP_GUI_NAV_WOW",
                                                 "VD_SETUP_GUI_NAV_BAKE", "VD_SETUP_GUI_NAV_PHONE"};
        const SetupFacts now = facts();
        for (u32 i = 0; i < kWizardSteps; ++i) {
            const auto step = static_cast<WizardStep>(i);
            const bool current = step == m_wizardStep;
            const bool done = static_cast<u32>(m_wizardStep) > i ||
                              (step == WizardStep::Wow && now.wowUsable) ||
                              (step == WizardStep::Bake && now.packsReady);
            ImGui::PushStyleColor(ImGuiCol_Text, current ? kGold : done ? kText : kMuted);
            statusDot(current ? (busy() ? Status::Busy : Status::Warning) : done ? Status::Ok : Status::None);
            ImGui::SameLine();
            text(trf("VD_SETUP_GUI_WIZARD_ITEM", {std::to_string(i + 1), std::string(tr(kSteps[i]))}));
            ImGui::PopStyleColor();
            gap(0.2f);
        }
    } else {
        navItem(Page::Home, "VD_SETUP_GUI_NAV_HOME", Status::None);
        navItem(Page::Wow, "VD_SETUP_GUI_NAV_WOW", pageStatus(Page::Wow));
        navItem(Page::Bake, "VD_SETUP_GUI_NAV_BAKE", pageStatus(Page::Bake));
        navItem(Page::Phone, "VD_SETUP_GUI_NAV_PHONE", pageStatus(Page::Phone));
        navItem(Page::Adb, "VD_SETUP_GUI_NAV_ADB", pageStatus(Page::Adb));
        navItem(Page::About, "VD_SETUP_GUI_NAV_ABOUT", pageStatus(Page::About));
    }

    // The language, at the bottom.
    const f32 bottom = ImGui::GetWindowHeight() - ImGui::GetStyle().WindowPadding.y -
                       ImGui::GetFrameHeightWithSpacing() - ImGui::GetTextLineHeightWithSpacing();
    if (ImGui::GetCursorPosY() < bottom) {
        ImGui::SetCursorPosY(bottom);
    }
    textMuted(tr("VD_SETUP_GUI_LANGUAGE_LABEL"));
    languageChooser(false);
}

void SetupWindow::languageChooser(bool large) {
    const std::vector<std::string> locales = setupLocales();
    // Phase 22 (polish): always the language the window is in (a change applies on the next frame), as "the
    // system's" when that is how it was chosen; never a setting that did not load.
    const std::string chosen = chooserLanguage(m_locale, m_settings.language, !m_languageOverride.empty());
    auto nameOf = [&](const std::string& locale) {
        return locale.empty() ? trf("VD_SETUP_GUI_LANGUAGE_SYSTEM", {setupLanguageName(systemSetupLocale())})
                              : setupLanguageName(locale);
    };
    const bool locked = languageLocked();
    ImGui::BeginDisabled(locked);
    if (large) {
        std::vector<std::string> options = {std::string()};
        options.insert(options.end(), locales.begin(), locales.end());
        for (const std::string& option : options) {
            const std::string text = nameOf(option) + "##lang" + option;
            if (ImGui::RadioButton(text.c_str(), chosen == option) && chosen != option) {
                m_pendingLanguage = option;
            }
        }
    } else {
        ImGui::SetNextItemWidth(-1.0f);
        if (ImGui::BeginCombo("##language", nameOf(chosen).c_str())) {
            std::vector<std::string> options = {std::string()};
            options.insert(options.end(), locales.begin(), locales.end());
            for (const std::string& option : options) {
                const std::string text = nameOf(option) + "##lang" + option;
                if (ImGui::Selectable(text.c_str(), chosen == option) && chosen != option) {
                    m_pendingLanguage = option;
                }
            }
            ImGui::EndCombo();
        }
    }
    ImGui::EndDisabled();
    if (locked && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::SetTooltip("%s", std::string(tr("VD_SETUP_GUI_LANGUAGE_LOCKED")).c_str());
    }
}

void SetupWindow::drawPage() {
    if (!m_settingsError.empty()) {
        messageText(Message::Level::Warning, trf("VD_SETUP_SETTINGS_DAMAGED", {m_settingsError}));
        gap(0.4f);
    }
    if (!m_dialogError.empty()) {
        messageText(Message::Level::Error, trf("VD_SETUP_GUI_DIALOG_FAILED", {m_dialogError}));
        gap(0.4f);
    }
    switch (m_page) {
    case Page::Home: drawHome(); break;
    case Page::Language: drawLanguage(); break;
    case Page::Wow: drawWow(); break;
    case Page::Bake: drawBake(); break;
    case Page::Phone: drawPhone(); break;
    case Page::Adb: drawAdb(); break;
    case Page::About: drawAbout(); break;
    }
}

void SetupWindow::drawWizardBar() {
    const SetupFacts now = facts();
    const ImGuiStyle& style = ImGui::GetStyle();
    const f32 right = ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x;
    if (button(label("VD_SETUP_GUI_BACK", "back"), canGoBack(m_wizardStep, now))) {
        goTo(m_wizardStep == WizardStep::Wow    ? Page::Language
             : m_wizardStep == WizardStep::Bake ? Page::Wow
                                                : Page::Bake);
    }
    const bool last = isLastStep(m_wizardStep);
    const std::string next = label(last ? "VD_SETUP_GUI_FINISH" : "VD_SETUP_GUI_NEXT", "next");
    const std::string skip = label("VD_SETUP_GUI_SKIP_WIZARD", "skip");
    const f32 nextWidth = ImGui::CalcTextSize(next.c_str(), nullptr, true).x + style.FramePadding.x * 3.2f;
    const f32 skipWidth = ImGui::CalcTextSize(skip.c_str(), nullptr, true).x + style.FramePadding.x * 2.0f;
    const bool may = canGoNext(m_wizardStep, now);
    // Why Next waits, between the buttons.
    std::string_view hint;
    if (!may && !now.busy) {
        hint = m_wizardStep == WizardStep::Wow ? tr("VD_SETUP_GUI_WIZARD_HINT_WOW")
                                               : tr("VD_SETUP_GUI_WIZARD_HINT_BAKE");
    }
    if (!hint.empty()) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        textColored(kMuted, hint, false);
    }
    ImGui::SameLine(right - nextWidth - skipWidth - style.ItemSpacing.x);
    if (button(skip, !now.busy)) {
        m_wizard = false;
        goTo(Page::Home);
    }
    ImGui::SameLine();
    if (primaryButton(next, may)) {
        if (last) {
            m_wizard = false;
            goTo(Page::Home);
        } else {
            goTo(m_wizardStep == WizardStep::Language ? Page::Wow
                 : m_wizardStep == WizardStep::Wow    ? Page::Bake
                                                      : Page::Phone);
        }
    }
}

void SetupWindow::drawCloseQuestion() {
    const std::string titleText = label("VD_SETUP_GUI_CLOSE_TITLE", "close");
    if (m_closeQuestion) {
        ImGui::OpenPopup(titleText.c_str());
        m_closeQuestion = false;
    }
    const ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 28.0f, 0));
    if (ImGui::BeginPopupModal(titleText.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        textWrapped(trf("VD_SETUP_GUI_CLOSE_TEXT", {runningSummary()}));
        gap(0.5f);
        if (primaryButton(label("VD_SETUP_GUI_CLOSE_YES", "yes"))) {
            m_closeConfirmed = true;
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button(label("VD_SETUP_GUI_CLOSE_NO", "no"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void SetupWindow::draw() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(viewport->WorkPos);
    ImGui::SetNextWindowSize(viewport->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##setup", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const f32 scale = ImGui::GetStyle().FontScaleDpi;
    const f32 sidebar = 236.0f * scale;

    ImGui::PushStyleColor(ImGuiCol_ChildBg, kSidebar);
    ImGui::BeginChild("##sidebar", ImVec2(sidebar, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                      ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    drawSidebar(sidebar);
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::SameLine(0, 0);

    ImGui::BeginGroup();
    const f32 bar =
        m_wizard ? ImGui::GetFrameHeight() * 1.3f + ImGui::GetStyle().WindowPadding.y * 2.0f : 0.0f;
    ImGui::BeginChild("##page", ImVec2(0, -bar), ImGuiChildFlags_AlwaysUseWindowPadding);
    drawPage();
    ImGui::EndChild();
    if (m_wizard) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
        ImGui::BeginChild("##wizard", ImVec2(0, 0), ImGuiChildFlags_AlwaysUseWindowPadding,
                          ImGuiWindowFlags_NoScrollbar);
        drawWizardBar();
        ImGui::EndChild();
        ImGui::PopStyleColor();
    }
    ImGui::EndGroup();

    drawCloseQuestion();
    ImGui::End();
}

} // namespace vd::setup::gui
