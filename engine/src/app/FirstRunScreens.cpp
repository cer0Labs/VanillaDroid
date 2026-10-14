#include "app/FirstRunScreens.h"

#include "app/FirstRunWindows.h"
#include "core/DevEnv.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Version.h"
#include "ui/Context.h"
#include "ui/TextInput.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

namespace vd {
namespace {

using Phase = DownloadStatus::Phase;
using Failure = DownloadStatus::Failure;

constexpr ui::WidgetId kAddressField = ui::widgetId("firstrun.address");
constexpr ui::WidgetId kCodeField = ui::widgetId("firstrun.code");
constexpr u32 kAddressBytes = 21; // 255.255.255.255:65535
constexpr u32 kCodeBytes = 7;     // the six digits, and the space some type in the middle

// "4.5 GB", "389 MB", "12 KB" (the decimal sign is the translation's).
std::string_view formatSize(std::span<char> out, u64 bytes) {
    constexpr u64 kKb = 1024, kMb = kKb * 1024, kGb = kMb * 1024;
    if (bytes >= kGb) {
        const u64 tenths = (bytes * 10 + kGb / 2) / kGb;
        return text::format(out, "VD_PAIR_SIZE_GB", static_cast<u32>(tenths / 10),
                            static_cast<u32>(tenths % 10));
    }
    if (bytes >= kMb) {
        return text::format(out, "VD_PAIR_SIZE_MB", static_cast<u32>((bytes + kMb / 2) / kMb));
    }
    return text::format(out, "VD_PAIR_SIZE_KB", static_cast<u32>(std::max<u64>(1, (bytes + kKb / 2) / kKb)));
}

// "3 min left": the minutes rounded up, the seconds under one.
std::string_view formatLeft(std::span<char> out, u64 seconds) {
    if (seconds >= 3600) {
        const u64 minutes = (seconds + 59) / 60;
        return text::format(out, "VD_PAIR_LEFT_HOURS", static_cast<u32>(minutes / 60),
                            static_cast<u32>(minutes % 60));
    }
    if (seconds >= 60) {
        return text::format(out, "VD_PAIR_LEFT_MINUTES", static_cast<u32>((seconds + 59) / 60));
    }
    return text::format(out, "VD_PAIR_LEFT_SECONDS", static_cast<u32>(std::max<u64>(1, seconds)));
}

template <usize N> void copyText(std::array<char, N>& out, std::string_view text) {
    const usize length = std::min(text.size(), N - 1);
    std::memcpy(out.data(), text.data(), length);
    out[length] = '\0';
}

u32 percentOf(f32 fraction) {
    return static_cast<u32>(std::clamp(fraction, 0.0f, 1.0f) * 100.0f + 0.5f);
}

// Phase 22 (phonebake): the steps of a first bake; development (VD_PHONE_BAKE_STEPS=db,cinematics,bodyfx):
// only those (the screens tried on the desktop without baking for 20 minutes).
std::vector<PhoneBakeJob> firstJobs() {
    std::vector<PhoneBakeJob> jobs = firstBakeJobs();
    if (const char* only = VD_DEV_ENV("VD_PHONE_BAKE_STEPS"); only && *only) {
        const std::string list = std::string(",") + only + ",";
        std::erase_if(jobs, [&](const PhoneBakeJob& job) {
            return list.find("," + job.step + ",") == std::string::npos;
        });
    }
    return jobs;
}

// Phase 22 (phonebake): "about 45 min", "about 1 h 5 min": rounded up to 5 minutes.
std::string_view formatAbout(std::span<char> out, f64 seconds) {
    const u64 minutes = std::max<u64>(5, (static_cast<u64>(std::max(seconds, 0.0)) + 299) / 300 * 5);
    if (minutes < 60) {
        return text::format(out, "VD_PHONE_ABOUT_MINUTES", static_cast<u32>(minutes));
    }
    if (minutes % 60 == 0) {
        return text::format(out, "VD_PHONE_ABOUT_WHOLE_HOURS", static_cast<u32>(minutes / 60));
    }
    return text::format(out, "VD_PHONE_ABOUT_HOURS", static_cast<u32>(minutes / 60),
                        static_cast<u32>(minutes % 60));
}

} // namespace

FirstRunScreens::~FirstRunScreens() {
    shutdown();
}

void FirstRunScreens::init(const Config& config) {
    m_config = config;
    copyText(m_version, kVersion);
#if !defined(VD_DIST)
    if (!config.demo.empty()) {
        m_demo = true;
        startDemo(config.demo);
        m_config.demo = {};
        return;
    }
#endif
    m_hasData = hasGameData(config.packsDir);
    m_wowOnDevice = hasWowOnDevice(config.wowDir);
    setPc(loadPairing(config.storageDir));
    m_download.start({config.packsDir, config.wowDir, config.storageDir / "downloads.json"});
    m_started = true;
    VD_LOG_INFO("First run: game data %s, %s, WoW on the device %s", m_hasData ? "there" : "missing",
                m_pc ? (m_pc->pending ? "an unfinished download" : "a paired PC") : "no paired PC",
                m_wowOnDevice ? "yes" : "no");
}

void FirstRunScreens::shutdown() {
    if (m_started) {
        m_download.stop();
        m_started = false;
    }
}

void FirstRunScreens::setPc(std::optional<PairedPc> pc) {
    m_pc = std::move(pc);
    copyText(m_pcAddress, m_pc ? m_pc->endpoint.text() : std::string());
}

void FirstRunScreens::savePairing() {
    if (m_demo || !m_config.writeFile) {
        return;
    }
    m_config.writeFile(m_config.writer, m_config.storageDir / std::string(kPairingFile),
                       m_pc ? writePairingFile(*m_pc) : writeForgottenPairing());
}

void FirstRunScreens::onLink(std::string_view uri) {
    if (m_demo || m_restart) {
        return;
    }
    std::optional<PairedPc> pc = parsePairingLink(uri);
    if (!pc) {
        VD_LOG_WARN("First run: not a pairing link of the local network");
        m_linkError = true;
        m_failedJob = Job::None;
        m_failure = Failure::None;
        m_back = Page::Home;
        m_page = Page::Failed;
        return;
    }
    // The page's link tapped again for the same PC: what is going on goes on.
    const bool same = m_pc && m_pc->endpoint.host == pc->endpoint.host &&
                      m_pc->endpoint.port == pc->endpoint.port && m_pc->key == pc->key;
    if (same && (m_page == Page::Downloading || (m_page == Page::Busy && m_job == Job::Check))) {
        return;
    }
    VD_LOG_INFO("First run: paired with %s by its link", pc->endpoint.text().c_str());
    pc->includeWow = same && m_pc->includeWow;
    setPc(std::move(pc));
    savePairing();
    startCheck();
}

void FirstRunScreens::connect(ui::TextInput& input) {
    net::HttpEndpoint endpoint;
    if (!net::parseLocalEndpoint(m_address.view(), kSetupPort, endpoint)) {
        m_formError = text::Key("VD_PAIR_ERROR_ADDRESS");
        return;
    }
    const std::string code = pairingCode(m_code.view());
    if (code.empty()) {
        m_formError = text::Key("VD_PAIR_ERROR_CODE");
        return;
    }
    m_formError = text::Key("");
    input.end();
    m_pairingAt = endpoint;
    copyText(m_pairingAddress, endpoint.text());
    m_back = Page::Address;
    m_page = Page::Busy;
    if (m_demo) {
        return;
    }
    m_jobId = m_download.pair(endpoint, code);
    m_job = Job::Pair;
}

void FirstRunScreens::startCheck() {
    m_linkError = false;
    m_back = Page::Home;
    m_page = Page::Busy;
    if (m_demo || !m_pc) {
        return;
    }
    m_jobId = m_download.check(m_pc->endpoint, m_pc->key);
    m_job = Job::Check;
}

void FirstRunScreens::startDownload() {
    m_page = Page::Downloading;
    if (m_demo || !m_pc) {
        return;
    }
    m_pc->pending = true;
    savePairing();
    m_jobId = m_download.download(m_pc->endpoint, m_pc->key, m_pc->includeWow);
    m_job = Job::Download;
}

void FirstRunScreens::play(ui::TextInput& input) {
    input.end();
    if (m_pc && m_pc->pending) {
        m_pc->pending = false; // "play with what is here": the game opens normally from now on
        savePairing();
    }
    // Phase 22 (phonebake): the data is there (baked here or from the PC): a bake on the phone left half way
    // is forgotten, or the game would open on this screen again.
    if (m_config.phoneBake && !m_demo && !m_bake.active() && m_bake.state != PhoneBakeState::None) {
        m_config.phoneBake->discard();
    }
    m_download.cancel();
    m_restart = true;
    VD_LOG_INFO("First run: the game starts over with its data");
}

void FirstRunScreens::forget() {
    m_download.cancel();
    m_job = Job::None;
    setPc(std::nullopt);
    savePairing();
    m_page = Page::Home;
}

void FirstRunScreens::goHome(ui::TextInput& input) {
    if (input.field() == kAddressField || input.field() == kCodeField) {
        input.end();
    }
    m_formError = text::Key("");
    m_linkError = false;
    m_page = Page::Home;
}

void FirstRunScreens::showFailure(Failure failure, std::string_view detail) {
    m_failure = failure;
    m_linkError = false;
    copyText(m_failureDetail, detail);
    m_back = m_failedJob == Job::Pair ? Page::Address : Page::Home;
    m_page = Page::Failed;
}

void FirstRunScreens::follow() {
    if (m_demo || !m_started) {
        return;
    }
    m_status = m_download.status();
    if (m_job == Job::None || m_status.job != m_jobId) {
        return;
    }
    switch (m_status.phase) {
    case Phase::Paired:
        if (m_job == Job::Pair) {
            PairedPc pc;
            pc.endpoint = m_pairingAt;
            pc.key = m_download.pairedKey();
            m_job = Job::None;
            if (!pc.valid()) {
                m_failedJob = Job::Pair;
                showFailure(Failure::NotSetup, {});
                break;
            }
            VD_LOG_INFO("First run: paired with %s by its code", pc.endpoint.text().c_str());
            m_code.wipe();
            setPc(std::move(pc));
            savePairing();
            startCheck();
        }
        break;
    case Phase::Ready:
        if (m_job == Job::Check) {
            m_job = Job::None;
            m_page = Page::Ready;
        }
        break;
    case Phase::Done:
        if (m_job == Job::Download) {
            m_job = Job::None;
            if (m_pc) {
                m_pc->pending = false;
                savePairing();
            }
            m_page = Page::Done;
        }
        break;
    case Phase::Failed:
        m_failedJob = m_job;
        m_job = Job::None;
        showFailure(m_status.failure, m_status.detail.data());
        break;
    case Phase::Cancelled:
        m_page = m_job == Job::Download ? Page::Paused : m_back;
        m_job = Job::None;
        break;
    default: break;
    }
}

bool FirstRunScreens::escape(ui::TextInput& input) {
    switch (m_page) {
    case Page::Home: return false;
    case Page::Downloading:
    case Page::Done:
    case Page::PhoneDone: return true; // the page's own buttons: Back never stops a download by mistake
    case Page::Busy:
        m_download.cancel();
        m_job = Job::None;
        m_page = m_back;
        return true;
    case Page::Failed:
        m_page = m_back;
        m_linkError = false;
        return true;
    default: goHome(input); return true;
    }
}

void FirstRunScreens::draw(DialogContext& c, ui::TextInput& input) {
    VD_PROFILE_ZONE("FirstRunScreens::draw");
    follow();
    followBake(); // phase 22 (phonebake)
    if (m_page != Page::Address && (input.field() == kAddressField || input.field() == kCodeField)) {
        input.end(); // a link arrived while typing: the keyboard goes with the page
    }
    switch (m_page) {
    case Page::Home: drawHome(c, input); break;
    case Page::Qr: drawQr(c, input); break;
    case Page::Address: drawAddress(c, input); break;
    case Page::Adb: drawAdb(c, input); break;
    case Page::Busy: drawBusy(c, input); break;
    case Page::Ready: drawReady(c, input); break;
    case Page::Downloading: drawDownloading(c, input, false); break;
    case Page::Paused: drawDownloading(c, input, true); break;
    case Page::Failed: drawFailed(c, input); break;
    case Page::Done: drawDone(c, input); break;
    case Page::PhoneIntro: drawPhoneIntro(c, input); break;
    case Page::PhoneBaking: drawPhoneBaking(c, input); break;
    case Page::PhoneStopped: drawPhoneStopped(c, input); break;
    case Page::PhoneDone: drawPhoneDone(c, input); break;
    }
}

void FirstRunScreens::drawHome(DialogContext& c, ui::TextInput& input) {
    enum class Action : u8 {
        Resume,
        Download,
        Qr,
        Address,
        Adb,
        Phone,
        PhoneBaking,
        PhoneStopped,
        PhoneDone,
        PlayNow
    };
    std::array<FirstRunButton, 8> buttons{};
    std::array<Action, 8> actions{};
    usize count = 0;
    const auto add = [&](Action action, text::Key label, std::string_view note, bool primary,
                         bool enabled = true) {
        buttons[count] = {text::get(label), note, primary, enabled};
        actions[count++] = action;
    };
    const bool paired = m_pc && m_pc->valid();
    const bool pending = paired && m_pc->pending;
    // Phase 22 (phonebake): a bake on the phone first, while it goes on or waits to go on; the PC's ways wait
    // for it (both would write the packs).
    const bool phone = m_config.phoneBake || m_demo;
    const bool baking = phone && m_bake.active();
    const bool phoneStopped = phone && m_bake.resumable() && m_bake.kind == PhoneBakeKind::First;
    const bool phoneDone = phone && !baking && m_bake.state == PhoneBakeState::Done;
    m_check.clear();
    if (baking) {
        if (m_bake.starting) {
            m_check.add("VD_PHONE_STARTING");
        } else {
            m_check.add("VD_PHONE_OPTION_BAKING_NOTE", m_bake.step + 1, m_bake.stepCount,
                        percentOf(m_bake.fraction));
        }
        add(Action::PhoneBaking, "VD_PHONE_OPTION_BAKING", m_check.view(), true);
    } else if (phoneStopped) {
        m_check.add("VD_PHONE_OPTION_RESUME_NOTE", m_bake.finished, m_bake.stepCount);
        add(Action::PhoneStopped, "VD_PHONE_OPTION_RESUME", m_check.view(), true);
    } else if (phoneDone) {
        add(Action::PhoneDone, "VD_PHONE_OPTION_DONE", {}, true);
    }
    const bool busy = baking || phoneStopped || phoneDone;
    m_note.clear();
    if (paired) {
        m_note.add("VD_PAIR_OPTION_FROM", std::string_view(m_pcAddress.data()));
        add(pending ? Action::Resume : Action::Download,
            pending ? "VD_PAIR_OPTION_RESUME" : "VD_PAIR_OPTION_DOWNLOAD", m_note.view(), !busy, !baking);
    }
    add(Action::Qr, "VD_PAIR_OPTION_QR", text::get("VD_PAIR_OPTION_QR_NOTE"), !paired && !busy, !baking);
    add(Action::Address, "VD_PAIR_OPTION_ADDRESS", text::get("VD_PAIR_OPTION_ADDRESS_NOTE"), false, !baking);
    add(Action::Adb, "VD_PAIR_OPTION_ADB", text::get("VD_PAIR_OPTION_ADB_NOTE"), false);
    if (phone && !busy && (m_wowOnDevice || m_bake.wowFound)) {
        // Active with the 12 game archives there and whole (pack::checkWowData, on PhoneBake's thread).
        std::string_view note = text::get("VD_PAIR_OPTION_PHONE_NOTE");
        if (!m_bake.checked) {
            note = text::get("VD_PHONE_CHECKING");
        } else if (!m_bake.wowComplete) {
            m_check.add("VD_PHONE_OPTION_INCOMPLETE", m_bake.wowMissing + m_bake.wowDamaged);
            note = m_check.view();
        }
        add(Action::Phone, "VD_PAIR_OPTION_PHONE", note, false, m_bake.checked && m_bake.wowComplete);
    }
    if (pending && m_hasData) {
        add(Action::PlayNow, "VD_PAIR_OPTION_PLAY_NOW", text::get("VD_PAIR_OPTION_PLAY_NOW_NOTE"), false);
    }
    FirstRunPageView view;
    view.version = m_version.data();
    view.body = text::get("VD_PAIR_INTRO");
    view.status = pending ? text::get("VD_PAIR_INTERRUPTED") : std::string_view{};
    view.buttons = std::span(buttons.data(), count);
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (r.button < 0) {
        return;
    }
    switch (actions[static_cast<usize>(r.button)]) {
    case Action::Resume: startDownload(); break;
    case Action::Download: startCheck(); break;
    case Action::Qr: m_page = Page::Qr; break;
    case Action::Address:
        m_formError = text::Key("");
        m_page = Page::Address;
        break;
    case Action::Adb: m_page = Page::Adb; break;
    case Action::Phone: openPhoneIntro(); break;
    case Action::PhoneBaking: m_page = Page::PhoneBaking; break;
    case Action::PhoneStopped:
        m_page = Page::PhoneStopped;
        if (m_config.phoneBake && !m_demo) {
            m_config.phoneBake->check(); // the room left, for going on
        }
        break;
    case Action::PhoneDone: m_page = Page::PhoneDone; break;
    case Action::PlayNow: play(input); break;
    }
}

void FirstRunScreens::drawQr(DialogContext& c, ui::TextInput& input) {
    m_body.clear();
    m_body.add("VD_PAIR_QR_STEPS", text::get("VD_SETUP_PAGE_PAIR"));
    const FirstRunButton buttons[] = {{text::get("VD_PAIR_QR_NO_CAMERA"), {}, false, true},
                                      {text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PAIR_QR_TITLE");
    view.body = m_body.view();
    view.buttons = buttons;
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (r.button == 0) {
        m_formError = text::Key("");
        m_page = Page::Address;
    } else if (r.button == 1) {
        goHome(input);
    }
}

void FirstRunScreens::drawAddress(DialogContext& c, ui::TextInput& input) {
    // The field the keyboard types into follows it; the form keeps its text.
    i32 editing = -1;
    if (input.active() && input.field() == kAddressField) {
        m_address.set(input.text());
        editing = 0;
    } else if (input.active() && input.field() == kCodeField) {
        m_code.set(input.text());
        editing = 1;
    }
    if (editing >= 0 && input.submitted()) {
        if (editing == 0) { // the keyboard's "next": on to the code
            input.begin(kCodeField, m_code.view(), kCodeBytes, ui::TextKind::Digits);
            editing = 1;
        } else {
            connect(input);
            editing = -1;
        }
    }
    if (editing >= 0 && input.cancelled()) {
        editing = -1;
    }
    const FirstRunField fields[] = {
        {text::get("VD_PAIR_ADDRESS_LABEL"), m_address.view(), text::get("VD_PAIR_ADDRESS_HINT"),
         editing == 0},
        {text::get("VD_PAIR_CODE_LABEL"), m_code.view(), text::get("VD_PAIR_CODE_HINT"), editing == 1}};
    const FirstRunButton buttons[] = {{text::get("VD_PAIR_CONNECT"), {}, true, true},
                                      {text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PAIR_ADDRESS_TITLE");
    view.fields = fields;
    view.body = text::get("VD_PAIR_ADDRESS_HELP");
    if (!m_formError.name.empty()) {
        view.status = text::get(m_formError);
        view.statusError = true;
    }
    view.buttons = buttons;
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (r.field == 0 && editing != 0) {
        input.begin(kAddressField, m_address.view(), kAddressBytes, ui::TextKind::Plain);
    } else if (r.field == 1 && editing != 1) {
        input.begin(kCodeField, m_code.view(), kCodeBytes, ui::TextKind::Digits);
    }
    if (r.button == 0) {
        connect(input);
    } else if (r.button == 1) {
        goHome(input);
    }
}

void FirstRunScreens::drawAdb(DialogContext& c, ui::TextInput& input) {
    const FirstRunButton buttons[] = {{text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PAIR_ADB_TITLE");
    view.body = text::get("VD_PAIR_ADB_INTRO");
    view.body2 = text::get("VD_SETUP_ADB_GUIDE"); // VanillaDroid Setup's own steps (setup.lang)
    view.buttons = buttons;
    if (drawFirstRunPage(c, view).button == 0) {
        goHome(input);
    }
}

void FirstRunScreens::statusLine(const DownloadStatus& status) {
    m_status1.clear();
    switch (status.phase) {
    case Phase::Waiting:
        if (status.detail[0] != '\0') {
            m_status1.add("VD_PAIR_RETRYING", std::string_view(status.detail.data()), status.retrySeconds);
        } else {
            m_status1.add("VD_PAIR_RETRYING_SHORT", status.retrySeconds);
        }
        break;
    case Phase::Preparing: m_status1.add("VD_PAIR_PREPARING", percentOf(status.fraction)); break;
    case Phase::Checking: m_status1.add("VD_PAIR_CHECKING", percentOf(status.fraction)); break;
    case Phase::Contacting:
        m_status1.add("VD_PAIR_CONTACTING",
                      std::string_view(m_job == Job::Pair ? m_pairingAddress.data() : m_pcAddress.data()));
        break;
    default: break;
    }
}

void FirstRunScreens::drawBusy(DialogContext& c, ui::TextInput& input) {
    static_cast<void>(input);
    statusLine(m_status.phase == Phase::Idle ? DownloadStatus{} : m_status);
    if (m_status1.length == 0) {
        m_status1.add("VD_PAIR_CONTACTING",
                      std::string_view(m_job == Job::Pair ? m_pairingAddress.data() : m_pcAddress.data()));
    }
    const FirstRunButton buttons[] = {{text::get("VD_PAIR_CANCEL"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PAIR_QR_TITLE");
    view.busy = true;
    view.body = m_status1.view();
    if (m_status.phase == Phase::Preparing || m_status.phase == Phase::Checking) {
        view.progress = true;
        view.fraction = m_status.fraction;
    }
    view.buttons = buttons;
    if (drawFirstRunPage(c, view).button == 0) {
        m_download.cancel();
        m_job = Job::None;
        m_page = m_back;
    }
}

void FirstRunScreens::drawReady(DialogContext& c, ui::TextInput& input) {
    const DownloadStatus& s = m_status;
    const bool wow = m_pc && m_pc->includeWow && s.wowOffered;
    const bool nothing = s.packFiles == 0 && (!wow || s.wowFiles == 0);
    char size[32], room[32];
    m_body.clear();
    m_body.add("VD_PAIR_LINKED", std::string_view(m_pcAddress.data()));
    m_body.raw("\n\n");
    if (nothing) {
        m_body.add("VD_PAIR_UP_TO_DATE");
    } else {
        m_body.add("VD_PAIR_TO_DOWNLOAD", formatSize(size, s.packBytes + (wow ? s.wowBytes : 0)),
                   s.packFiles + (wow ? s.wowFiles : 0));
    }
    m_check.clear();
    if (s.wowOffered && s.wowFiles > 0) {
        m_check.add("VD_PAIR_WOW_OPTION", formatSize(size, s.wowBytes));
    }
    const u64 needed = s.packBytes + (wow ? s.wowBytes : 0) + DataDownload::kSpareBytes;
    const bool room0 = needed <= s.freeBytes;
    m_status1.clear();
    if (room0 || nothing) {
        m_status1.add("VD_PAIR_FREE_SPACE", formatSize(room, s.freeBytes));
    } else {
        m_status1.add("VD_PAIR_NO_SPACE", formatSize(size, needed), formatSize(room, s.freeBytes));
    }
    const FirstRunButton buttons[] = {
        {text::get(nothing ? "VD_PAIR_PLAY" : "VD_PAIR_DOWNLOAD"), {}, true, room0 || nothing},
        {text::get("VD_PAIR_FORGET"), {}, false, true},
        {text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PAIR_READY_TITLE");
    view.body = m_body.view();
    view.hasCheck = m_check.length > 0;
    view.checkLabel = m_check.view();
    view.checked = wow;
    view.status = m_status1.view();
    view.statusError = !room0 && !nothing;
    view.buttons = buttons;
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (r.toggled && m_pc) {
        m_pc->includeWow = !m_pc->includeWow;
    }
    if (r.button == 0) {
        startDownload(); // with nothing to fetch it only tidies up and ends: then Play
    } else if (r.button == 1) {
        forget();
    } else if (r.button == 2) {
        goHome(input);
    }
}

void FirstRunScreens::drawDownloading(DialogContext& c, ui::TextInput& input, bool paused) {
    const DownloadStatus& s = m_status;
    char done[32], total[32], speed[32], left[48];
    m_progressTop.clear();
    if (s.fileCount > 0 && s.file[0] != '\0') {
        m_progressTop.add("VD_PAIR_FILE", std::string_view(s.file.data()), s.fileIndex, s.fileCount);
    }
    m_progressLeft.clear();
    if (s.totalBytes > 0) {
        m_progressLeft.add("VD_PAIR_DONE_OF", formatSize(done, s.doneBytes), formatSize(total, s.totalBytes));
    }
    m_progressRight.clear();
    if (!paused && s.phase == Phase::Downloading && s.bytesPerSecond > 1.0f && s.totalBytes > s.doneBytes) {
        const u64 seconds = static_cast<u64>(static_cast<f64>(s.totalBytes - s.doneBytes) / s.bytesPerSecond);
        m_progressRight.add("VD_PAIR_SPEED", formatSize(speed, static_cast<u64>(s.bytesPerSecond)),
                            formatLeft(left, seconds));
    }
    if (paused) {
        m_status1.clear();
    } else {
        statusLine(s);
    }
    const FirstRunButton downloading[] = {{text::get("VD_PAIR_PAUSE"), {}, false, true}};
    const FirstRunButton stopped[] = {{text::get("VD_PAIR_RESUME"), {}, true, true},
                                      {text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get(paused ? "VD_PAIR_PAUSED_TITLE" : "VD_PAIR_DOWNLOADING_TITLE");
    view.progress = true;
    view.indeterminate = !paused && s.totalBytes == 0;
    view.fraction = s.totalBytes > 0
                        ? static_cast<f32>(static_cast<f64>(s.doneBytes) / static_cast<f64>(s.totalBytes))
                        : 0.0f;
    view.progressTop = m_progressTop.view();
    view.progressLeft = m_progressLeft.view();
    view.progressRight = m_progressRight.view();
    view.body = text::get(paused ? "VD_PAIR_PAUSED" : "VD_PAIR_KEEP_OPEN");
    view.status = m_status1.view();
    view.buttons =
        paused ? std::span<const FirstRunButton>(stopped) : std::span<const FirstRunButton>(downloading);
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (!paused && r.button == 0) {
        m_download.cancel(); // the .part files stay: Resume goes on from them
        m_job = Job::None;
        m_page = Page::Paused;
    } else if (paused && r.button == 0) {
        startDownload();
    } else if (paused && r.button == 1) {
        goHome(input);
    }
}

void FirstRunScreens::drawFailed(DialogContext& c, ui::TextInput& input) {
    char needed[32], room[32];
    const std::string_view address = m_failedJob == Job::Pair ? std::string_view(m_pairingAddress.data())
                                                              : std::string_view(m_pcAddress.data());
    m_body.clear();
    if (m_linkError) {
        m_body.add("VD_PAIR_ERROR_LINK");
    } else {
        switch (m_failure) {
        case Failure::Unreachable: m_body.add("VD_PAIR_ERROR_UNREACHABLE", address); break;
        case Failure::NotSetup: m_body.add("VD_PAIR_ERROR_NOT_SETUP", address); break;
        case Failure::WrongCode: m_body.add("VD_PAIR_ERROR_WRONG_CODE"); break;
        case Failure::CodeLocked: m_body.add("VD_PAIR_ERROR_CODE_LOCKED"); break;
        case Failure::Unauthorized: m_body.add("VD_PAIR_ERROR_UNAUTHORIZED"); break;
        case Failure::NoSpace:
            m_body.add("VD_PAIR_NO_SPACE", formatSize(needed, m_status.neededBytes),
                       formatSize(room, m_status.freeBytes));
            break;
        case Failure::NoPacks: m_body.add("VD_PAIR_ERROR_NO_PACKS"); break;
        case Failure::BadList: m_body.add("VD_PAIR_ERROR_BAD_LIST"); break;
        case Failure::Damaged:
            m_body.add("VD_PAIR_ERROR_DAMAGED", std::string_view(m_failureDetail.data()));
            break;
        case Failure::Disk: m_body.add("VD_PAIR_ERROR_DISK", std::string_view(m_failureDetail.data())); break;
        case Failure::None: break;
        }
    }
    const bool pairAgain = !m_linkError && m_failure == Failure::Unauthorized;
    const FirstRunButton retry[] = {
        {text::get(pairAgain ? "VD_PAIR_PAIR_AGAIN" : "VD_PAIR_TRY_AGAIN"), {}, true, true},
        {text::get("VD_PAIR_BACK"), {}, false, true}};
    const FirstRunButton back[] = {{text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PAIR_FAILED_TITLE");
    view.body = m_body.view();
    view.buttons =
        m_linkError ? std::span<const FirstRunButton>(back) : std::span<const FirstRunButton>(retry);
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (r.button < 0) {
        return;
    }
    if (m_linkError || r.button == 1) {
        m_linkError = false;
        m_page = m_back;
        if (m_page == Page::Home) {
            goHome(input);
        }
        return;
    }
    if (pairAgain) {
        forget();
        return;
    }
    switch (m_failedJob) {
    case Job::Pair: m_page = Page::Address; break;
    case Job::Check: startCheck(); break;
    case Job::Download: startDownload(); break;
    case Job::None: goHome(input); break;
    }
}

void FirstRunScreens::drawDone(DialogContext& c, ui::TextInput& input) {
    const FirstRunButton buttons[] = {{text::get("VD_PAIR_PLAY"), {}, true, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PAIR_DONE_TITLE");
    view.progress = true;
    view.fraction = 1.0f;
    view.body = text::get("VD_PAIR_DONE");
    view.buttons = buttons;
    if (drawFirstRunPage(c, view).button == 0) {
        play(input);
    }
}

// --- Phase 22 (phonebake): the bake on the phone
// ------------------------------------------------------------------------------------

void FirstRunScreens::followBake() {
    if (m_demo || !m_config.phoneBake) {
        return;
    }
    m_bake = m_config.phoneBake->status();
    if (m_bake.sequence == 0) {
        return; // its thread has not looked yet
    }
    if (!m_bakeFollowed) {
        // Opened while a first bake runs, or after it ended with the app away: its page.
        m_bakeFollowed = true;
        if (m_page == Page::Home && m_bake.kind == PhoneBakeKind::First) {
            if (m_bake.active()) {
                m_page = Page::PhoneBaking;
            } else if (m_bake.state == PhoneBakeState::Done) {
                m_page = Page::PhoneDone;
            }
        }
    }
    switch (m_page) {
    case Page::PhoneBaking:
        if (m_bake.active()) {
            break;
        }
        if (m_bake.state == PhoneBakeState::Done) {
            m_page = Page::PhoneDone;
        } else if (m_bake.state == PhoneBakeState::None) {
            m_page = Page::Home; // the plan is gone
        } else {
            m_page = Page::PhoneStopped; // failed, cancelled, stopped by Android
            m_config.phoneBake->check(); // the room left, for going on
        }
        break;
    case Page::PhoneStopped:
        if (m_bake.active()) {
            m_page = Page::PhoneBaking;
        } else if (m_bake.state == PhoneBakeState::Done) {
            m_page = Page::PhoneDone;
        }
        break;
    default: break;
    }
}

void FirstRunScreens::openPhoneIntro() {
    m_page = Page::PhoneIntro;
    if (m_config.phoneBake && !m_demo) {
        m_config.phoneBake->check(); // the WoW and the room as they are now
    }
}

void FirstRunScreens::phoneStepLine() {
    m_progressTop.clear();
    if (m_bake.stepCount > 0 && m_bake.step < m_bake.stepCount) {
        m_progressTop.add("VD_PHONE_STEP_OF", phoneBakeStepText(m_bake.stepName.data()), m_bake.step + 1,
                          m_bake.stepCount);
    }
}

void FirstRunScreens::drawPhoneIntro(DialogContext& c, ui::TextInput& input) {
    const PhoneBakeStatus& b = m_bake;
    char about[48], needed[32], room[32];
    m_body.clear();
    m_body.add("VD_PHONE_INTRO", formatAbout(about, b.firstBakeSeconds),
               formatSize(needed, b.firstBakeBytes));
    m_status1.clear();
    bool ready = false;
    bool error = false;
    if (!b.checked) {
        m_status1.add("VD_PHONE_CHECKING");
    } else if (!b.wowComplete) {
        m_status1.add("VD_PHONE_WOW_INCOMPLETE", std::string_view(b.wowProblem.data()));
        error = true;
    } else if (b.freeBytes < b.firstBakeBytes) {
        m_status1.add("VD_PHONE_NO_SPACE", formatSize(needed, b.firstBakeBytes),
                      formatSize(room, b.freeBytes));
        error = true;
    } else {
        m_status1.add("VD_PAIR_FREE_SPACE", formatSize(room, b.freeBytes));
        ready = true;
    }
    const FirstRunButton buttons[] = {{text::get("VD_PHONE_START"), {}, true, ready && !b.active()},
                                      {text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PHONE_INTRO_TITLE");
    view.body = m_body.view();
    view.status = m_status1.view();
    view.statusError = error;
    view.buttons = buttons;
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (r.button == 0) {
        if (m_demo) {
            m_page = Page::PhoneBaking;
        } else if (m_config.phoneBake && m_config.phoneBake->start(firstJobs(), PhoneBakeKind::First)) {
            VD_LOG_INFO("First run: the data is prepared on this phone");
            m_bakeFollowed = true;
            m_bake.starting = true;
            m_page = Page::PhoneBaking;
        }
    } else if (r.button == 1) {
        goHome(input);
    }
}

void FirstRunScreens::drawPhoneBaking(DialogContext& c, ui::TextInput& input) {
    const PhoneBakeStatus& b = m_bake;
    char left[48];
    phoneStepLine();
    m_progressLeft.clear();
    m_progressRight.clear();
    m_status1.clear();
    if (b.starting) {
        m_status1.add("VD_PHONE_STARTING");
    } else {
        m_progressLeft.add("VD_PHONE_PERCENT", percentOf(b.fraction));
        if (b.state == PhoneBakeState::Running && b.secondsLeft >= 1.0) {
            m_progressRight.raw(formatLeft(left, static_cast<u64>(b.secondsLeft)));
        }
        if (b.state == PhoneBakeState::Cooling) {
            m_status1.add("VD_PHONE_COOLING");
        } else if (b.jobs > 0 && b.jobs < b.maxJobs) {
            m_status1.add("VD_PHONE_WARM", static_cast<u32>(b.jobs), static_cast<u32>(b.maxJobs));
        }
    }
    const FirstRunButton buttons[] = {{text::get("VD_PHONE_STOP"), {}, false, !b.starting},
                                      {text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PHONE_BAKING_TITLE");
    view.progress = true;
    view.indeterminate = b.starting;
    view.fraction = b.fraction;
    view.progressTop = m_progressTop.view();
    view.progressLeft = m_progressLeft.view();
    view.progressRight = m_progressRight.view();
    view.body = text::get("VD_PHONE_BAKING");
    view.status = m_status1.view();
    view.buttons = buttons;
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (r.button == 0) {
        if (m_demo) {
            m_bake.state = PhoneBakeState::Cancelled;
            m_page = Page::PhoneStopped;
        } else if (m_config.phoneBake) {
            m_config.phoneBake->cancel(); // the page follows the status to "stopped"
        }
    } else if (r.button == 1) {
        goHome(input); // the bake goes on
    }
}

void FirstRunScreens::drawPhoneStopped(DialogContext& c, ui::TextInput& input) {
    const PhoneBakeStatus& b = m_bake;
    const bool failed = b.state == PhoneBakeState::Failed;
    m_body.clear();
    if (failed) {
        m_body.add("VD_PHONE_FAILED", phoneBakeStepText(b.stepName.data()), std::string_view(b.error.data()));
    } else if (b.launchFailed) {
        m_body.add("VD_PHONE_NOT_STARTED");
    } else if (b.state == PhoneBakeState::Interrupted) {
        m_body.add("VD_PHONE_INTERRUPTED");
    } else {
        m_body.add("VD_PHONE_CANCELLED");
    }
    m_body.raw("\n\n");
    m_body.add("VD_PHONE_KEPT", b.finished, b.stepCount);
    m_progressLeft.clear();
    m_progressLeft.add("VD_PHONE_PERCENT", percentOf(b.fraction));
    // The room the steps left need, against what is free now (check() on the way here).
    char needed[32], room[32];
    m_status1.clear();
    const bool room0 = !b.checked || b.neededBytes <= b.freeBytes;
    if (!room0) {
        m_status1.add("VD_PHONE_NO_SPACE", formatSize(needed, b.neededBytes), formatSize(room, b.freeBytes));
    }
    const FirstRunButton buttons[] = {
        {text::get(failed ? "VD_PAIR_TRY_AGAIN" : "VD_PAIR_RESUME"), {}, true, !b.active() && room0},
        {text::get("VD_PHONE_START_OVER"), {}, false, !b.active()},
        {text::get("VD_PAIR_BACK"), {}, false, true}};
    FirstRunPageView view;
    view.title = text::get(failed ? "VD_PAIR_FAILED_TITLE" : "VD_PHONE_STOPPED_TITLE");
    view.progress = true;
    view.fraction = b.fraction;
    view.progressLeft = m_progressLeft.view();
    view.body = m_body.view();
    view.status = m_status1.view();
    view.statusError = !room0;
    view.buttons = buttons;
    const FirstRunInput r = drawFirstRunPage(c, view);
    if (r.button == 0) {
        if (m_demo) {
            m_page = Page::PhoneBaking;
        } else if (m_config.phoneBake && m_config.phoneBake->resume()) {
            m_bake.starting = true;
            m_page = Page::PhoneBaking;
        }
    } else if (r.button == 1) {
        // Everything again (another WoW copied meanwhile, a bake that keeps failing): the explanation first.
        if (m_config.phoneBake && !m_demo) {
            m_config.phoneBake->discard();
        }
        openPhoneIntro();
    } else if (r.button == 2) {
        goHome(input);
    }
}

void FirstRunScreens::drawPhoneDone(DialogContext& c, ui::TextInput& input) {
    const FirstRunButton buttons[] = {{text::get("VD_PAIR_PLAY"), {}, true, true}};
    FirstRunPageView view;
    view.title = text::get("VD_PAIR_DONE_TITLE");
    view.progress = true;
    view.fraction = 1.0f;
    view.body = text::get("VD_PHONE_DONE");
    view.buttons = buttons;
    if (drawFirstRunPage(c, view).button == 0) {
        play(input);
    }
}

#if !defined(VD_DIST)
void FirstRunScreens::startDemo(std::string_view page) {
    // Development (VD_PAIRING_DEMO=<page>): the pages with sample data, for screenshots; nothing is asked or
    // written.
    PairedPc pc;
    pc.endpoint.host = "192.168.1.20";
    pc.endpoint.port = kSetupPort;
    pc.key = std::string(32, 'a');
    m_pairingAt = pc.endpoint;
    copyText(m_pairingAddress, pc.endpoint.text());
    DownloadStatus& s = m_status;
    s = DownloadStatus{};
    s.packFiles = 101;
    s.packBytes = 4831838208ull; // 4.5 GB
    s.wowOffered = true;
    s.wowFiles = 12;
    s.wowBytes = 5422520320ull; // 5.05 GB
    s.freeBytes = 193273528320ull;
    s.totalBytes = s.packBytes;
    s.doneBytes = 2040109466ull;
    s.fileIndex = 63;
    s.fileCount = 101;
    s.bytesPerSecond = 41.5f * 1048576.0f;
    copyText(s.file, "textures.vdpk");
    const auto paired = [&](bool pending) {
        pc.pending = pending;
        setPc(pc);
    };
    if (page == "home2") { // a download left half way, the WoW on the phone
        paired(true);
        m_hasData = true;
        m_wowOnDevice = true;
    } else if (page == "qr") {
        m_page = Page::Qr;
    } else if (page == "address" || page == "address2") {
        m_page = Page::Address;
        m_address.set("192.168.1.20:47800");
        if (page == "address2") {
            m_code.set("12 34");
            m_formError = text::Key("VD_PAIR_ERROR_CODE");
        }
    } else if (page == "adb") {
        m_page = Page::Adb;
    } else if (page == "busy" || page == "preparing") {
        paired(false);
        m_page = Page::Busy;
        m_job = Job::Check;
        s.phase = page == "busy" ? Phase::Contacting : Phase::Preparing;
        s.fraction = 0.42f;
    } else if (page == "ready" || page == "ready2") {
        paired(false);
        m_page = Page::Ready;
        s.phase = Phase::Ready;
        if (page == "ready2") { // the WoW too, and not enough room for it
            m_pc->includeWow = true;
            s.freeBytes = 6012954214ull;
        }
    } else if (page == "downloading" || page == "retrying") {
        paired(true);
        m_page = Page::Downloading;
        m_job = Job::Download;
        s.phase = page == "downloading" ? Phase::Downloading : Phase::Waiting;
        s.retrySeconds = 8;
        copyText(s.detail, "Connection to 192.168.1.20:47800 timed out");
    } else if (page == "paused") {
        paired(true);
        m_page = Page::Paused;
        s.phase = Phase::Cancelled;
    } else if (page == "failed") {
        paired(false);
        m_failedJob = Job::Check;
        m_page = Page::Failed;
        m_failure = Failure::Unreachable;
    } else if (page == "link") {
        m_page = Page::Failed;
        m_linkError = true;
    } else if (page == "done") {
        paired(false);
        m_page = Page::Done;
    }
    // Phase 22 (phonebake): the bake on the phone, with the costs measured on a Mi 11X.
    PhoneBakeStatus& b = m_bake;
    b = PhoneBakeStatus{};
    b.sequence = 1;
    b.checked = true;
    b.wowFound = true;
    b.wowComplete = true;
    b.wowBytes = s.wowBytes;
    b.freeBytes = s.freeBytes;
    b.firstBakeBytes = 4'990'000'000ull;
    b.firstBakeSeconds = 2512.0;
    b.stepCount = 13;
    b.maxJobs = 7;
    b.jobs = 7;
    m_bakeFollowed = true;
    const auto baking = [&](u32 step, std::string_view name, f32 fraction) {
        m_wowOnDevice = true;
        b.state = PhoneBakeState::Running;
        b.step = step;
        b.finished = step;
        copyText(b.stepName, name);
        b.fraction = fraction;
        b.secondsLeft = 1450.0;
    };
    if (page == "home2" || page == "home3") {
        m_wowOnDevice = true;
        if (page == "home3") { // the WoW cut short
            b.wowComplete = false;
            b.wowMissing = 1;
            b.wowDamaged = 1;
        }
    } else if (page == "phone" || page == "phone2") {
        m_wowOnDevice = true;
        m_page = Page::PhoneIntro;
        if (page == "phone2") {
            b.freeBytes = 3'435'973'837ull; // 3.2 GB
        }
    } else if (page == "baking" || page == "warm" || page == "cooling" || page == "starting") {
        baking(1, "textures", 0.31f);
        b.stepFraction = 0.42f;
        m_page = Page::PhoneBaking;
        if (page == "warm") {
            b.jobs = 3;
        } else if (page == "cooling") {
            b.state = PhoneBakeState::Cooling;
            b.jobs = 0;
        } else if (page == "starting") {
            b.state = PhoneBakeState::None;
            b.starting = true;
            b.fraction = 0.0f;
        }
    } else if (page == "stopped" || page == "phonefailed" || page == "interrupted") {
        baking(4, "world", 0.83f);
        m_page = Page::PhoneStopped;
        b.state = page == "stopped"
                      ? PhoneBakeState::Cancelled
                      : (page == "phonefailed" ? PhoneBakeState::Failed : PhoneBakeState::Interrupted);
        copyText(b.error, "world_kalimdor: cannot read World\\Maps\\Kalimdor\\Kalimdor_32_48.adt");
    } else if (page == "phonehome") { // the home page while it bakes
        baking(1, "textures", 0.31f);
    } else if (page == "phonedone") {
        b.state = PhoneBakeState::Done;
        b.finished = 13;
        b.fraction = 1.0f;
        m_page = Page::PhoneDone;
    }
    VD_LOG_INFO("VD_PAIRING_DEMO=%.*s", static_cast<int>(page.size()), page.data());
}
#endif

} // namespace vd
