#include "SetupFlow.h"

#include "pack/PackManifest.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace vd::setup {
namespace {

std::filesystem::path apkIn(const std::filesystem::path& dir) {
    if (dir.empty()) {
        return {};
    }
    if (std::filesystem::path named = findChildNoCase(dir, "VanillaDroid.apk"); !named.empty()) {
        return named;
    }
    std::error_code error;
    std::vector<std::filesystem::path> apks;
    for (const auto& entry : std::filesystem::directory_iterator(dir, error)) {
        if (entry.is_regular_file(error) && equalsNoCase(utf8FromPath(entry.path().extension()), ".apk")) {
            apks.push_back(entry.path());
        }
    }
    std::sort(apks.begin(), apks.end());
    return apks.empty() ? std::filesystem::path() : apks.front();
}

std::string twoDigits(u64 value) {
    char text[8];
    std::snprintf(text, sizeof(text), "%02u", static_cast<unsigned>(value % 100));
    return text;
}

} // namespace

std::filesystem::path findApk(const std::filesystem::path& chosen, const std::filesystem::path& programDir) {
    if (!chosen.empty()) {
        return chosen;
    }
    if (std::filesystem::path apk = apkIn(programDir); !apk.empty()) {
        return apk;
    }
    // A macOS bundle: VanillaDroid Setup.app/Contents/MacOS/<program>, its files in Contents/Resources.
    if (equalsNoCase(utf8FromPath(programDir.filename()), "MacOS")) {
        return apkIn(programDir.parent_path() / "Resources");
    }
    return {};
}

Clients checkClients(const Settings& settings) {
    Clients clients;
    if (settings.wow.empty()) {
        return clients;
    }
    clients.base = checkClient(settings.wow, settings.includeArchives);
    if (!clients.base.usable || settings.localeClient.empty()) {
        return clients;
    }
    clients.locale = checkClient(settings.localeClient, settings.includeArchives);
    clients.hasLocale = clients.locale.usable && clients.locale.locale != clients.base.locale;
    clients.localeSkipped = !clients.hasLocale;
    return clients;
}

std::string fingerprintOf(const Settings& settings, const Clients& clients) {
    return bakeFingerprint(clients.base, clients.hasLocale ? &clients.locale : nullptr,
                           settings.includeArchives);
}

BakePlan planFor(const Settings& settings, const std::filesystem::path& packs, const Clients& clients,
                 bool force, std::string_view only) {
    PlanInputs inputs;
    inputs.baseLocale = clients.base.locale;
    inputs.overlayLocale = clients.hasLocale ? clients.locale.locale : std::string();
    inputs.inputsChanged =
        !settings.lastBake.empty() && settings.lastBake != fingerprintOf(settings, clients);
    inputs.force = force;
    inputs.only = std::string(only);
    Result<pack::PackManifest> manifest = pack::PackManifest::load(packs);
    const std::vector<PackFile> files = listPackFiles(packs);
    return planBake(inputs, manifest ? manifest.value() : pack::PackManifest{}, pack::releaseRecipes(),
                    files);
}

BakeRun bakeRunFor(const Settings& settings, const Clients& clients, const std::filesystem::path& baker,
                   const std::filesystem::path& packs) {
    BakeRun run;
    run.baker = baker;
    run.wow = clients.base.folder;
    run.skipArchives = clients.base.skippedArchives(settings.includeArchives);
    if (clients.hasLocale) {
        run.localeClient = clients.locale.folder;
        run.localeSkipArchives = clients.locale.skippedArchives(settings.includeArchives);
    }
    run.packsDir = packs;
    return run;
}

std::string describeJob(const BakeJob& job) {
    std::string text = job.step;
    if (!job.maps.empty()) {
        text += " (";
        for (usize i = 0; i < job.maps.size(); ++i) {
            text += (i ? ", " : "") + job.maps[i];
        }
        text += ")";
    }
    return text;
}

std::vector<std::string> outdatedPacksOf(const BakePlan& plan, const BakeJob& job) {
    std::vector<std::string> files;
    for (const std::string& file : plan.outdated) {
        const pack::BakeStep step = pack::bakeStepOf(file);
        if (step.step != job.step) {
            continue;
        }
        if (!job.maps.empty() && std::find(job.maps.begin(), job.maps.end(), step.map) == job.maps.end()) {
            continue;
        }
        files.push_back(file);
    }
    return files;
}

