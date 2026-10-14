#pragma once

#include "app/DataDownload.h"
#include "app/Dialogs.h"
#include "app/Pairing.h"
#include "app/PhoneBake.h" // phase 22 (phonebake)
#include "core/Text.h"

#include <array>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace vd {

// Phase 22 (pairing, ROADMAP 22.4, ADR-0005 §5): the screen a phone without the game's data opens on (Game,
// with app.vdpk alone). It explains the ways to bring the data from VanillaDroid Setup on the PC (pairing
// over the local network through the QR code's link or an address and a 6-digit code, or ADB), remembers the
// paired PC (pairing.json, internal storage), downloads (app/DataDownload, thread vd-download) and, with the
// data there, asks the platform to start the game over (wantsRestart: Game::init opens the packs). The pages
// are pure views (app/FirstRunWindows); this is their glue. Phase 22 (phonebake, ROADMAP 22.7): with the WoW
// copied to the phone, "Prepare the data on this phone" bakes the packs there (app/PhoneBake: a service of
// its own process; the pages only follow it).
class FirstRunScreens {
public:
    // Writes a small file off the game thread (Game's DeviceMonitor).
    using FileWriter = void (*)(void* user, const std::filesystem::path& path, std::string text);
    struct Config {
        std::filesystem::path packsDir;
        std::filesystem::path wowDir;     // the WoW's archives on the device (<app files>/wow/Data)
        std::filesystem::path storageDir; // internal storage: pairing.json, downloads.json
        FileWriter writeFile = nullptr;
        void* writer = nullptr;
        // Phase 22 (phonebake): the bake on the phone (Game's; already started). Null: no such option.
        PhoneBake* phoneBake = nullptr;
        // Development (VD_PAIRING_DEMO): a page with sample data, no network (screenshots).
        std::string_view demo;
    };

    FirstRunScreens() = default;
    ~FirstRunScreens();
    FirstRunScreens(const FirstRunScreens&) = delete;
    FirstRunScreens& operator=(const FirstRunScreens&) = delete;

    void init(const Config& config);
    void shutdown(); // stops the download thread (what was downloaded stays, resumable)

    // vanilladroid://pair?h=<ip>:<port>&k=<key>: the app opened (or brought back) by the PC page's link.
    void onLink(std::string_view uri);
    // One frame of the screen on the HUD's context; `input` is the one text field (the address and the code).
    void draw(DialogContext& c, ui::TextInput& input);
    // Back (Android) / Escape (desktop): the page before; false on the home page (the platform's own then).
    bool escape(ui::TextInput& input);
    bool wantsRestart() const { return m_restart; }

private:
    enum class Page : u8 {
        Home,
        Qr,
        Address,
        Adb,
        Busy,
        Ready,
        Downloading,
        Paused,
        Failed,
        Done,
        // Phase 22 (phonebake): the bake on the phone.
        PhoneIntro,
        PhoneBaking,
        PhoneStopped, // cancelled, stopped by Android, or failed
        PhoneDone,
    };
    enum class Job : u8 { None, Pair, Check, Download };
    // A text made of several formatted pieces, in place (nothing allocates while drawing).
    template <usize N> struct Lines {
        std::array<char, N> bytes{};
        usize length = 0;
        void clear() {
            length = 0;
            bytes[0] = '\0';
        }
        template <typename... Args> void add(text::Key key, const Args&... args) {
            if (length + 1 < N) {
                length += text::format(std::span(bytes.data() + length, N - length), key, args...).size();
            }
        }
        void raw(std::string_view text) {
            for (const char c : text) {
                if (length + 1 >= N) {
                    break;
                }
                bytes[length++] = c;
            }
            bytes[length] = '\0';
        }
        std::string_view view() const { return {bytes.data(), length}; }
    };

    void follow();
    void setPc(std::optional<PairedPc> pc);
    void connect(ui::TextInput& input);
    void startCheck();
    void startDownload();
    void play(ui::TextInput& input);
    void forget();
    void savePairing();
    void showFailure(DownloadStatus::Failure failure, std::string_view detail);
    void goHome(ui::TextInput& input);
    void startDemo(std::string_view page);

    void drawHome(DialogContext& c, ui::TextInput& input);
    void drawQr(DialogContext& c, ui::TextInput& input);
    void drawAddress(DialogContext& c, ui::TextInput& input);
    void drawAdb(DialogContext& c, ui::TextInput& input);
    void drawBusy(DialogContext& c, ui::TextInput& input);
    void drawReady(DialogContext& c, ui::TextInput& input);
    void drawDownloading(DialogContext& c, ui::TextInput& input, bool paused);
    void drawFailed(DialogContext& c, ui::TextInput& input);
    void drawDone(DialogContext& c, ui::TextInput& input);
    // The status line of a download job still talking to the PC (preparing, checking, retrying).
    void statusLine(const DownloadStatus& status);
    // Phase 22 (phonebake): the bake's pages, and the page its state leads to.
    void followBake();
    void openPhoneIntro();
    void drawPhoneIntro(DialogContext& c, ui::TextInput& input);
    void drawPhoneBaking(DialogContext& c, ui::TextInput& input);
    void drawPhoneStopped(DialogContext& c, ui::TextInput& input);
    void drawPhoneDone(DialogContext& c, ui::TextInput& input);
    void phoneStepLine(); // m_progressTop: "Textures (2 of 13)"

    Config m_config;
    DataDownload m_download;
    bool m_started = false;
    bool m_demo = false;
    Page m_page = Page::Home;
    Page m_back = Page::Home; // where Cancel and Back go from the busy and failed pages
    Job m_job = Job::None;    // what the busy or downloading page waits for
    Job m_failedJob = Job::None;
    u32 m_jobId = 0;
    std::optional<PairedPc> m_pc;  // the PC paired (pairing.json)
    net::HttpEndpoint m_pairingAt; // the PC being paired with a code
    std::array<char, 32> m_pcAddress{};
    std::array<char, 32> m_pairingAddress{};
    bool m_hasData = false; // the game could start (an interrupted download: "play with what is here")
    bool m_wowOnDevice = false;
    bool m_restart = false;
    DownloadStatus m_status; // the download thread's, this frame
    DownloadStatus::Failure m_failure = DownloadStatus::Failure::None;
    bool m_linkError = false;
    std::array<char, 160> m_failureDetail{};
    text::Key m_formError = text::Key("");
    FormText<32> m_address;
    FormText<16> m_code;
    // The pages' formatted texts.
    Lines<1024> m_body;
    Lines<256> m_status1;
    Lines<160> m_progressTop;
    Lines<96> m_progressLeft;
    Lines<96> m_progressRight;
    Lines<256> m_check;
    Lines<128> m_note;
    std::array<char, 24> m_version{};
    // Phase 22 (phonebake): PhoneBake's status, this frame; whether the screen went to the bake's page once.
    PhoneBakeStatus m_bake;
    bool m_bakeFollowed = false;
};

} // namespace vd
