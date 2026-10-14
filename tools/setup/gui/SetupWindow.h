#pragma once

#include "Adb.h"
#include "ApkInfo.h"
#include "BakeRunner.h"
#include "GuiKit.h"
#include "GuiTask.h"
#include "NetInterfaces.h"
#include "PhoneServer.h"
#include "QrText.h"
#include "SetupFlow.h"
#include "SetupSettings.h"
#include "SetupUpdate.h" // phase 22 (polish): the update check on the About page
#include "WowCheck.h"
#include "core/Clock.h"

#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

struct SDL_Window;
struct SDL_Texture;

// Phase 22 (gui, ADR-0005 §4-§5): VanillaDroid Setup's window. A guide the first time (language, the WoW, the
// game data, the phone), then a home page with the state of each part and a page for each: the WoW (folder,
// check, archives that are not official, a second client), the game data (plan, bake with progress), the
// phone over Wi-Fi (the local server, its QR code and code, each phone's downloads) and with ADB (the guide's
// steps as buttons), and the versions. It drives the same core as the console tool vdsetup (tools/setup):
// long operations are Tasks on their own threads, applied here on the window's thread.
namespace vd::setup::gui {

struct WindowOptions {
    std::filesystem::path dataDir; // --data-dir (settings, packs, adb, the digests' cache)
    std::string page;              // --page: the page to open (home, language, wow, bake, phone, adb, about)
    std::string language;          // --lang: this run's language, not saved
    std::filesystem::path wow;     // --wow: a WoW folder to check at once, as if chosen in the folder window
    std::filesystem::path localeClient; // --locale-client: the same for the second client
    bool loopback = false; // --loopback: the server listens on 127.0.0.1 only (tests: no firewall question)
};

class SetupWindow {
public:
    SetupWindow(SDL_Window* window, WindowOptions options);
    ~SetupWindow();
    SetupWindow(const SetupWindow&) = delete;
    SetupWindow& operator=(const SetupWindow&) = delete;

    // The logo for the sidebar (owned by main.cpp).
    void setLogo(SDL_Texture* texture, f32 width, f32 height);
    // Before ImGui::NewFrame: what has to change between frames (the language, finished tasks, server
    // events). True when something did: the window then draws a second frame (sizes that follow their content
    // settle).
    bool update();
    // The frame's content, inside ImGui's frame.
    void draw();
    // Whether something moves on screen (progress, a spinner, a text cursor): the loop then wakes on a timer.
    bool animating() const;
    // A file or folder dropped on the window: on the WoW page, the WoW to check.
    void dropped(const std::filesystem::path& path);
    // The window's close button. True when it may close now; otherwise a question is shown first.
    bool requestClose();
    bool closeConfirmed() const { return m_closeConfirmed; }
    // Stops everything (the server, a bake, adb) and waits: what is done is kept.
    void shutdown();

private:
    enum class Page : u8 { Home, Language, Wow, Bake, Phone, Adb, About };
    enum class DialogPurpose : u8 { Wow, LocaleClient, Packs, Adb, Apk };
    enum class AdbStep : u8 { None, Tools, Connect, Devices, Install, Push, SendWow };

    // What a refresh found (the settings' clients, the plan, the packs, the programs), applied on the
    // window's thread.
    struct Snapshot {
        bool done = false;
        std::filesystem::path wow; // the settings' WoW it checked
        Clients clients;
        bool clientsChecked = false;
        BakePlan plan;
        std::vector<PackFile> packs;
        u64 packBytes = 0;
        std::string bakedBy;
        std::string bakedLocale;
        u64 freeSpace = 0;
        bool freeKnown = false;
        std::filesystem::path apk;
        bool apkRead = false;
        ApkInfo apkInfo;
        std::string apkError;
        std::filesystem::path adb;
        std::filesystem::path baker;
    };
    struct BakeOutcomeView {
        bool shown = false;
        bool cancelled = false;
        std::vector<std::string> failed;
        std::vector<std::string> baked; // the steps' names, as the player reads them
        std::string error;
        f64 seconds = 0.0;
    };
    struct ServerLine {
        f64 seconds = 0.0;
        Message line;
    };

    // --- State ---
    void saveSettings();
    void refresh(bool checkClients);
    SetupFacts facts() const;
    bool busy() const;           // a task that blocks the guide and the language runs
    bool languageLocked() const; // something reads texts on another thread
    std::filesystem::path packs() const;
    std::string runningSummary() const; // what closing would stop, for the question

    // --- Frame ---
    void drawSidebar(f32 width);
    void drawPage();
    void drawWizardBar();
    void drawCloseQuestion();
    void goTo(Page page);
    void navItem(Page page, std::string_view key, Status status);
    Status pageStatus(Page page) const;