Result<std::vector<PushItem>> packPushItems(const std::filesystem::path& packs, HashCache& cache,
                                            const Operation& operation) {
    std::vector<HashJob> jobs;
    std::vector<std::string> names;
    for (const PackFile& file : listPackFiles(packs)) {
        jobs.push_back({packs / pathFromUtf8(file.name), 0, 0, {}});
        names.push_back(file.name);
    }
    std::vector<PushItem> items;
    if (jobs.empty()) {
        return items;
    }
    VD_TRY(hashFiles(jobs, cache, operation));
    for (usize i = 0; i < jobs.size(); ++i) {
        items.push_back({jobs[i].file, names[i], jobs[i].size, jobs[i].sha256});
    }
    Result<pack::PackManifest> manifest = pack::PackManifest::load(packs);
    if (manifest && !manifest.value().packs.empty()) {
        bool changed = false;
        for (const PushItem& item : items) {
            if (const pack::PackManifest::Entry* entry = manifest.value().find(item.name);
                entry && entry->size == item.size && entry->sha256 != item.sha256) {
                manifest.value().record(item.name).sha256 = item.sha256;
                changed = true;
            }
        }
        if (changed) {
            static_cast<void>(manifest.value().save(packs)); // without them the phone only reads more
        }
        const std::filesystem::path file = packs / std::string(pack::kManifestFile);
        std::error_code error;
        const u64 size = std::filesystem::file_size(file, error);
        Result<std::string> digest = sha256FileProgress(file, {}, operation.cancel);
        if (digest && !error) {
            items.push_back({file, std::string(pack::kManifestFile), size, digest.value()});
        }
    }
    return items;
}

Result<std::vector<PushItem>> wowPushItems(const ClientCheck& check, HashCache& cache,
                                           const Operation& operation) {
    std::vector<HashJob> jobs;
    for (const ArchiveInfo& archive : check.gameArchives()) {
        jobs.push_back({check.dataDir / pathFromUtf8(archive.name), 0, 0, {}});
    }
    VD_TRY(hashFiles(jobs, cache, operation));
    std::vector<PushItem> items;
    for (const HashJob& job : jobs) {
        items.push_back({job.file, utf8FromPath(job.file.filename()), job.size, job.sha256});
    }
    return items;
}

Message serveEventMessage(const ServeEvent& event) {
    using Kind = ServeEvent::Kind;
    const std::string client = ipv4Text(event.client);
    switch (event.kind) {
    case Kind::PrepareFailed: return {Message::Level::Error, "VD_SETUP_SERVE_PREPARE_FAILED", {event.text}};
    case Kind::Download:
        return {Message::Level::Info,
                event.offset > 0 ? "VD_SETUP_SERVE_RESUME" : "VD_SETUP_SERVE_DOWNLOAD",
                {client, event.path, formatBytes(event.offset), formatBytes(event.total)}};
    case Kind::Sent:
        return {event.complete ? Message::Level::Info : Message::Level::Warning,
                event.complete ? "VD_SETUP_SERVE_SENT" : "VD_SETUP_SERVE_CUT",
                {client, event.path, formatBytes(event.bytes)}};
    case Kind::Paired: return {Message::Level::Info, "VD_SETUP_SERVE_PAIRED", {client}};
    case Kind::WrongCode: return {Message::Level::Warning, "VD_SETUP_SERVE_WRONG_CODE", {client}};
    case Kind::CodeLocked: return {Message::Level::Error, "VD_SETUP_GUI_CODE_LOCKED", {}};
    case Kind::Unauthorized:
        return {Message::Level::Warning, "VD_SETUP_SERVE_UNAUTHORIZED", {client, event.path}};
    case Kind::Preparing:
    case Kind::Ready:
    case Kind::Progress: break;
    }
    return {};
}

PhoneActivity& PhoneActivityLog::phone(u32 client) {
    for (PhoneActivity& phone : m_phones) {
        if (phone.client == client) {
            return phone;
        }
    }
    PhoneActivity added;
    added.client = client;
    m_phones.push_back(std::move(added));
    return m_phones.back();
}

