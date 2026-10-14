// VanillaDroid Setup's window: the phone with ADB, the steps of `vdsetup adb help` as buttons (phase 22,
// gui).
#include "SetupWindow.h"

#include <algorithm>
#include <cstdio>

namespace vd::setup::gui {
namespace {

// A line of the core's own words (an error it already put in the player's language).
Message raw(Message::Level level, std::string text) {
    return {level, "VD_SETUP_GUI_RAW", {std::move(text)}};
}

void command(std::string_view key) {
    textColored(kMuted, trf("VD_SETUP_GUI_COMMAND", {std::string(tr(key))}));
}

std::string_view stateWords(const std::string& state) {
    if (state == "device") {
        return tr("VD_SETUP_GUI_ADB_STATE_DEVICE");
    }
    if (state == "unauthorized") {
        return tr("VD_SETUP_GUI_ADB_STATE_UNAUTHORIZED");
    }
    return tr("VD_SETUP_GUI_ADB_STATE_OFFLINE");
}

void numbered(std::string_view key) {
    heading(tr(key));
}

} // namespace

std::string SetupWindow::chosenSerial() const {
    for (const AdbDevice& device : m_devices) {
        if (device.serial == m_settings.serial) {
            return device.serial;
        }
    }
    return m_devices.size() == 1 ? m_devices.front().serial : std::string();
}

Adb SetupWindow::adbFor() const {
    return Adb(m_snapshot.adb, chosenSerial());
}

void SetupWindow::adbRefreshDevices() {
    m_adbStep = AdbStep::Devices;
    const Adb adb = adbFor();
    m_adbTask.start([this, adb](const Operation&) -> Task::Apply {
        auto devices = std::make_shared<Result<std::vector<AdbDevice>>>(adb.devices());
        return [this, devices]() {
            m_devicesListed = true;
            if (*devices) {
                m_devices = devices->value();
                m_adbLines.clear(); // the list says it (or that there is none)
            } else {
                m_devices.clear();
                m_adbLines = {raw(Message::Level::Error, devices->error().message)};
            }
        };
    });
}

void SetupWindow::adbSetup() {
    m_adbStep = AdbStep::Tools;
    const std::filesystem::path data = m_dataDir;
    m_adbLines.clear();
    m_adbTask.start([this, data](const Operation& operation) -> Task::Apply {
        auto installed =
            std::make_shared<Result<std::filesystem::path>>(installPlatformTools(data, operation));
        return [this, installed]() {
            if (*installed) {
                m_adbLines = {
                    {Message::Level::Info, "VD_SETUP_ADB_INSTALLED", {utf8FromPath(installed->value())}}};
            } else {
                m_adbLines = {raw(Message::Level::Error, installed->error().message)};
            }
            refresh(false);
        };
    });
}

void SetupWindow::adbPair() {
    m_adbStep = AdbStep::Connect;
    const Adb adb = adbFor();
    const std::string address = m_pairAddress;
    const std::string code = m_pairCode;
    m_adbLines.clear();
    m_adbTask.start([this, adb, address, code](const Operation&) -> Task::Apply {
        auto paired = std::make_shared<Result<std::string>>(adb.pair(address, code));
        return [this, paired, address]() {
            if (*paired) {
                m_adbLines = {{Message::Level::Info, "VD_SETUP_ADB_PAIRED", {address}},
                              {Message::Level::Info, "VD_SETUP_GUI_ADB_PAIRED_NEXT", {}}};
                m_pairCode[0] = 0;
            } else {
                m_adbLines = {raw(Message::Level::Error, paired->error().message)};
            }
        };
    });
}

void SetupWindow::adbConnect() {
    m_adbStep = AdbStep::Connect;
    const Adb adb = adbFor();
    const std::string address = m_connectAddress;
    m_adbLines.clear();
    m_adbTask.start([this, adb, address](const Operation&) -> Task::Apply {
        auto connected = std::make_shared<Result<std::string>>(adb.connect(address));
        auto devices = std::make_shared<Result<std::vector<AdbDevice>>>(adb.devices());
        return [this, connected, devices, address]() {
            if (*connected) {
                m_adbLines = {{Message::Level::Info, "VD_SETUP_ADB_CONNECTED", {address}}};
            } else {
                m_adbLines = {raw(Message::Level::Error, connected->error().message)};
            }
            if (*devices) {
                m_devices = devices->value();
                m_devicesListed = true;
            }
        };
    });
}

void SetupWindow::adbInstall() {
    m_adbStep = AdbStep::Install;
    const Snapshot& s = m_snapshot;
    m_adbLines.clear();
    if (!s.apkRead) {
        m_adbLines = {{Message::Level::Error, "VD_SETUP_GUI_PHONE_NO_APK", {}}};
        return;
    }
    if (s.apkInfo.package != m_settings.appId) {
        m_adbLines = {
            {Message::Level::Error, "VD_SETUP_GUI_ADB_OTHER_APP", {s.apkInfo.package, m_settings.appId}}};
        return;
    }
    const Adb adb = adbFor();
    const std::filesystem::path apk = s.apk;
    const std::string version = s.apkInfo.versionName;
    m_adbTask.start([this, adb, apk, version](const Operation& operation) -> Task::Apply {
        auto installed = std::make_shared<Result<void>>(adb.install(apk, operation));
        return [this, installed, version]() {
            if (*installed) {
                m_adbLines = {{Message::Level::Info, "VD_SETUP_ADB_INSTALL_DONE", {version}},
                              {Message::Level::Info, "VD_SETUP_GUI_ADB_OPEN_ONCE", {}}};
            } else {
                m_adbLines = {raw(Message::Level::Error, installed->error().message)};
            }
        };
    });
}

void SetupWindow::adbPush(bool wow) {
    m_adbStep = wow ? AdbStep::SendWow : AdbStep::Push;
    const Adb adb = adbFor();
    const std::string appId = m_settings.appId;
    const std::filesystem::path packsFolder = packs();
    const std::filesystem::path data = m_dataDir;
    const ClientCheck client = m_snapshot.clients.base;
    m_adbLines.clear();
    m_adbTask.start([this, adb, appId, packsFolder, data, client,
                     wow](const Operation& operation) -> Task::Apply {
        auto lines = std::make_shared<std::vector<Message>>();
        const auto start = Clock::now();
        Result<bool> installed = adb.isInstalled(appId);
        if (installed && !installed.value()) {
            lines->push_back({Message::Level::Error, "VD_SETUP_GUI_ADB_NOT_INSTALLED", {appId}});
        } else {
            HashCache cache(data / "hashes.json");
            cache.load();
            Result<std::vector<PushItem>> items =
                wow ? wowPushItems(client, cache, operation) : packPushItems(packsFolder, cache, operation);
            if (!items) {
                lines->push_back(raw(Message::Level::Error, items.error().message));
            } else if (items.value().empty()) {
                lines->push_back({Message::Level::Error, "VD_SETUP_GUI_PHONE_NO_PACKS", {}});
            } else {
                const std::string remote = remoteFilesDir(appId) + (wow ? "/wow/Data" : "/packs");
                Result<PushOutcome> pushed = pushFiles(adb, remote, items.value(), operation);
                if (!pushed) {
                    lines->push_back(raw(Message::Level::Error, operation.cancelled()
                                                                    ? formatText("VD_SETUP_GUI_ADB_CANCELLED")
                                                                    : pushed.error().message));
                } else {
                    char seconds[32];
                    std::snprintf(seconds, sizeof(seconds), "%.0f", secondsBetween(start, Clock::now()));
                    lines->push_back(
                        {Message::Level::Info,
                         "VD_SETUP_ADB_PUSH_DONE",
                         {std::to_string(pushed.value().pushed), formatBytes(pushed.value().bytes),
                          std::to_string(pushed.value().skipped), seconds}});
                }
            }
        }
        return [this, lines]() { m_adbLines = *lines; };
    });
}

void SetupWindow::drawAdbState(AdbStep step) {
    // What runs, or how the latest operation ended, under the step that started it.
    const bool running = m_adbTask.running();
    if (m_adbStep != step || (!running && m_adbLines.empty())) {
        return;
    }
    gap(0.2f);
    ImGui::PushStyleColor(ImGuiCol_ChildBg, kPanel);
    ImGui::BeginChild("##adbstate", ImVec2(0, 0),
                      ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY |
                          ImGuiChildFlags_AlwaysUseWindowPadding);
    if (running) {
        const Task::State state = m_adbTask.state();
        spinner();
        ImGui::SameLine();
        text(state.text.empty() ? tr("VD_SETUP_GUI_ADB_WORKING") : std::string_view(state.text));
        if (state.step == "push" || state.step == "hash" || state.step == "download") {
            progressBar(state.fraction,
                        trf("VD_SETUP_GUI_PERCENT",
                            {std::to_string(static_cast<int>(std::max(0.0, state.fraction) * 100))}));
        }
        if (button(label("VD_SETUP_GUI_CANCEL", "canceladb"))) {
            m_adbTask.cancel();
        }
    } else {
        for (const Message& line : m_adbLines) {
            message(line);
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void SetupWindow::drawAdb() {
    title(tr("VD_SETUP_GUI_ADB_TITLE"));
    textWrapped(tr("VD_SETUP_GUI_ADB_INTRO"));
    const Snapshot& s = m_snapshot;
    const bool running = m_adbTask.running();
    const bool hasAdb = !s.adb.empty();
    const std::string serial = chosenSerial();
    const auto chosen = std::find_if(m_devices.begin(), m_devices.end(),
                                     [&](const AdbDevice& d) { return d.serial == serial; });
    const bool deviceReady = chosen != m_devices.end() && chosen->state == "device";

    numbered("VD_SETUP_GUI_ADB_STEP_DEVELOPER");
    textWrapped(tr("VD_SETUP_GUI_ADB_DEVELOPER_TEXT"));

    numbered("VD_SETUP_GUI_ADB_STEP_TOOLS");
    if (!s.done) {
        spinner();
        ImGui::SameLine();
        text(tr("VD_SETUP_GUI_HOME_CHECKING"));
    } else if (hasAdb) {
        messageText(Message::Level::Info, trf("VD_SETUP_GUI_ADB_FOUND", {utf8FromPath(s.adb)}));
    } else {
        messageText(Message::Level::Warning, tr("VD_SETUP_GUI_ADB_NOT_FOUND"));
        textWrapped(trf("VD_SETUP_ADB_TERMS",
                        {std::string(platformToolsUrl()), std::string(platformToolsTermsUrl())}));
        const std::string terms(platformToolsTermsUrl());
        ImGui::TextLinkOpenURL(label("VD_SETUP_GUI_ADB_READ_TERMS", "terms").c_str(), terms.c_str());
        ImGui::Checkbox(label("VD_SETUP_GUI_ADB_ACCEPT", "accept").c_str(), &m_acceptTerms);
        if (primaryButton(label("VD_SETUP_GUI_ADB_DOWNLOAD", "download"), m_acceptTerms && !running)) {
            adbSetup();
        }
        ImGui::SameLine();
    }
    if (button(label("VD_SETUP_GUI_ADB_OTHER", "otheradb"), !running && !m_dialogOpen)) {
        openFileDialog(DialogPurpose::Adb, s.adb.empty() ? m_dataDir : s.adb.parent_path());
    }
    command("VD_SETUP_GUI_ADB_CMD_SETUP");
    drawAdbState(AdbStep::Tools);

    numbered("VD_SETUP_GUI_ADB_STEP_CONNECT");
    textWrapped(tr("VD_SETUP_GUI_ADB_WIRELESS"));
    const f32 field = ImGui::GetFontSize() * 11.0f;
    ImGui::SetNextItemWidth(field);
    ImGui::InputTextWithHint("##pairaddress", std::string(tr("VD_SETUP_GUI_ADB_PAIR_ADDRESS")).c_str(),
                             m_pairAddress, sizeof(m_pairAddress), ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(field * 0.6f);
    ImGui::InputTextWithHint("##paircode", std::string(tr("VD_SETUP_GUI_ADB_PAIR_CODE")).c_str(), m_pairCode,
                             sizeof(m_pairCode), ImGuiInputTextFlags_CharsDecimal);
    ImGui::SameLine();
    if (button(label("VD_SETUP_GUI_ADB_PAIR", "pair"),
               hasAdb && !running && m_pairAddress[0] && m_pairCode[0])) {
        adbPair();
    }
    command("VD_SETUP_GUI_ADB_CMD_PAIR");
    gap(0.2f);
    textWrapped(tr("VD_SETUP_GUI_ADB_CONNECT_TEXT"));
    ImGui::SetNextItemWidth(field);
    ImGui::InputTextWithHint("##connectaddress", std::string(tr("VD_SETUP_GUI_ADB_CONNECT_ADDRESS")).c_str(),
                             m_connectAddress, sizeof(m_connectAddress), ImGuiInputTextFlags_CharsNoBlank);
    ImGui::SameLine();
    if (button(label("VD_SETUP_GUI_ADB_CONNECT", "connect"), hasAdb && !running && m_connectAddress[0])) {
        adbConnect();
    }
    command("VD_SETUP_GUI_ADB_CMD_CONNECT");
    gap(0.2f);
    drawAdbState(AdbStep::Connect);
    textWrapped(tr("VD_SETUP_GUI_ADB_CABLE"));

    numbered("VD_SETUP_GUI_ADB_STEP_DEVICES");
    if (button(label("VD_SETUP_GUI_ADB_REFRESH", "devices"), hasAdb && !running)) {
        adbRefreshDevices();
    }
    if (m_devicesListed && m_devices.empty()) {
        textMuted(tr("VD_SETUP_GUI_ADB_NO_DEVICES"));
    }
    for (const AdbDevice& device : m_devices) {
        const std::string text = device.serial + (device.model.empty() ? "" : "  " + device.model) + "  -  " +
                                 std::string(stateWords(device.state)) + "##dev" + device.serial;
        if (ImGui::RadioButton(text.c_str(), device.serial == serial) && device.serial != m_settings.serial) {
            m_settings.serial = device.serial;
            saveSettings();
        }
    }
    if (m_devices.size() > 1 && serial.empty()) {
        textMuted(tr("VD_SETUP_GUI_ADB_CHOOSE_ONE"));
    }
    command("VD_SETUP_GUI_ADB_CMD_DEVICES");
    drawAdbState(AdbStep::Devices);

    numbered("VD_SETUP_GUI_ADB_STEP_INSTALL");
    if (s.apkRead) {
        textWrapped(trf("VD_SETUP_GUI_ADB_APK", {s.apkInfo.versionName, s.apkInfo.package}));
    } else {
        messageText(Message::Level::Warning, tr("VD_SETUP_GUI_PHONE_NO_APK"));
    }
    const bool canUse = hasAdb && !running && deviceReady;
    if (primaryButton(label("VD_SETUP_GUI_ADB_INSTALL", "install"), canUse && s.apkRead)) {
        adbInstall();
    }
    if (!deviceReady && hasAdb) {
        ImGui::SameLine();
        ImGui::AlignTextToFramePadding();
        textColored(kMuted, tr("VD_SETUP_GUI_ADB_NEED_DEVICE"), false);
    }
    command("VD_SETUP_GUI_ADB_CMD_INSTALL");
    drawAdbState(AdbStep::Install);

    numbered("VD_SETUP_GUI_ADB_STEP_PUSH");
    textWrapped(tr("VD_SETUP_GUI_ADB_PUSH_NOTE"));
    if (primaryButton(label("VD_SETUP_GUI_ADB_PUSH", "push"), canUse && !s.packs.empty())) {
        adbPush(false);
    }
    command("VD_SETUP_GUI_ADB_CMD_PUSH");
    drawAdbState(AdbStep::Push);

    numbered("VD_SETUP_GUI_ADB_STEP_WOW");
    if (s.clients.base.usable) {
        textWrapped(trf("VD_SETUP_GUI_ADB_WOW_NOTE", {formatBytes(s.clients.base.gameBytes())}));
    }
    if (button(label("VD_SETUP_GUI_ADB_SEND_WOW", "sendwow"), canUse && s.clients.base.usable)) {
        adbPush(true);
    }
    command("VD_SETUP_GUI_ADB_CMD_SEND_WOW");
    drawAdbState(AdbStep::SendWow);
}

} // namespace vd::setup::gui
