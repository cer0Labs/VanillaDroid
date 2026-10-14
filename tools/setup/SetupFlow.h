#pragma once

#include "Adb.h"
#include "BakeRunner.h"
#include "HashCache.h"
#include "PhoneServer.h"
#include "SetupCommon.h"
#include "SetupSettings.h"
#include "WowCheck.h"
#include "core/Result.h"
#include "core/Types.h"

#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// Phase 22 (gui, ADR-0005 §4-§5): the steps of VanillaDroid Setup above its core, as the window (22.6) takes
// them and the console tool could: the APK to offer, the two clients checked, a bake's plan and run, what adb
// sends, the local server's events as lines and per phone, and the window's first-run guide. No SDL and no
// Dear ImGui: tested in vd_tests.
namespace vd::setup {

// --- Files and clients
// ---------------------------------------------------------------------------------------------

// The APK the Setup offers: `chosen` when set (an option or the settings), else VanillaDroid.apk next to the
// program (in a macOS bundle, also in Contents/Resources), else the first *.apk there by name. Empty when
// there is none.
std::filesystem::path findApk(const std::filesystem::path& chosen, const std::filesystem::path& programDir);

// The settings' clients, checked again (it reads a few headers and three tables: a moment).
struct Clients {
    ClientCheck base;           // empty (folder unset) when no WoW is chosen
    ClientCheck locale;         // the second client, when one is chosen
    bool hasLocale = false;     // it can be baked: usable and in another language than the base
    bool localeSkipped = false; // one is chosen but cannot be used now (VD_SETUP_BAKE_LOCALE_CLIENT_SKIPPED)
};
Clients checkClients(const Settings& settings);

// What a bake of these clients reads (Settings::lastBake after a bake that ended well).
std::string fingerprintOf(const Settings& settings, const Clients& clients);
// The plan for `packs` (its manifest and files) from these clients; `only` empty: every step.
BakePlan planFor(const Settings& settings, const std::filesystem::path& packs, const Clients& clients,
                 bool force, std::string_view only);
// The baker's inputs for these clients (the archives the player did not include are skipped).
BakeRun bakeRunFor(const Settings& settings, const Clients& clients, const std::filesystem::path& baker,
                   const std::filesystem::path& packs);
// "textures", "world (azeroth, kalimdor)".
std::string describeJob(const BakeJob& job);
// Why a job of `plan` runs: the packs of it the plan found out of date (empty when the step is missing,
// forced or everything is baked again).
std::vector<std::string> outdatedPacksOf(const BakePlan& plan, const BakeJob& job);

// Room a whole bake takes where the packs go: 4.5 GB of packs, the baker's cache (~3.2 GB) and a margin.
inline constexpr u64 kFullBakeBytes = 9ull * 1000 * 1000 * 1000;

// The packs of a folder for `adb push`, with their digests (through `cache`: "hash" progress), and
// manifest.json last with each pack's digest written in (the phone then knows what it has). Empty without
// packs.
Result<std::vector<PushItem>> packPushItems(const std::filesystem::path& packs, HashCache& cache,
                                            const Operation& operation);
// The official archives of a usable client for `adb send-wow`.
Result<std::vector<PushItem>> wowPushItems(const ClientCheck& check, HashCache& cache,
                                           const Operation& operation);

// --- The local server
// ------------------------------------------------------------------------------------------

// The line the console prints for a server event (its VD_SETUP_SERVE_* texts); an empty key for the events
// that have none (Preparing, Ready and Progress are shown otherwise).
Message serveEventMessage(const ServeEvent& event);

// What each phone did, from the server's events: the window's table of phones.
struct PhoneTransfer {
    std::string path; // "packs/models.vdpk"
    u64 position = 0; // bytes of the file the phone has now (the resumed offset and what was sent since)
    u64 total = 0;    // the file's size
    f64 lastSeconds = 0.0;
};
struct PhoneActivity {
    u32 client = 0; // IPv4 address, host order
    bool paired = false;
    bool unauthorized = false;            // the latest request came without the key
    std::vector<PhoneTransfer> transfers; // going now
    u32 filesReceived = 0;
    u64 bytesSent = 0;     // this session, every file and piece
    f64 lastSeconds = 0.0; // the caller's clock when it last did something
};
class PhoneActivityLog {
public:
    void apply(const ServeEvent& event, f64 nowSeconds);
    // Drops the transfers nothing was heard of for `idleSeconds` (a piece of a file that ended mid-file sends
    // no Sent event).
    void prune(f64 nowSeconds, f64 idleSeconds);
    void clear() { m_phones.clear(); }
    const std::vector<PhoneActivity>& phones() const { return m_phones; }

private:
    PhoneActivity& phone(u32 client);
    std::vector<PhoneActivity> m_phones; // in the order they first came
};

// --- The window's guide and its texts
// -------------------------------------------------------------------------------

// The guide the window opens with until a WoW is chosen: language, the WoW, the game data, the phone.
enum class WizardStep : u8 { Language, Wow, Bake, Phone };
inline constexpr u32 kWizardSteps = 4;

struct SetupFacts {
    bool wowUsable = false;  // the WoW of the settings was checked and can be used
    bool packsReady = false; // there are packs and nothing is left to bake
    bool busy = false;       // a long operation runs: the guide waits
};

bool wizardNeeded(const Settings& settings);
// Whether the guide may go on from `step` (Next) and back (Back); the last step's Next ends it.
bool canGoNext(WizardStep step, const SetupFacts& facts);
bool canGoBack(WizardStep step, const SetupFacts& facts);
WizardStep nextStep(WizardStep step);
WizardStep previousStep(WizardStep step);
bool isLastStep(WizardStep step);

// "1 h 05 min", "12 min 05 s", "45 s" (VD_SETUP_GUI_TIME_*).
std::string formatDuration(f64 seconds);
// The seconds left of an operation `fraction` done after `elapsed` seconds; negative while it is too early to
// tell (under 3 % or 10 s).
f64 estimateRemaining(f64 fraction, f64 elapsedSeconds);
// The name of a bake step for the player (VD_SETUP_GUI_STEP_<STEP>), with the maps of a partial one.
std::string stepTitle(const BakeJob& job);

} // namespace vd::setup
