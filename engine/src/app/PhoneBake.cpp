#include "app/PhoneBake.h"

#include "app/PhoneBakeRunner.h"
#include "core/Log.h"
#include "core/Profile.h"
#include "core/Text.h"
#include "pack/WowArchives.h"
#include "platform/Thread.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iterator>

namespace vd {
namespace {

template <usize N> void copyText(std::array<char, N>& out, std::string_view text) {
    const usize length = std::min(text.size(), N - 1);
    std::memcpy(out.data(), text.data(), length);
    out[length] = '\0';
}

bool readText(const std::filesystem::path& path, std::string& text) {
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    text.assign(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    return true;
}

bool writeAtomically(const std::filesystem::path& path, const std::string& text) {
    std::filesystem::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file.write(text.data(), static_cast<std::streamsize>(text.size()));
        if (!file) {
            return false;
        }
    }
    std::error_code error;
    std::filesystem::rename(temp, path, error);
    return !error;
}

// A step's label in the notification: "Textures (2 of 13)".
std::string stepLabel(std::string_view step, u32 index, u32 count) {
    char label[96];
    return std::string(text::format(label, "VD_PHONE_STEP_OF", phoneBakeStepText(step), index + 1, count));
}

PhoneBakeTexts notificationTexts() {
    PhoneBakeTexts texts;
    texts.channel = text::get("VD_PHONE_NOTIFY_CHANNEL");
    texts.title = text::get("VD_PHONE_NOTIFY_TITLE");
    texts.cancel = text::get("VD_PHONE_NOTIFY_CANCEL");
    texts.cooling = text::get("VD_PHONE_NOTIFY_COOLING");
    texts.done = text::get("VD_PHONE_NOTIFY_DONE");
    texts.failed = text::get("VD_PHONE_NOTIFY_FAILED");
    texts.stopped = text::get("VD_PHONE_NOTIFY_STOPPED");
    return texts;
}

constexpr i64 kStartTimeoutMs = 20'000; // the service answers within this, or it did not start

} // namespace

std::string_view phoneBakeStepText(std::string_view step) {
    struct Name {
        std::string_view step;
        text::Key key;
    };
    static constexpr Name kNames[] = {
        {"db", "VD_PHONE_STEP_DB"},           {"textures", "VD_PHONE_STEP_TEXTURES"},
        {"models", "VD_PHONE_STEP_MODELS"},   {"actors", "VD_PHONE_STEP_ACTORS"},
        {"world", "VD_PHONE_STEP_WORLD"},     {"collision", "VD_PHONE_STEP_COLLISION"},
        {"objects", "VD_PHONE_STEP_OBJECTS"}, {"ui", "VD_PHONE_STEP_UI"},
        {"sound", "VD_PHONE_STEP_SOUND"},     {"cinematics", "VD_PHONE_STEP_CINEMATICS"},
        {"grass", "VD_PHONE_STEP_GRASS"},     {"worldsound", "VD_PHONE_STEP_WORLDSOUND"},
        {"bodyfx", "VD_PHONE_STEP_BODYFX"},
    };
    for (const Name& name : kNames) {
        if (name.step == step) {
            return text::get(name.key);
        }
    }
    return step;
}

PhoneBake::Config PhoneBake::configFor(const PlatformInfo& platform) {
    Config config;
    std::error_code error;
    std::filesystem::path packs = std::filesystem::absolute(std::filesystem::path(platform.packsDir), error);
    packs = packs.lexically_normal();
    if (packs.filename().empty()) {
        packs = packs.parent_path(); // "packs/" -> "packs"
    }
    config.places.packsDir = packs;
    config.places.wowDir = packs.parent_path() / "wow";
    config.places.bakeDir = packs.parent_path() / "bake";
#if defined(__ANDROID__)
    config.launcher = makeAndroidBakeLauncher(platform);
#elif defined(_WIN32)
    config.launcher = makeDesktopBakeLauncher(findDesktopBaker());
#endif
    return config;
}

PhoneBake::~PhoneBake() {
    shutdown();
}

void PhoneBake::init(Config config) {
    shutdown();
    m_config = std::move(config);
    m_cores = m_config.cores > 0 ? m_config.cores : std::max(1u, std::thread::hardware_concurrency());
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = false;
        m_requests.clear();
        m_status = PhoneBakeStatus{};
    }
    m_plan.reset();
    m_run = PhoneBakeRun{};
    m_working = PhoneBakeStatus{};
    m_requestedMs = 0;
    m_launchFailed = false;
    m_thread = std::thread([this] { run(); });
}

