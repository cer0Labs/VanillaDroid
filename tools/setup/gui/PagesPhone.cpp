// VanillaDroid Setup's window: the phone over Wi-Fi. The local server, its QR code and pairing code, and what
// each phone downloads (phase 22, gui; vdsetup serve).
#include "SetupWindow.h"

#include "HttpServer.h"

#include <algorithm>
#include <cstdio>

namespace vd::setup::gui {
namespace {

constexpr usize kMaxServerLines = 200;

std::string clockText(f64 seconds) {
    const u64 total = static_cast<u64>(seconds);
    char text[16];
    std::snprintf(text, sizeof(text), "%02u:%02u:%02u", static_cast<unsigned>(total / 3600),
                  static_cast<unsigned>(total / 60 % 60), static_cast<unsigned>(total % 60));
    return text;
}

// "042917" -> "042 917": easier to read and type.
std::string spacedCode(const std::string& code) {
    return code.size() == 6 ? code.substr(0, 3) + " " + code.substr(3) : code;
}

} // namespace

void SetupWindow::startServer() {
    m_serveError.clear();
    if (m_settings.pairingKey.empty()) {
        m_settings.pairingKey = newPairingKey();
        if (m_settings.pairingKey.empty()) {
            m_serveError = tr("VD_SETUP_SERVE_NO_RANDOM");
            return;
        }
        saveSettings();
    }
    std::string ip;
    if (m_options.loopback) {
        ip = "127.0.0.1";
    } else if (m_addressChoice >= 0 && m_addressChoice < static_cast<int>(m_addresses.size())) {
        ip = m_addresses[static_cast<usize>(m_addressChoice)].text();
    } else {
        m_serveError = tr("VD_SETUP_SERVE_NO_NETWORK");
        return;
    }

    ServeOptions options;
    options.packsDir = packs();
    options.apk = m_snapshot.apk;
    options.key = m_settings.pairingKey;
    options.code = newPairingCode();
    options.appId = m_settings.appId;
    options.htmlLang = toLowerAscii(m_locale.substr(0, 2));
    options.hashCache = m_dataDir / "hashes.json";
    if (m_serveWow && m_snapshot.clients.base.usable) {
        options.wowData = m_snapshot.clients.base.dataDir;
        for (const ArchiveInfo& archive : m_snapshot.clients.base.gameArchives()) {
            options.wowArchives.push_back(archive.name);
        }
    }
    m_serveCode = options.code;
    {
        std::lock_guard<std::mutex> lock(m_eventMutex);
        m_events.clear();
    }
    m_phones.clear();
    m_serverLines.clear();
    m_serverPreparing = true;
    m_serverReady = false;
    m_prepareFraction = 0.0;
    m_prepareText.clear();
    m_server = std::make_unique<PhoneServer>(options, [this](const ServeEvent& event) {
        {
            std::lock_guard<std::mutex> lock(m_eventMutex);
            m_events.push_back(event);
        }
        wakeWindow();
    });
    Result<u16> port = m_server->start(m_settings.port, 20, m_options.loopback);
    if (!port) {
        m_server.reset();
        m_serverPreparing = false;
        m_serveError = trf("VD_SETUP_SERVE_LISTEN_FAILED", {port.error().message});
        return;
    }
    m_serveHost = ip + ":" + std::to_string(port.value());
    m_server->setHost(m_serveHost);
    m_serveUrl = "http://" + m_serveHost + "/?k=" + m_settings.pairingKey;
    Result<QrMatrix> qr = encodeQr(m_serveUrl);
    m_qr = qr ? qr.value() : QrMatrix{};
}

void SetupWindow::stopServer() {
    if (m_server) {
        m_server->stop();
        m_server.reset();
    }
    std::lock_guard<std::mutex> lock(m_eventMutex);
    m_events.clear();
    m_serverPreparing = false;
    m_serverReady = false;
}

void SetupWindow::serverEvent(const ServeEvent& event) {
    if (!m_server) {
        return; // a late event of a server already stopped
    }
    const f64 now = secondsBetween(m_start, Clock::now());
    switch (event.kind) {
    case ServeEvent::Kind::Preparing:
        m_serverPreparing = true;
        m_prepareFraction = event.fraction;
        m_prepareText = event.text;
        break;
    case ServeEvent::Kind::Ready:
        m_serverPreparing = false;
        m_serverReady = true;
        m_readyBytes = event.bytes;
        m_serverLines.push_back(
            {now, {Message::Level::Info, "VD_SETUP_SERVE_READY", {formatBytes(event.bytes)}}});
        break;
    case ServeEvent::Kind::PrepareFailed:
        m_serverPreparing = false;
        m_serverReady = false;
        break;
    default: break;
    }
    m_phones.apply(event, now);
    if (Message line = serveEventMessage(event); !line.key.empty()) {
        m_serverLines.push_back({now, std::move(line)});
    }
    while (m_serverLines.size() > kMaxServerLines) {
        m_serverLines.pop_front();
    }
}

void SetupWindow::drawPhone() {
    title(tr("VD_SETUP_GUI_PHONE_TITLE"));
    textWrapped(tr("VD_SETUP_GUI_PHONE_INTRO"));
    gap(0.5f);
    if (m_server) {
        drawPhoneServing();
        return;
    }
    const Snapshot& s = m_snapshot;
    if (!m_serveError.empty()) {
        messageText(Message::Level::Error, m_serveError);
    }
    if (s.done && s.packs.empty()) {
        messageText(Message::Level::Warning, tr("VD_SETUP_GUI_PHONE_NO_PACKS"));
    } else if (s.done && s.clients.base.usable && !s.plan.jobs.empty()) {
        messageText(Message::Level::Warning, tr("VD_SETUP_GUI_PHONE_PACKS_OUTDATED"));
    }
    if (s.done && s.apk.empty()) {
        messageText(Message::Level::Warning, tr("VD_SETUP_GUI_PHONE_NO_APK"));
    }
    gap(0.3f);

    if (m_options.loopback) {
        messageText(Message::Level::Info, tr("VD_SETUP_GUI_LOOPBACK"));
    } else if (m_addresses.empty()) {
        messageText(Message::Level::Error, tr("VD_SETUP_SERVE_NO_NETWORK"));
        if (button(label("VD_SETUP_GUI_PHONE_LOOK_AGAIN", "addresses"))) {
            m_addresses = localAddresses();
            const LocalAddress* best = pickLanAddress(m_addresses);
            m_addressChoice = best ? static_cast<int>(best - m_addresses.data()) : 0;
        }
    } else if (m_addresses.size() > 1) {
        text(tr("VD_SETUP_GUI_PHONE_ADDRESS"));
        auto name = [](const LocalAddress& address) {
            return address.text() + "  (" + address.adapter + ")";
        };
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 24.0f);
        const int choice = std::clamp(m_addressChoice, 0, static_cast<int>(m_addresses.size()) - 1);
        if (ImGui::BeginCombo("##address", name(m_addresses[static_cast<usize>(choice)]).c_str())) {
            for (usize i = 0; i < m_addresses.size(); ++i) {
                if (ImGui::Selectable((name(m_addresses[i]) + "##a" + std::to_string(i)).c_str(),
                                      static_cast<int>(i) == choice)) {
                    m_addressChoice = static_cast<int>(i);
                }
            }
            ImGui::EndCombo();
        }
        textMuted(tr("VD_SETUP_GUI_PHONE_ADDRESS_NOTE"));
    }
    if (s.clients.base.usable) {
        gap(0.2f);
        const std::string offer =
            trf("VD_SETUP_GUI_PHONE_OFFER_WOW", {formatBytes(s.clients.base.gameBytes())}) + "##wow";
        ImGui::Checkbox(offer.c_str(), &m_serveWow);
    }
    gap(0.4f);
    if (primaryButton(label("VD_SETUP_GUI_PHONE_START", "start"),
                      s.done && (m_options.loopback || !m_addresses.empty()))) {
        startServer();
    }
    gap(0.6f);
    if (ImGui::TextLink(label("VD_SETUP_GUI_PHONE_USE_ADB", "useadb").c_str())) {
        m_wizard = false;
        goTo(Page::Adb);
    }
}

