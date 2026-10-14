// VanillaDroid Setup's window: the WoW 1.12.1 folder, its check, the archives that are not official and the
// second client in another language (phase 22, gui; vdsetup wow).
#include "SetupWindow.h"

#include <algorithm>
#include <cstdio>

namespace vd::setup::gui {
namespace {

bool listed(const std::vector<std::string>& names, std::string_view name) {
    return std::any_of(names.begin(), names.end(),
                       [&](const std::string& n) { return equalsNoCase(n, name); });
}

std::string_view archiveNote(const ArchiveInfo& archive) {
    switch (archive.state) {
    case ArchiveState::NotMpq: return tr("VD_SETUP_GUI_ARCHIVE_DAMAGED");
    case ArchiveState::Truncated: return tr("VD_SETUP_GUI_ARCHIVE_TRUNCATED");
    case ArchiveState::Unreadable: return tr("VD_SETUP_GUI_ARCHIVE_UNREADABLE");
    case ArchiveState::Ok: break;
    }
    switch (archive.kind) {
    case ArchiveKind::Game: return tr("VD_SETUP_GUI_ARCHIVE_OFFICIAL");
    case ArchiveKind::Launcher: return tr("VD_SETUP_GUI_ARCHIVE_LAUNCHER");
    case ArchiveKind::Unofficial: break;
    }
    return tr("VD_SETUP_GUI_ARCHIVE_UNOFFICIAL");
}

std::string versionOf(const ClientCheck& check) {
    if (check.exeVersionRead) {
        return check.exeVersion.toString();
    }
    return check.macVersion.empty() ? std::string(tr("VD_SETUP_GUI_UNKNOWN")) : check.macVersion;
}

// A value with its name above, side by side with the next.
void fact(std::string_view name, const std::string& value) {
    ImGui::BeginGroup();
    textColored(kMuted, name, false);
    ImGui::PushFont(nullptr, kFontSize * 1.2f);
    text(value);
    ImGui::PopFont();
    ImGui::EndGroup();
}

} // namespace

void SetupWindow::checkWow(const std::filesystem::path& folder) {
    if (folder.empty()) {
        return;
    }
    const std::vector<std::string> include = m_settings.includeArchives;
    m_checkingLocale = false;
    m_wowTask.start([this, folder, include](const Operation&) -> Task::Apply {
        auto check = std::make_shared<ClientCheck>(checkClient(folder, include));
        return [this, check]() {
            m_candidate = *check;
            if (!check->usable) {
                return;
            }
            if (m_settings.wow != check->folder) {
                m_settings.wow = check->folder;
                m_bakeOutcome = {};
            }
            saveSettings();
            const std::string text = utf8FromPath(check->folder);
            std::snprintf(m_wowInput, sizeof(m_wowInput), "%s", text.c_str());
            refresh(true);
        };
    });
}

void SetupWindow::checkLocaleClient(const std::filesystem::path& folder) {
    if (folder.empty()) {
        return;
    }
    const std::vector<std::string> include = m_settings.includeArchives;
    m_checkingLocale = true;
    const std::filesystem::path wow = m_settings.wow;
    m_wowTask.start([this, folder, include, wow](const Operation&) -> Task::Apply {
        auto check = std::make_shared<ClientCheck>(checkClient(folder, include));
        // The WoW's language, read here too: the window's own check of it may still be running.
        const std::string baseLocale = check->usable ? checkClient(wow, include).locale : std::string();
        return [this, check, baseLocale]() {
            m_localeCandidate = *check;
            m_localeProblem = {};
            if (!check->usable) {
                m_localeProblem = {Message::Level::Error, "VD_SETUP_WOW_REFUSED", {}};
                return;
            }
            if (check->locale == baseLocale) {
                m_localeProblem = {Message::Level::Error, "VD_SETUP_WOW_SAME_LOCALE", {check->locale}};
                return;
            }
            m_settings.localeClient = check->folder;
            saveSettings();
            const std::string text = utf8FromPath(check->folder);
            std::snprintf(m_localeInput, sizeof(m_localeInput), "%s", text.c_str());
            refresh(true);
        };
    });
}

void SetupWindow::drawWowCheck(const ClientCheck& check, bool base) {
    if (!check.dataDir.empty()) {
        fact(tr("VD_SETUP_GUI_WOW_VERSION"), versionOf(check));
        ImGui::SameLine(0, ImGui::GetFontSize() * 2.5f);
        fact(tr("VD_SETUP_GUI_WOW_LANGUAGE"),
             check.locale.empty() ? std::string(tr("VD_SETUP_GUI_UNKNOWN")) : check.locale);
        ImGui::SameLine(0, ImGui::GetFontSize() * 2.5f);
        fact(tr("VD_SETUP_GUI_WOW_SIZE"), formatBytes(check.gameBytes()));
        gap(0.3f);
    }
    for (const Message& line : check.messages) {
        // The archives that are not official have their own boxes below.
        if (line.key == "VD_SETUP_WOW_UNOFFICIAL" || line.key == "VD_SETUP_WOW_UNOFFICIAL_INCLUDED") {
            continue;
        }
        message(line);
    }
    if (base && check.usable && check.folder == m_settings.wow) {
        textColored(kOk, tr("VD_SETUP_GUI_WOW_USED"));
    }
    if (!check.usable) {
        messageText(Message::Level::Error, tr("VD_SETUP_GUI_WOW_REFUSED"));
    }

    // Archives that are not official: left out unless ticked (vdsetup wow --include).
    std::vector<const ArchiveInfo*> unofficial;
    for (const ArchiveInfo& archive : check.archives) {
        if (archive.kind == ArchiveKind::Unofficial) {
            unofficial.push_back(&archive);
        }
    }
    if (!unofficial.empty()) {
        heading(tr("VD_SETUP_GUI_WOW_UNOFFICIAL_TITLE"));
        textWrapped(tr("VD_SETUP_GUI_WOW_UNOFFICIAL_INTRO"));
        ImGui::BeginDisabled(busy());
        for (const ArchiveInfo* archive : unofficial) {
            bool included = listed(m_settings.includeArchives, archive->name);
            const std::string box = archive->name + " (" + formatBytes(archive->size) + ")##inc" +
                                    (base ? "b" : "l") + archive->name;
            if (ImGui::Checkbox(box.c_str(), &included)) {
                std::vector<std::string>& names = m_settings.includeArchives;
                names.erase(
                    std::remove_if(names.begin(), names.end(),
                                   [&](const std::string& n) { return equalsNoCase(n, archive->name); }),
                    names.end());
                if (included) {
                    names.push_back(archive->name);
                }
                saveSettings();
                // Checked again with the new choice: the candidate if it was refused, else the settings'
                // clients.
                if (m_candidate && !m_candidate->usable && base) {
                    checkWow(m_candidate->folder);
                } else {
                    m_candidate.reset();
                    refresh(true);
                }
            }
        }
        ImGui::EndDisabled();
        if (!m_snapshot.packs.empty()) {
            textMuted(tr("VD_SETUP_GUI_WOW_INCLUDE_NOTE"));
        }
    }

    if (!check.archives.empty()) {
        gap(0.3f);
        const std::string header = trf("VD_SETUP_GUI_WOW_ARCHIVES", {std::to_string(check.archives.size())}) +
                                   (base ? "##arcb" : "##arcl");
        if (ImGui::CollapsingHeader(header.c_str())) {
            if (ImGui::BeginTable(base ? "##archivesb" : "##archivesl", 3,
                                  ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_BordersInnerH)) {
                for (const ArchiveInfo& archive : check.archives) {
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    text(archive.name);
                    ImGui::TableNextColumn();
                    textColored(kMuted, formatBytes(archive.size), false);
                    ImGui::TableNextColumn();
                    const bool bad = archive.state != ArchiveState::Ok;
                    textColored(bad                                       ? kError
                                : archive.kind == ArchiveKind::Unofficial ? kWarning
                                                                          : kMuted,
                                archiveNote(archive), false);
                }
                ImGui::EndTable();
            }
        }
    }
}

void SetupWindow::drawWow() {
    title(tr("VD_SETUP_GUI_WOW_TITLE"));
    textWrapped(tr("VD_SETUP_GUI_WOW_INTRO"));
    gap(0.5f);

    const bool locked = m_wowTask.running() || m_bakeTask.running() || m_dialogOpen;
    if (primaryButton(label("VD_SETUP_GUI_CHOOSE_FOLDER", "choosewow"), !locked)) {
        openFolderDialog(DialogPurpose::Wow, m_settings.wow);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 10.0f,
                                     ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 6.0f));
    const std::string hint(tr("VD_SETUP_GUI_PATH_HINT"));
    const bool entered = ImGui::InputTextWithHint("##wowpath", hint.c_str(), m_wowInput, sizeof(m_wowInput),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool pressed = button(label("VD_SETUP_GUI_CHECK", "checkwow"), m_wowInput[0] != 0);
    ImGui::EndDisabled();
    if ((entered || pressed) && m_wowInput[0] != 0 && !locked) {
        checkWow(pathFromUtf8(m_wowInput));
    }
    if (m_bakeTask.running()) {
        textMuted(tr("VD_SETUP_GUI_WOW_LOCKED_BAKING"));
    }
    gap(0.4f);

    if (m_wowTask.running() && !m_checkingLocale) {
        spinner();
        ImGui::SameLine();
        text(trf("VD_SETUP_GUI_WOW_CHECKING", {m_wowInput}));
    } else if (m_candidate) {
        drawWowCheck(*m_candidate, true);
        if (!m_candidate->usable && !m_settings.wow.empty() && m_candidate->folder != m_settings.wow) {
            gap(0.2f);
            textMuted(trf("VD_SETUP_GUI_WOW_KEPT", {utf8FromPath(m_settings.wow)}));
        }
    } else if (m_settings.wow.empty()) {
        textMuted(tr("VD_SETUP_GUI_WOW_NONE"));
    } else if (!m_snapshot.done || (m_refreshTask.running() && m_snapshot.clients.base.folder.empty())) {
        spinner();
        ImGui::SameLine();
        text(trf("VD_SETUP_GUI_WOW_CHECKING", {utf8FromPath(m_settings.wow)}));
    } else {
        drawWowCheck(m_snapshot.clients.base, true);
    }

    gap(0.6f);
    ImGui::Separator();
    drawLocaleClient();
}

void SetupWindow::drawLocaleClient() {
    heading(tr("VD_SETUP_GUI_LOCALE_TITLE"));
    textWrapped(tr("VD_SETUP_GUI_LOCALE_INTRO"));
    if (!m_snapshot.clients.base.usable) {
        textMuted(tr("VD_SETUP_GUI_LOCALE_NEEDS_WOW"));
        return;
    }
    gap(0.2f);
    const bool locked = m_wowTask.running() || m_bakeTask.running() || m_dialogOpen;
    if (button(label("VD_SETUP_GUI_CHOOSE_FOLDER", "chooselocale"), !locked)) {
        openFolderDialog(DialogPurpose::LocaleClient, m_settings.localeClient.empty()
                                                          ? m_settings.wow.parent_path()
                                                          : m_settings.localeClient);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(locked);
    ImGui::SetNextItemWidth(std::max(ImGui::GetFontSize() * 10.0f,
                                     ImGui::GetContentRegionAvail().x - ImGui::GetFontSize() * 12.0f));
    const std::string hint(tr("VD_SETUP_GUI_PATH_HINT"));
    const bool entered =
        ImGui::InputTextWithHint("##localepath", hint.c_str(), m_localeInput, sizeof(m_localeInput),
                                 ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    const bool pressed = button(label("VD_SETUP_GUI_CHECK", "checklocale"), m_localeInput[0] != 0);
    ImGui::SameLine();
    const bool remove =
        button(label("VD_SETUP_GUI_REMOVE", "removelocale"), !m_settings.localeClient.empty());
    ImGui::EndDisabled();
    if ((entered || pressed) && m_localeInput[0] != 0 && !locked) {
        checkLocaleClient(pathFromUtf8(m_localeInput));
    }
    if (remove && !locked) {
        m_settings.localeClient.clear();
        m_localeInput[0] = 0;
        m_localeCandidate.reset();
        m_localeProblem = {};
        saveSettings();
        refresh(true);
    }
    gap(0.3f);

    if (m_wowTask.running() && m_checkingLocale) {
        spinner();
        ImGui::SameLine();
        text(trf("VD_SETUP_GUI_WOW_CHECKING", {m_localeInput}));
        return;
    }
    if (!m_localeProblem.key.empty() && m_localeCandidate) {
        drawWowCheck(*m_localeCandidate, false);
        message(m_localeProblem);
        return;
    }
    const Clients& clients = m_snapshot.clients;
    if (m_settings.localeClient.empty()) {
        textMuted(tr("VD_SETUP_GUI_LOCALE_NONE"));
    } else if (clients.hasLocale) {
        messageText(Message::Level::Info, trf("VD_SETUP_GUI_LOCALE_USED", {clients.locale.locale}));
    } else if (clients.localeSkipped) {
        messageText(Message::Level::Warning, tr("VD_SETUP_BAKE_LOCALE_CLIENT_SKIPPED"));
        drawWowCheck(clients.locale, false);
    }
}

} // namespace vd::setup::gui