void PhoneBake::shutdown() {
    if (!m_thread.joinable()) {
        return;
    }
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_quit = true;
    }
    m_wake.notify_all();
    m_thread.join();
    m_config.launcher.reset(); // the desktop's runner stops with the game; Android's service goes on
}

bool PhoneBake::start(std::vector<PhoneBakeJob> jobs, PhoneBakeKind kind) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_thread.joinable() || jobs.empty() || m_status.active()) {
        return false;
    }
    Request request;
    request.command = Command::Start;
    request.jobs = std::move(jobs);
    request.kind = kind;
    m_requests.push_back(std::move(request));
    m_status.starting = true; // until the thread says more: a second tap starts nothing
    ++m_status.sequence;
    m_wake.notify_all();
    return true;
}

bool PhoneBake::resume() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_thread.joinable() || m_status.active() || m_status.stepCount == 0) {
        return false;
    }
    m_requests.push_back({Command::Resume, {}, {}});
    m_status.starting = true;
    ++m_status.sequence;
    m_wake.notify_all();
    return true;
}

void PhoneBake::cancel() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requests.push_back({Command::Cancel, {}, {}});
    m_wake.notify_all();
}

void PhoneBake::discard() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requests.push_back({Command::Discard, {}, {}});
    m_wake.notify_all();
}

void PhoneBake::check() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_requests.push_back({Command::Check, {}, {}});
    m_wake.notify_all();
}

PhoneBakeStatus PhoneBake::status() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_status;
}

void PhoneBake::run() {
    setCurrentThreadName("vd-bake");
    VD_PROFILE_THREAD_NAME("vd-bake");
    checkDevice();
    refresh();
    publish();
    for (;;) {
        Request request;
        bool quit = false;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // Twice a second while a bake runs; now and then otherwise (a service Android started again).
            const auto wait = std::chrono::milliseconds(m_working.active() ? 500 : (m_plan ? 3000 : 10000));
            m_wake.wait_for(lock, wait, [this] { return m_quit || !m_requests.empty(); });
            quit = m_quit;
            if (quit) {
                // What was asked before the end still happens (a cancel, the discard of Play), but nothing
                // starts.
                std::vector<Request> left = std::move(m_requests);
                m_requests.clear();
                lock.unlock();
                for (Request& last : left) {
                    if (last.command == Command::Cancel || last.command == Command::Discard) {
                        execute(last);
                    }
                }
                break;
            }
            if (!m_requests.empty()) {
                request = std::move(m_requests.front());
                m_requests.erase(m_requests.begin());
            }
        }
        if (request.command != Command::None) {
            execute(request);
        }
        refresh();
        publish();
    }
    if (m_config.launcher) {
        m_config.launcher->threadEnds();
    }
}

void PhoneBake::execute(Request& request) {
    const PhoneBakePlaces& places = m_config.places;
    std::error_code error;
    switch (request.command) {
    case Command::None: break;
    case Command::Start: startPlan(request); break;
    case Command::Resume:
        if (!m_plan || !m_config.launcher) {
            m_launchFailed = true;
            break;
        }
        VD_LOG_INFO("Phone bake: going on from step %u of %zu", phoneBakeResumeStep(*m_plan, m_run) + 1,
                    m_plan->steps.size());
        m_requestedMs = phoneBakeNowMs();
        m_launchFailed = !m_config.launcher->start(places.bakeDir);
        if (m_launchFailed) {
            m_requestedMs = 0;
        }
        break;
    case Command::Cancel:
        VD_LOG_INFO("Phone bake: cancelled by the player");
        if (m_config.launcher) {
            m_config.launcher->cancel(places.bakeDir);
        }
        m_requestedMs = 0;
        break;
    case Command::Discard:
        if (m_working.active() && !m_working.stale && m_config.launcher) {
            m_config.launcher->cancel(places.bakeDir);
        }
        VD_LOG_INFO("Phone bake: the unfinished bake is forgotten");
        std::filesystem::remove(places.planFile(), error);
        std::filesystem::remove(places.statusFile(), error);
        std::filesystem::remove_all(places.stagingDir(), error);
        m_plan.reset();
        m_run = PhoneBakeRun{};
        m_requestedMs = 0;
        m_launchFailed = false;
        break;
    case Command::Check: checkDevice(); break;
    }
}