void SetupWindow::drawPhoneServing() {
    const f32 scale = ImGui::GetStyle().FontScaleDpi;
    const f32 side = std::min(300.0f * scale, ImGui::GetContentRegionAvail().x * 0.42f);
    if (ImGui::BeginTable("##serving", 2, ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("##qr", ImGuiTableColumnFlags_WidthFixed, side);
        ImGui::TableSetupColumn("##how", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        qrCode(m_qr, side);
        ImGui::TableNextColumn();
        ImGui::PushTextWrapPos(0.0f);
        heading(tr("VD_SETUP_SERVE_SCAN"));
        textWrapped(trf("VD_SETUP_SERVE_URL", {m_serveUrl}));
        if (button(label("VD_SETUP_GUI_COPY", "copyurl"))) {
            ImGui::SetClipboardText(m_serveUrl.c_str());
            m_copiedAt = ImGui::GetTime();
        }
        if (ImGui::GetTime() - m_copiedAt < 2.0) {
            ImGui::SameLine();
            ImGui::AlignTextToFramePadding();
            textColored(kOk, tr("VD_SETUP_GUI_COPIED"), false);
        }
        gap(0.5f);
        textWrapped(trf("VD_SETUP_SERVE_CODE", {m_serveHost, spacedCode(m_serveCode)}));
        ImGui::BeginGroup();
        textColored(kMuted, tr("VD_SETUP_GUI_PHONE_HOST_LABEL"), false);
        ImGui::PushFont(nullptr, kFontSize * 1.6f);
        textColored(kText, m_serveHost, false);
        ImGui::PopFont();
        ImGui::EndGroup();
        ImGui::SameLine(0, ImGui::GetFontSize() * 2.0f);
        ImGui::BeginGroup();
        textColored(kMuted, tr("VD_SETUP_GUI_PHONE_CODE_LABEL"), false);
        ImGui::PushFont(nullptr, kFontSize * 1.6f);
        textColored(kGold, spacedCode(m_serveCode), false);
        ImGui::PopFont();
        ImGui::EndGroup();
        gap(0.5f);
        if (m_serverPreparing) {
            text(m_prepareText.empty() ? tr("VD_SETUP_GUI_PHONE_PREPARING")
                                       : std::string_view(m_prepareText));
            progressBar(
                m_prepareFraction,
                trf("VD_SETUP_GUI_PERCENT", {std::to_string(static_cast<int>(m_prepareFraction * 100.0))}));
        } else if (m_serverReady) {
            messageText(Message::Level::Info, trf("VD_SETUP_SERVE_READY", {formatBytes(m_readyBytes)}));
        }
        ImGui::PopTextWrapPos();
        ImGui::EndTable();
    }

    if (m_options.loopback) {
        messageText(Message::Level::Info, tr("VD_SETUP_GUI_LOOPBACK"));
    }
    heading(tr("VD_SETUP_GUI_PHONE_PHONES"));
    if (m_phones.phones().empty()) {
        textMuted(tr("VD_SETUP_GUI_PHONE_NO_PHONES"));
    } else if (ImGui::BeginTable("##phones", 3,
                                 ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                     ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableSetupColumn(label("VD_SETUP_GUI_PHONE_COL_PHONE", "phone").c_str(),
                                ImGuiTableColumnFlags_WidthStretch, 0.8f);
        ImGui::TableSetupColumn(label("VD_SETUP_GUI_PHONE_COL_NOW", "now").c_str(),
                                ImGuiTableColumnFlags_WidthStretch, 2.0f);
        ImGui::TableSetupColumn(label("VD_SETUP_GUI_PHONE_COL_DONE", "done").c_str(),
                                ImGuiTableColumnFlags_WidthStretch, 0.9f);
        ImGui::TableHeadersRow();
        for (const PhoneActivity& phone : m_phones.phones()) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            text(ipv4Text(phone.client));
            if (phone.unauthorized) {
                textColored(kWarning, tr("VD_SETUP_GUI_PHONE_NO_KEY"), false);
            } else if (phone.paired) {
                textColored(kOk, tr("VD_SETUP_GUI_PHONE_LINKED"), false);
            }
            ImGui::TableNextColumn();
            if (phone.transfers.empty()) {
                textMuted(tr("VD_SETUP_GUI_PHONE_IDLE"));
            }
            for (const PhoneTransfer& transfer : phone.transfers) {
                text(transfer.path);
                const f64 fraction =
                    transfer.total ? static_cast<f64>(transfer.position) / static_cast<f64>(transfer.total)
                                   : 0.0;
                progressBar(fraction, formatBytes(transfer.position) + " / " + formatBytes(transfer.total));
            }
            ImGui::TableNextColumn();
            text(trf("VD_SETUP_GUI_PHONE_FILES",
                     {std::to_string(phone.filesReceived), formatBytes(phone.bytesSent)}));
        }
        ImGui::EndTable();
    }

    gap(0.4f);
    if (button(label("VD_SETUP_GUI_PHONE_STOP", "stop"))) {
        stopServer();
        return;
    }
    ImGui::SameLine();
    const std::string question = label("VD_SETUP_GUI_PHONE_NEW_KEY", "newkey");
    if (button(question)) {
        ImGui::OpenPopup(question.c_str());
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(ImGui::GetFontSize() * 26.0f, 0));
    if (ImGui::BeginPopupModal(question.c_str(), nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        textWrapped(tr("VD_SETUP_GUI_PHONE_NEW_KEY_CONFIRM"));
        gap(0.4f);
        if (primaryButton(label("VD_SETUP_GUI_PHONE_NEW_KEY_YES", "yes"))) {
            m_settings.pairingKey = newPairingKey();
            saveSettings();
            stopServer();
            startServer();
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (button(label("VD_SETUP_GUI_CANCEL", "no"))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }

    gap(0.4f);
    textMuted(tr("VD_SETUP_GUI_SERVE_KEEP_OPEN"));
#if defined(__APPLE__)
    textMuted(tr("VD_SETUP_GUI_FIREWALL_MAC"));
#else
    textMuted(tr("VD_SETUP_SERVE_FIREWALL"));
#endif

    if (!m_serverLines.empty()) {
        gap(0.3f);
        if (ImGui::CollapsingHeader(label("VD_SETUP_GUI_PHONE_ACTIVITY", "activity").c_str())) {
            for (auto it = m_serverLines.rbegin(); it != m_serverLines.rend(); ++it) {
                textColored(kMuted, clockText(it->seconds), false);
                ImGui::SameLine();
                message(it->line);
            }
        }
    }
}

} // namespace vd::setup::gui