    // --- Pages ---
    void drawHome();
    void drawLanguage();
    void drawWow();
    void drawWowCheck(const ClientCheck& check, bool base);
    void drawLocaleClient();
    void drawBake();
    void drawBakeProgress();
    void drawPhone();
    void drawPhoneServing();
    void drawAdb();
    void drawAdbState(AdbStep step);
    void drawAbout();
    void languageChooser(bool large);

    // --- Operations ---
    void openFolderDialog(DialogPurpose purpose, const std::filesystem::path& start);
    void openFileDialog(DialogPurpose purpose, const std::filesystem::path& start);
    void dialogChosen(DialogPurpose purpose, const std::filesystem::path& path);
    void checkWow(const std::filesystem::path& folder);
    void checkLocaleClient(const std::filesystem::path& folder);
    void startBake();
    void updateOptionsPlan();
    void startServer();
    void stopServer();
    void serverEvent(const ServeEvent& event);
    void adbRefreshDevices();
    void adbSetup();
    void adbPair();
    void adbConnect();
    void adbInstall();
    void adbPush(bool wow);
    std::string chosenSerial() const; // the device chosen, or the only one connected
    Adb adbFor() const;
    void checkUpdates(); // phase 22 (polish)

    SDL_Window* m_window = nullptr;
    WindowOptions m_options;
    std::filesystem::path m_dataDir;
    std::filesystem::path m_settingsFile;
    Settings m_settings;
    std::string m_settingsError;
    Clock::time_point m_start = Clock::now();

    SDL_Texture* m_logo = nullptr;
    f32 m_logoWidth = 0.0f;
    f32 m_logoHeight = 0.0f;

    // Language: the one loaded, and a change waiting for the next frame.
    std::string m_locale;
    std::optional<std::string> m_pendingLanguage; // "" = the system's
    std::string m_languageOverride;               // --lang's, shown until the player picks one

    // Navigation.
    Page m_page = Page::Home;
    bool m_wizard = false;
    WizardStep m_wizardStep = WizardStep::Language;
    bool m_closeQuestion = false;
    bool m_closeConfirmed = false;

    // Folder and file dialogs (their answers come on another thread, through a mailbox: SetupWindow.cpp).
    bool m_dialogOpen = false;
    std::string m_dialogError;

    // Refresh.
    Task m_refreshTask;
    bool m_refreshAgain = false;
    bool m_refreshAgainClients = false;
    Snapshot m_snapshot;

    // The WoW page.
    Task m_wowTask;
    std::optional<ClientCheck> m_candidate;       // the latest folder checked by hand, usable or not
    std::optional<ClientCheck> m_localeCandidate; // the same for the second client
    Message m_localeProblem;                      // why it was refused (an empty key: it was not)
    bool m_checkingLocale = false;                // m_wowTask checks the second client
    char m_wowInput[1024] = {};
    char m_localeInput[1024] = {};

    // Bake.
    Task m_bakeTask;
    bool m_bakeForce = false;
    int m_bakeOnly = 0;                    // 0: every step; else bakeSteps()[n - 1]
    std::optional<BakePlan> m_optionsPlan; // the plan with those two options, when either is set
    bool m_showBakeLog = false;
    BakeOutcomeView m_bakeOutcome;

    // Phone over Wi-Fi.
    std::unique_ptr<PhoneServer> m_server;
    std::vector<LocalAddress> m_addresses;
    int m_addressChoice = 0;
    bool m_serveWow = false;
    std::string m_serveHost; // "192.168.1.20:47800"
    std::string m_serveUrl;
    std::string m_serveCode;
    QrMatrix m_qr;
    std::string m_serveError;
    bool m_serverPreparing = false;
    f64 m_prepareFraction = 0.0;
    std::string m_prepareText;
    bool m_serverReady = false;
    u64 m_readyBytes = 0;
    std::mutex m_eventMutex;
    std::vector<ServeEvent> m_events; // from the server's threads, drained by update()
    PhoneActivityLog m_phones;
    std::deque<ServerLine> m_serverLines;
    bool m_newKeyQuestion = false;
    f64 m_copiedAt = -10.0;

    // ADB.
    Task m_adbTask;
    std::vector<AdbDevice> m_devices;
    bool m_devicesListed = false;
    bool m_acceptTerms = false;
    char m_pairAddress[64] = {};
    char m_pairCode[16] = {};
    char m_connectAddress[64] = {};
    std::vector<Message> m_adbLines;   // the outcome of the latest adb operation
    AdbStep m_adbStep = AdbStep::None; // the step that started it: its state shows there

    // Phase 22 (polish): updates, as `vdsetup status` says them (updateStatusMessage).
    Task m_updateTask;
    std::string m_updateUrl;   // the manifest's
    bool m_updateAuto = false; // this build looks by itself (distribution, or development with VD_UPDATE_URL)
    UpdateCache m_updateCache; // what the latest check found (update.json, shared with vdsetup)
    std::string m_updateError; // why the latest check failed (empty: it worked)
};

} // namespace vd::setup::gui