void PhoneBake::startPlan(Request& request) {
    const PhoneBakePlaces& places = m_config.places;
    checkDevice(); // the unofficial archives to leave out, the packs there now
    static u32 counter = 0;
    PhoneBakePlanInputs inputs;
    inputs.places = places;
    inputs.kind = request.kind;
    inputs.nowMs = phoneBakeNowMs();
    inputs.id = std::to_string(inputs.nowMs) + "-" + std::to_string(++counter);
    inputs.cores = m_cores;
    inputs.skipArchives = m_skipArchives;
    inputs.existing = listClientPacks(places.packsDir);
    inputs.texts = notificationTexts();
    inputs.label = stepLabel;
    PhoneBakePlan plan = makePhoneBakePlan(request.jobs, inputs);
    std::error_code error;
    std::filesystem::create_directories(places.bakeDir, error);
    std::filesystem::remove(places.statusFile(), error);
    std::filesystem::remove_all(places.stagingDir(), error); // what an older bake left, its cache too
    if (!writeAtomically(places.planFile(), writePhoneBakePlan(plan))) {
        VD_LOG_ERROR("Phone bake: cannot write %s", places.planFile().string().c_str());
        m_launchFailed = true;
        return;
    }
    VD_LOG_INFO(
        "Phone bake: %s bake of %zu steps (%.0f s on a Mi 11X, %.1f GB at its peak, %.1f GB free), %u "
        "archives left out",
        plan.kind == PhoneBakeKind::First ? "a first" : "an update", plan.steps.size(),
        phoneBakeSeconds(plan), static_cast<f64>(phoneBakeSpaceNeeded(plan)) / 1e9,
        static_cast<f64>(m_working.freeBytes) / 1e9, static_cast<u32>(m_skipArchives.size()));
    m_plan = std::move(plan);
    m_run = PhoneBakeRun{};
    m_requestedMs = phoneBakeNowMs();
    m_launchFailed = !m_config.launcher || !m_config.launcher->start(places.bakeDir);
    if (m_launchFailed) {
        m_requestedMs = 0;
        VD_LOG_ERROR("Phone bake: the platform cannot run it");
    }
}

void PhoneBake::checkDevice() {
    VD_PROFILE_ZONE("PhoneBake::checkDevice");
    const PhoneBakePlaces& places = m_config.places;
    PhoneBakeStatus& s = m_working;
    s.checked = true;
    const std::filesystem::path data = pack::findWowDataDir(places.wowDir);
    const pack::WowDataCheck wow = data.empty() ? pack::WowDataCheck{} : pack::checkWowData(data);
    s.wowFound = wow.archives > 0;
    s.wowComplete = wow.complete();
    s.wowMissing = static_cast<u32>(wow.missing.size());
    s.wowDamaged = static_cast<u32>(wow.damaged.size());
    s.wowBytes = wow.gameBytes;
    std::string problem;
    for (const std::vector<std::string>* list : {&wow.missing, &wow.damaged}) {
        for (const std::string& name : *list) {
            problem += problem.empty() ? "" : ", ";
            problem += name;
        }
    }
    copyText(s.wowProblem, problem);
    m_skipArchives = wow.unofficial;
    std::error_code error;
    for (const std::filesystem::path& dir : {places.packsDir, places.bakeDir.parent_path()}) {
        const std::filesystem::space_info space = std::filesystem::space(dir, error);
        if (!error) {
            s.freeBytes = space.available;
            break;
        }
    }
    PhoneBakePlanInputs inputs;
    inputs.places = places;
    inputs.cores = m_cores;
    inputs.existing = listClientPacks(places.packsDir);
    const std::vector<PhoneBakeJob> jobs = firstBakeJobs();
    const PhoneBakePlan first = makePhoneBakePlan(jobs, inputs);
    s.firstBakeBytes = phoneBakeSpaceNeeded(first);
    s.firstBakeSeconds = phoneBakeSeconds(first);
    VD_LOG_INFO(
        "Phone bake: WoW on the device %s (%u archives, %u missing, %u damaged, %u not official), %.1f GB "
        "free, a first bake needs %.1f GB",
        s.wowComplete ? "complete" : (s.wowFound ? "incomplete" : "absent"), wow.archives, s.wowMissing,
        s.wowDamaged, static_cast<u32>(wow.unofficial.size()), static_cast<f64>(s.freeBytes) / 1e9,
        static_cast<f64>(s.firstBakeBytes) / 1e9);
}

