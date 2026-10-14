// VanillaDroid Setup's window: the home page, the language and the versions (phase 22, gui).
#include "SetupWindow.h"

#include "SetupText.h"
#include "core/Version.h"
#include "pack/ReleaseManifest.h" // phase 22 (polish)

#include <SDL3/SDL.h>

#include <algorithm>

namespace vd::setup::gui {
namespace {

std::string clientVersion(const ClientCheck& check) {
    if (check.exeVersionRead) {
        return check.exeVersion.toString();
    }
    return check.macVersion.empty() ? std::string(tr("VD_SETUP_GUI_UNKNOWN")) : check.macVersion;
}

// A card of the home page: the state of one part and the way to its page.
bool card(std::string_view id, Status status, std::string_view titleKey, const std::string& summary) {
    ImGui::PushID(id.data(), id.data() + id.size());
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::BeginChild("##card", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    statusDot(status);
    ImGui::SameLine();
    ImGui::PushFont(nullptr, kFontSize * 1.15f);
    textColored(kGold, tr(titleKey), false);
    ImGui::PopFont();
    textWrapped(summary);
    gap(0.2f);
    const bool open = button(label("VD_SETUP_GUI_OPEN", "open"));
    ImGui::EndChild();
    ImGui::PopStyleColor();
    ImGui::PopID();
    return open;
}

} // namespace

void SetupWindow::drawHome() {
    title(tr("VD_SETUP_GUI_HOME_TITLE"));
    textWrapped(tr("VD_SETUP_GUI_HOME_INTRO"));
    gap(0.6f);
    const Snapshot& s = m_snapshot;

    std::string wow;
    if (m_settings.wow.empty()) {
        wow = tr("VD_SETUP_GUI_HOME_WOW_NONE");
    } else if (!s.done || m_wowTask.running()) {
        wow = tr("VD_SETUP_GUI_HOME_CHECKING");
    } else if (s.clients.base.usable) {
        wow = trf("VD_SETUP_GUI_HOME_WOW_OK", {clientVersion(s.clients.base), s.clients.base.locale,
                                               formatBytes(s.clients.base.gameBytes())});
        if (s.clients.hasLocale) {
            wow += "\n" + trf("VD_SETUP_GUI_LOCALE_USED", {s.clients.locale.locale});
        }
    } else {
        wow = tr("VD_SETUP_GUI_HOME_WOW_BAD");
    }

    std::string packsText;
    if (m_bakeTask.running()) {
        const Task::State state = m_bakeTask.state();
        packsText = trf("VD_SETUP_GUI_HOME_BAKING",
                        {std::to_string(static_cast<int>(std::max(0.0, state.fraction) * 100.0))});
    } else if (!s.clients.base.usable) {
        packsText = tr("VD_SETUP_GUI_HOME_PACKS_NEED_WOW");
    } else if (s.packs.empty()) {
        packsText = tr("VD_SETUP_GUI_HOME_PACKS_NONE");
    } else if (s.plan.jobs.empty()) {
        packsText = trf("VD_SETUP_GUI_HOME_PACKS_OK", {formatBytes(s.packBytes)});
    } else {
        packsText = trf("VD_SETUP_GUI_HOME_PACKS_TODO", {std::to_string(s.plan.jobs.size())});
    }

    const std::string phone = m_server ? trf("VD_SETUP_GUI_HOME_PHONE_ON", {m_serveHost})
                                       : std::string(tr("VD_SETUP_GUI_HOME_PHONE_OFF"));
    const std::string adb = s.adb.empty() ? std::string(tr("VD_SETUP_GUI_HOME_ADB_MISSING"))
                                          : std::string(tr("VD_SETUP_GUI_HOME_ADB_FOUND"));

    if (ImGui::BeginTable("##cards", 2, ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_PadOuterX)) {
        ImGui::TableNextColumn();
        if (card("wow", pageStatus(Page::Wow), "VD_SETUP_GUI_NAV_WOW", wow)) {
            goTo(Page::Wow);
        }
        ImGui::TableNextColumn();
        if (card("bake", pageStatus(Page::Bake), "VD_SETUP_GUI_NAV_BAKE", packsText)) {
            goTo(Page::Bake);
        }
        ImGui::TableNextColumn();
        if (card("phone", pageStatus(Page::Phone), "VD_SETUP_GUI_NAV_PHONE", phone)) {
            goTo(Page::Phone);
        }
        ImGui::TableNextColumn();
        if (card("adb", pageStatus(Page::Adb), "VD_SETUP_GUI_NAV_ADB", adb)) {
            goTo(Page::Adb);
        }
        ImGui::EndTable();
    }
    gap(0.6f);
    if (ImGui::TextLink(label("VD_SETUP_GUI_WIZARD_AGAIN", "wizard").c_str())) {
        m_wizard = true;
        goTo(Page::Language);
    }
}

void SetupWindow::drawLanguage() {
    title(tr("VD_SETUP_GUI_LANGUAGE_TITLE"));
    textWrapped(tr("VD_SETUP_GUI_LANGUAGE_INTRO"));
    gap(0.8f);
    text(tr("VD_SETUP_GUI_LANGUAGE_CHOOSE"));
    gap(0.2f);
    languageChooser(true);
    if (languageLocked()) {
        gap(0.3f);
        textMuted(tr("VD_SETUP_GUI_LANGUAGE_LOCKED"));
    }
}

void SetupWindow::drawAbout() {
    title(tr("VD_SETUP_GUI_ABOUT_TITLE"));
    const Snapshot& s = m_snapshot;

    heading(tr("VD_SETUP_GUI_ABOUT_VERSIONS"));
    if (ImGui::BeginTable("##versions", 2, ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("##name", ImGuiTableColumnFlags_WidthFixed);
        ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        auto row = [](std::string_view name, const std::string& value) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            textColored(kMuted, name, false);
            ImGui::TableNextColumn();
            textWrapped(value);
        };
        row(tr("VD_SETUP_GUI_ABOUT_SETUP"), kVersion);
        if (s.apk.empty()) {
            row(tr("VD_SETUP_GUI_ABOUT_APP"), std::string(tr("VD_SETUP_GUI_ABOUT_APP_NONE")));
        } else if (s.apkRead) {
            row(tr("VD_SETUP_GUI_ABOUT_APP"),
                trf("VD_SETUP_GUI_ABOUT_APP_VERSION",
                    {s.apkInfo.versionName, s.apkInfo.package, utf8FromPath(s.apk)}));
        } else {
            row(tr("VD_SETUP_GUI_ABOUT_APP"),
                trf("VD_SETUP_STATUS_APK_BAD", {utf8FromPath(s.apk), s.apkError}));
        }
        row(tr("VD_SETUP_GUI_ABOUT_APP_ID"), m_settings.appId);
        row(tr("VD_SETUP_GUI_ABOUT_PACKS"),
            s.bakedBy.empty() ? std::string(tr("VD_SETUP_GUI_HOME_PACKS_NONE"))
                              : trf("VD_SETUP_GUI_ABOUT_PACKS_BY", {s.bakedBy, s.bakedLocale}));
        row(tr("VD_SETUP_GUI_ABOUT_BAKER"),
            s.baker.empty() ? std::string(tr("VD_SETUP_GUI_BAKE_NO_BAKER")) : utf8FromPath(s.baker));
        row(tr("VD_SETUP_GUI_ABOUT_ADB"),
            s.adb.empty() ? std::string(tr("VD_SETUP_GUI_ADB_NOT_FOUND")) : utf8FromPath(s.adb));
        ImGui::EndTable();
    }
    gap(0.3f);
    if (button(label("VD_SETUP_GUI_CHOOSE_APK", "apk"), !m_dialogOpen)) {
        openFileDialog(DialogPurpose::Apk, s.apk.empty() ? executableDir() : s.apk.parent_path());
    }

    heading(tr("VD_SETUP_GUI_ABOUT_DATA"));
    textWrapped(tr("VD_SETUP_GUI_ABOUT_DATA_TEXT"));
    selectableText(utf8FromPath(m_dataDir), "datadir");
    ImGui::SameLine();
    if (button(label("VD_SETUP_GUI_OPEN_FOLDER", "opendata"))) {
        std::error_code error;
        std::filesystem::create_directories(m_dataDir, error);
        openPath(utf8FromPath(m_dataDir));
    }

    heading(tr("VD_SETUP_GUI_UPDATES_TITLE"));
    // Phase 22 (polish): the latest check's result in the words of `vdsetup status` (updateStatusMessage,
    // with the window's wording), and a check now. Installing is `vdsetup update`'s.
    textWrapped(tr(m_updateAuto ? "VD_SETUP_GUI_UPDATES_TEXT" : "VD_SETUP_GUI_UPDATES_DEVELOPMENT"));
    gap(0.2f);
    if (m_updateTask.running()) {
        const Task::State state = m_updateTask.state();
        spinner();
        ImGui::SameLine();
        text(state.text.empty() ? tr("VD_SETUP_UPDATE_CHECKING") : std::string_view(state.text));
    } else {
        const Message status = updateStatusMessage(m_updateCache, m_updateUrl);
        message(status);
        if (!m_updateError.empty()) {
            messageText(Message::Level::Error, m_updateError);
        }
        if (status.key == "VD_SETUP_STATUS_UPDATE_AVAILABLE") {
            if (!m_updateCache.notes.empty()) {
                ImGui::TextLinkOpenURL(label("VD_SETUP_GUI_UPDATES_NOTES", "notes").c_str(),
                                       m_updateCache.notes.c_str());
                ImGui::SameLine();
            }
            const std::string page(pack::kReleasePageUrl);
            ImGui::TextLinkOpenURL(label("VD_SETUP_GUI_UPDATES_PAGE", "page").c_str(), page.c_str());
            textColored(kMuted, trf("VD_SETUP_GUI_COMMAND", {std::string(tr("VD_SETUP_GUI_UPDATES_CMD"))}));
        }
    }
    gap(0.2f);
    if (button(label("VD_SETUP_GUI_UPDATES_CHECK", "updates"), !m_updateTask.running())) {
        checkUpdates();
    }

    heading(tr("VD_SETUP_GUI_LICENSES"));
    textWrapped(tr("VD_SETUP_GUI_LICENSES_TEXT"));
    // Phase 22 (polish): one notices file for everything published (THIRD_PARTY_NOTICES.md at the repository's root).
    std::filesystem::path notices = executableDir() / "THIRD_PARTY_NOTICES.md";
    std::error_code error;
    if (!std::filesystem::is_regular_file(notices, error)) {
        notices = executableDir().parent_path() / "Resources" / "THIRD_PARTY_NOTICES.md"; // a macOS bundle
    }
    if (button(label("VD_SETUP_GUI_LICENSES_OPEN", "licenses"),
               std::filesystem::is_regular_file(notices, error))) {
        openPath(utf8FromPath(notices));
    }
}

void SetupWindow::checkUpdates() {
    const std::filesystem::path data = m_dataDir;
    const std::string url = m_updateUrl;
    m_updateTask.start([this, data, url](const Operation& operation) -> Task::Apply {
        auto checked = std::make_shared<Result<UpdateCheck>>(checkForUpdate(data, url, operation, 30));
        auto cache = std::make_shared<UpdateCache>(loadUpdateCache(data));
        return [this, checked, cache]() {
            m_updateCache = *cache;
            m_updateError = *checked ? std::string() : checked->error().message;
        };
    });
}

} // namespace vd::setup::gui