void PhoneActivityLog::apply(const ServeEvent& event, f64 nowSeconds) {
    using Kind = ServeEvent::Kind;
    if (event.kind == Kind::Preparing || event.kind == Kind::Ready || event.kind == Kind::PrepareFailed ||
        event.kind == Kind::CodeLocked) {
        return; // the server's own state, no phone's
    }
    PhoneActivity& activity = phone(event.client);
    activity.lastSeconds = nowSeconds;
    auto transfer = [&]() -> PhoneTransfer& {
        for (PhoneTransfer& going : activity.transfers) {
            if (going.path == event.path) {
                return going;
            }
        }
        activity.transfers.push_back({event.path, 0, 0, nowSeconds});
        return activity.transfers.back();
    };
    switch (event.kind) {
    case Kind::Download: {
        PhoneTransfer& going = transfer();
        going.position = event.offset;
        going.total = event.total;
        going.lastSeconds = nowSeconds;
        activity.unauthorized = false;
        break;
    }
    case Kind::Progress: {
        PhoneTransfer& going = transfer();
        going.position = std::max(going.position, event.offset + event.bytes);
        going.total = event.total;
        going.lastSeconds = nowSeconds;
        break;
    }
    case Kind::Sent: {
        activity.bytesSent += event.bytes;
        if (event.complete) {
            ++activity.filesReceived;
        }
        activity.transfers.erase(std::remove_if(activity.transfers.begin(), activity.transfers.end(),
                                                [&](const PhoneTransfer& t) { return t.path == event.path; }),
                                 activity.transfers.end());
        break;
    }
    case Kind::Paired:
        activity.paired = true;
        activity.unauthorized = false;
        break;
    case Kind::Unauthorized: activity.unauthorized = true; break;
    default: break;
    }
}

void PhoneActivityLog::prune(f64 nowSeconds, f64 idleSeconds) {
    for (PhoneActivity& activity : m_phones) {
        activity.transfers.erase(
            std::remove_if(activity.transfers.begin(), activity.transfers.end(),
                           [&](const PhoneTransfer& t) { return nowSeconds - t.lastSeconds > idleSeconds; }),
            activity.transfers.end());
    }
}

bool wizardNeeded(const Settings& settings) {
    return settings.wow.empty();
}

bool canGoNext(WizardStep step, const SetupFacts& facts) {
    if (facts.busy) {
        return false;
    }
    switch (step) {
    case WizardStep::Language: return true;
    case WizardStep::Wow: return facts.wowUsable;
    case WizardStep::Bake: return facts.packsReady;
    case WizardStep::Phone: return true;
    }
    return false;
}

bool canGoBack(WizardStep step, const SetupFacts& facts) {
    return step != WizardStep::Language && !facts.busy;
}

WizardStep nextStep(WizardStep step) {
    return isLastStep(step) ? step : static_cast<WizardStep>(static_cast<u8>(step) + 1);
}

WizardStep previousStep(WizardStep step) {
    return step == WizardStep::Language ? step : static_cast<WizardStep>(static_cast<u8>(step) - 1);
}

bool isLastStep(WizardStep step) {
    return static_cast<u32>(step) + 1 >= kWizardSteps;
}

std::string formatDuration(f64 seconds) {
    const u64 total = seconds > 0 ? static_cast<u64>(std::llround(seconds)) : 0;
    if (total >= 3600) {
        return formatText("VD_SETUP_GUI_TIME_HOURS",
                          {std::to_string(total / 3600), twoDigits(total / 60 % 60)});
    }
    if (total >= 60) {
        return formatText("VD_SETUP_GUI_TIME_MINUTES", {std::to_string(total / 60), twoDigits(total % 60)});
    }
    return formatText("VD_SETUP_GUI_TIME_SECONDS", {std::to_string(total)});
}

f64 estimateRemaining(f64 fraction, f64 elapsedSeconds) {
    if (fraction < 0.03 || elapsedSeconds < 10.0 || fraction >= 1.0) {
        return fraction >= 1.0 ? 0.0 : -1.0;
    }
    return elapsedSeconds * (1.0 - fraction) / fraction;
}

std::string stepTitle(const BakeJob& job) {
    std::string key = "VD_SETUP_GUI_STEP_";
    for (const char c : job.step) {
        key.push_back(c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c);
    }
    std::string text = formatText(key);
    if (!job.maps.empty()) {
        text += " (";
        for (usize i = 0; i < job.maps.size(); ++i) {
            text += (i ? ", " : "") + job.maps[i];
        }
        text += ")";
    }
    return text;
}

} // namespace vd::setup