void PhoneBake::refresh() {
    const PhoneBakePlaces& places = m_config.places;
    std::string text;
    if (!m_plan && readText(places.planFile(), text)) {
        if (Result<PhoneBakePlan> plan = readPhoneBakePlan(text); plan) {
            m_plan = std::move(plan.value());
        } else {
            VD_LOG_WARN("Phone bake: %s", plan.error().message.c_str());
        }
    }
    PhoneBakeStatus& s = m_working;
    const i64 now = phoneBakeNowMs();
    if (!m_plan) {
        s.state = PhoneBakeState::None;
        s.starting = false;
        s.stale = false;
        s.stepCount = 0;
        s.finished = 0;
        s.fraction = 0.0f;
        return;
    }
    if (readText(places.statusFile(), text)) {
        if (Result<PhoneBakeRun> run = readPhoneBakeRun(text); run && run.value().plan == m_plan->id) {
            m_run = std::move(run.value());
        }
    }
    const PhoneBakePlan& plan = *m_plan;
    const bool ours = m_run.plan == plan.id;
    s.kind = plan.kind;
    s.stepCount = static_cast<u32>(plan.steps.size());
    s.maxJobs = plan.thermal.maxJobs;
    s.state = ours ? m_run.state : PhoneBakeState::None;
    s.finished = ours ? std::min(m_run.finished, s.stepCount) : 0;
    s.step = ours ? std::min(m_run.step, s.stepCount) : 0;
    s.jobs = ours ? m_run.jobs : 0;
    s.exitCode = ours ? m_run.exitCode : 0;
    copyText(s.error, ours ? std::string_view(m_run.error) : std::string_view{});
    copyText(s.stepName,
             s.step < s.stepCount ? std::string_view(plan.steps[s.step].name) : std::string_view{});
    // Waiting for the platform to start it: an answer newer than the request ends the wait.
    if (m_requestedMs != 0) {
        if (ours && m_run.updatedMs + 2000 >= m_requestedMs && m_run.state != PhoneBakeState::None) {
            m_requestedMs = 0;
        } else if (now - m_requestedMs > kStartTimeoutMs) {
            VD_LOG_WARN("Phone bake: its service did not answer");
            m_requestedMs = 0;
            m_launchFailed = true;
        }
    }
    s.starting = m_requestedMs != 0;
    s.stale = !s.starting && ours && phoneBakeStale(m_run, now);
    if (s.stale) {
        s.state = PhoneBakeState::Interrupted;
    } else if (!s.starting && m_launchFailed && !(ours && m_run.updatedMs > 0 && s.active())) {
        s.state = PhoneBakeState::Interrupted; // it could not be started: shown as stopped, to try again
    }
    if (s.starting) {
        s.state = ours && m_run.state != PhoneBakeState::None ? m_run.state : PhoneBakeState::None;
    }
    s.launchFailed = !s.starting && m_launchFailed && s.state == PhoneBakeState::Interrupted && !s.stale;
    const PhoneBakeProgress progress = phoneBakeProgress(plan, m_run, now);
    s.fraction = progress.fraction;
    s.stepFraction = progress.stepFraction;
    s.secondsLeft = progress.secondsLeft;
    s.elapsedSeconds = progress.elapsedSeconds;
    s.neededBytes = phoneBakeSpaceNeeded(plan, s.finished);
}

void PhoneBake::publish() {
    std::lock_guard<std::mutex> lock(m_mutex);
    const u32 sequence = m_status.sequence + 1;
    m_status = m_working;
    m_status.sequence = sequence;
}

} // namespace vd
