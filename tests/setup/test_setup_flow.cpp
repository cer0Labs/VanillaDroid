// Phase 22 (gui): the steps above VanillaDroid Setup's core that its window takes (SetupFlow.h): the
// first-run guide, times, step names, the APK next to the program, the reasons of a plan, the local server's
// events as lines and per phone, the progress of a download, and the window's texts in both languages.
#include "HttpProtocol.h"
#include "HttpServer.h"
#include "PhoneServer.h"
#include "SetupFlow.h"
#include "SetupText.h"
#include "core/Text.h"
#include "pack/PackManifest.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#include <winsock2.h>
#include <ws2tcpip.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

using namespace vd::setup;

namespace {

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_setup_tests" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

void writeFile(const std::filesystem::path& path, const std::string& content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(content.data(), static_cast<std::streamsize>(content.size()));
}

ServeEvent event(ServeEvent::Kind kind, vd::u32 client, std::string path = {}) {
    ServeEvent e;
    e.kind = kind;
    e.client = client;
    e.path = std::move(path);
    return e;
}

} // namespace

TEST_CASE("Setup flow: the first-run guide") {
    Settings settings;
    CHECK(wizardNeeded(settings));
    settings.wow = "C:/World of Warcraft 1.12";
    CHECK_FALSE(wizardNeeded(settings));

    SetupFacts facts;
    CHECK(canGoNext(WizardStep::Language, facts));
    CHECK_FALSE(canGoBack(WizardStep::Language, facts));
    CHECK_FALSE(canGoNext(WizardStep::Wow, facts)); // no usable WoW yet
    facts.wowUsable = true;
    CHECK(canGoNext(WizardStep::Wow, facts));
    CHECK_FALSE(canGoNext(WizardStep::Bake, facts)); // the packs are not ready
    facts.packsReady = true;
    CHECK(canGoNext(WizardStep::Bake, facts));
    CHECK(canGoNext(WizardStep::Phone, facts));
    facts.busy = true; // nothing moves while something runs
    for (vd::u32 i = 0; i < kWizardSteps; ++i) {
        CHECK_FALSE(canGoNext(static_cast<WizardStep>(i), facts));
        CHECK_FALSE(canGoBack(static_cast<WizardStep>(i), facts));
    }

    CHECK(nextStep(WizardStep::Language) == WizardStep::Wow);
    CHECK(nextStep(WizardStep::Wow) == WizardStep::Bake);
    CHECK(nextStep(WizardStep::Bake) == WizardStep::Phone);
    CHECK(nextStep(WizardStep::Phone) == WizardStep::Phone);
    CHECK(previousStep(WizardStep::Phone) == WizardStep::Bake);
    CHECK(previousStep(WizardStep::Language) == WizardStep::Language);
    CHECK(isLastStep(WizardStep::Phone));
    CHECK_FALSE(isLastStep(WizardStep::Bake));
}

TEST_CASE("Setup flow: times, estimates and step names") {
    REQUIRE(loadSetupTexts("enUS") == "enUS");
    CHECK(formatDuration(0) == "0 s");
    CHECK(formatDuration(-3) == "0 s");
    CHECK(formatDuration(45.4) == "45 s");
    CHECK(formatDuration(125) == "2 min 05 s");
    CHECK(formatDuration(3725) == "1 h 02 min");
    CHECK(formatDuration(36000) == "10 h 00 min");

    CHECK(estimateRemaining(0.01, 60) < 0); // too early
    CHECK(estimateRemaining(0.5, 5) < 0);   // too soon
    CHECK(estimateRemaining(0.5, 100) == doctest::Approx(100));
    CHECK(estimateRemaining(0.25, 60) == doctest::Approx(180));
    CHECK(estimateRemaining(1.0, 60) == 0);

    // Every step of the baker has a name for the player, in both languages.
    for (const char* locale : {"enUS", "esES"}) {
        loadSetupTexts(locale);
        for (const std::string_view step : bakeSteps()) {
            std::string key = "VD_SETUP_GUI_STEP_";
            for (const char c : step) {
                key.push_back(static_cast<char>(c >= 'a' && c <= 'z' ? c - 'a' + 'A' : c));
            }
            CHECK_MESSAGE(vd::text::has(vd::text::Key(key)), key);
        }
    }
    loadSetupTexts("enUS");
    CHECK(stepTitle(BakeJob{"textures", {}}) == "Textures");
    CHECK(stepTitle(BakeJob{"world", {"azeroth", "kalimdor"}}) == "Terrain and water (azeroth, kalimdor)");
    CHECK(describeJob(BakeJob{"world", {"azeroth", "kalimdor"}}) == "world (azeroth, kalimdor)");
    CHECK(describeJob(BakeJob{"db", {}}) == "db");
    loadSetupTexts("esES");
    CHECK(formatDuration(125) == "2 min 05 s");
    CHECK(stepTitle(BakeJob{"textures", {}}) == "Texturas");
    vd::text::unload();
}

TEST_CASE("Setup flow: the window's texts are built in with the console's") {
    // One entry per language, though each has two files (setup.lang and setupwindow.lang).
    const std::vector<std::string> locales = setupLocales();
    CHECK(locales.size() == 2);
    CHECK(std::find(locales.begin(), locales.end(), "enUS") != locales.end());
    CHECK(std::find(locales.begin(), locales.end(), "esES") != locales.end());
    CHECK(setupLanguageName("enUS") == "English");
    CHECK(setupLanguageName("esES") == "Español");
    CHECK(setupLanguageName("xxXX") == "xxXX");

    REQUIRE(loadSetupTexts("esES") == "esES");
    CHECK(std::string(vd::text::get("VD_SETUP_GUI_NAV_BAKE")) == "Datos del juego"); // setupwindow.lang
    CHECK(std::string(vd::text::get("VD_SETUP_WOW_SAVED")) == "Guardado.");          // setup.lang
    REQUIRE(loadSetupTexts("enUS") == "enUS");
    CHECK(std::string(vd::text::get("VD_SETUP_GUI_NAV_BAKE")) == "Game data");
    CHECK(std::string(vd::text::get("VD_SETUP_WOW_SAVED")) == "Saved.");
    vd::text::unload();
}

TEST_CASE("Setup flow: the APK next to the program") {
    const std::filesystem::path dir = tempDir("flow_apk");
    CHECK(findApk({}, dir).empty());
    writeFile(dir / "b-build.apk", "b");
    writeFile(dir / "a-build.apk", "a");
    CHECK(findApk({}, dir) == dir / "a-build.apk"); // the first by name
    writeFile(dir / "VanillaDroid.apk", "v");
    CHECK(findApk({}, dir) == dir / "VanillaDroid.apk");
    CHECK(findApk(dir / "chosen.apk", dir) == dir / "chosen.apk"); // the settings' or an option's, as given

    // A macOS bundle: the program in Contents/MacOS, the APK in Contents/Resources.
    const std::filesystem::path bundle = dir / "VanillaDroid Setup.app" / "Contents";
    std::filesystem::create_directories(bundle / "MacOS");
    std::filesystem::create_directories(bundle / "Resources");
    CHECK(findApk({}, bundle / "MacOS").empty());
    writeFile(bundle / "Resources" / "VanillaDroid.apk", "v");
    CHECK(findApk({}, bundle / "MacOS") == bundle / "Resources" / "VanillaDroid.apk");
}

TEST_CASE("Setup flow: why a step of a plan is baked") {
    BakePlan plan;
    plan.outdated = {"models.vdpk", "world_azeroth.vdpk", "world_kalimdor.vdpk", "collision_objects.vdpk"};
    const std::vector<std::string> world = outdatedPacksOf(plan, BakeJob{"world", {"azeroth"}});
    REQUIRE(world.size() == 1);
    CHECK(world[0] == "world_azeroth.vdpk");
    CHECK(outdatedPacksOf(plan, BakeJob{"world", {}}).size() == 2); // the whole step
    CHECK(outdatedPacksOf(plan, BakeJob{"models", {}}) == std::vector<std::string>{"models.vdpk"});
    CHECK(outdatedPacksOf(plan, BakeJob{"objects", {}}) ==
          std::vector<std::string>{"collision_objects.vdpk"});
    CHECK(outdatedPacksOf(plan, BakeJob{"textures", {}}).empty());
}

TEST_CASE("Setup flow: the server's events as lines and per phone") {
    const vd::u32 phone = 0xC0A80132; // 192.168.1.50
    ServeEvent download = event(ServeEvent::Kind::Download, phone, "packs/models.vdpk");
    download.offset = 0;
    download.total = 1000;
    CHECK(serveEventMessage(download).key == "VD_SETUP_SERVE_DOWNLOAD");
    CHECK(serveEventMessage(download).args[0] == "192.168.1.50");
    download.offset = 400;
    CHECK(serveEventMessage(download).key == "VD_SETUP_SERVE_RESUME");
    ServeEvent cut = event(ServeEvent::Kind::Sent, phone, "packs/models.vdpk");
    CHECK(serveEventMessage(cut).key == "VD_SETUP_SERVE_CUT");
    CHECK(serveEventMessage(cut).level == Message::Level::Warning);
    cut.complete = true;
    CHECK(serveEventMessage(cut).key == "VD_SETUP_SERVE_SENT");
    CHECK(serveEventMessage(event(ServeEvent::Kind::Paired, phone)).key == "VD_SETUP_SERVE_PAIRED");
    CHECK(serveEventMessage(event(ServeEvent::Kind::WrongCode, phone)).key == "VD_SETUP_SERVE_WRONG_CODE");
    CHECK(serveEventMessage(event(ServeEvent::Kind::CodeLocked, 0)).key == "VD_SETUP_GUI_CODE_LOCKED");
    CHECK(serveEventMessage(event(ServeEvent::Kind::Unauthorized, phone, "/files.json")).key ==
          "VD_SETUP_SERVE_UNAUTHORIZED");
    CHECK(serveEventMessage(event(ServeEvent::Kind::Progress, phone, "packs/models.vdpk")).key.empty());
    CHECK(serveEventMessage(event(ServeEvent::Kind::Preparing, 0)).key.empty());

    PhoneActivityLog log;
    log.apply(event(ServeEvent::Kind::Ready, 0), 0.0); // the server's own: no phone
    CHECK(log.phones().empty());
    log.apply(event(ServeEvent::Kind::Unauthorized, phone, "/files.json"), 1.0);
    REQUIRE(log.phones().size() == 1);
    CHECK(log.phones()[0].unauthorized);
    log.apply(event(ServeEvent::Kind::Paired, phone), 2.0);
    CHECK(log.phones()[0].paired);
    CHECK_FALSE(log.phones()[0].unauthorized);

    ServeEvent resumed = event(ServeEvent::Kind::Download, phone, "packs/models.vdpk");
    resumed.offset = 400;
    resumed.total = 1000;
    log.apply(resumed, 3.0);
    REQUIRE(log.phones()[0].transfers.size() == 1);
    CHECK(log.phones()[0].transfers[0].position == 400);
    ServeEvent progress = event(ServeEvent::Kind::Progress, phone, "packs/models.vdpk");
    progress.offset = 400;
    progress.bytes = 300;
    progress.total = 1000;
    log.apply(progress, 4.0);
    CHECK(log.phones()[0].transfers[0].position == 700);
    // A second file at the same time, and another phone.
    log.apply(event(ServeEvent::Kind::Download, phone, "packs/db.vdpk"), 4.0);
    log.apply(event(ServeEvent::Kind::Download, 0xC0A80133, "vanilladroid.apk"), 4.0);
    REQUIRE(log.phones().size() == 2);
    CHECK(log.phones()[0].transfers.size() == 2);

    ServeEvent sent = event(ServeEvent::Kind::Sent, phone, "packs/models.vdpk");
    sent.bytes = 600;
    sent.complete = true;
    log.apply(sent, 5.0);
    CHECK(log.phones()[0].filesReceived == 1);
    CHECK(log.phones()[0].bytesSent == 600);
    REQUIRE(log.phones()[0].transfers.size() == 1);
    CHECK(log.phones()[0].transfers[0].path == "packs/db.vdpk");
    // A piece that ended mid-file sends no Sent: dropped once nothing is heard of it.
    log.prune(30.0, 60.0);
    CHECK(log.phones()[0].transfers.size() == 1);
    log.prune(70.0, 60.0);
    CHECK(log.phones()[0].transfers.empty());
    CHECK(log.phones()[1].transfers.empty());
    log.clear();
    CHECK(log.phones().empty());
}

TEST_CASE("Setup flow: a download's progress, a few times a second") {
    loadSetupTexts("enUS");
    const std::filesystem::path dir = tempDir("flow_progress");
    const std::filesystem::path packs = dir / "packs";
    std::filesystem::create_directories(packs);
    writeFile(packs / "models.vdpk", std::string(100000, 'm'));
    ServeOptions options;
    options.packsDir = packs;
    options.key = "0123456789abcdef0123456789abcdef";
    options.code = "042917";
    options.hashCache = dir / "hashes.json";
    std::vector<ServeEvent> events;
    PhoneServer server(options, [&](const ServeEvent& e) { events.push_back(e); });
    REQUIRE(server.prepareNow());

    const vd::Result<HttpRequest> request =
        parseRequestHead("GET /packs/models.vdpk?k=" + options.key + " HTTP/1.1\r\nRange: bytes=1000-\r\n");
    REQUIRE(request.ok());
    HttpResponse response = server.handle(request.value(), 0xC0A80132);
    REQUIRE(response.status == 206);
    REQUIRE(response.onProgress);
    events.clear();
    response.onProgress(25000); // the first block: told at once
    response.onProgress(50000); // a moment later: not again yet
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == ServeEvent::Kind::Progress);
    CHECK(events[0].path == "packs/models.vdpk");
    CHECK(events[0].offset == 1000);
    CHECK(events[0].bytes == 25000);
    CHECK(events[0].total == 100000);
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    response.onProgress(75000);
    REQUIRE(events.size() == 2);
    CHECK(events[1].bytes == 75000);
    vd::text::unload();
}

namespace {

#if defined(_WIN32)
using ClientSocket = SOCKET;
#else
using ClientSocket = int;
#endif

// A client of the local server on this PC (127.0.0.1), as a phone's connection.
ClientSocket connectTo(vd::u16 port) {
    const ClientSocket client = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(connect(client, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    return client;
}

void closeClient(ClientSocket client) {
#if defined(_WIN32)
    closesocket(client);
#else
    close(client);
#endif
}

// stop() on another thread, given 5 s: true when it returned. A server whose stop() hangs is left behind (its
// thread still uses it) rather than hang the tests.
bool stopsAtOnce(HttpServer* server) {
    auto stopped = std::make_shared<std::atomic<bool>>(false);
    std::thread([server, stopped] {
        server->stop();
        *stopped = true;
    }).detach();
    for (int i = 0; i < 50 && !*stopped; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (*stopped) {
        delete server;
    }
    return *stopped;
}

} // namespace

TEST_CASE("Setup flow: the local server stops at once") {
    // The window stops the server when it closes, vdsetup serve on Ctrl+C: a waiting accept() and a phone's
    // connection waiting in recv() must end. On Windows a synchronous socket made closesocket() wait for the
    // accept() (until another phone connected) and shutdown() does not end another thread's recv():
    // overlapped sockets and CancelIoEx.
    const auto handler = [](const HttpRequest&, vd::u32) {
        HttpResponse response;
        response.body = "ok";
        return response;
    };
    SUBCASE("nobody connected") {
        auto* server = new HttpServer();
        const vd::Result<vd::u16> port = server->start(47950, 30, handler, true);
        REQUIRE(port.ok());
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // accept() waits
        CHECK(stopsAtOnce(server));
    }
    SUBCASE("a phone connected that sends nothing") {
        auto* server = new HttpServer();
        const vd::Result<vd::u16> port = server->start(47950, 30, handler, true);
        REQUIRE(port.ok());
        const ClientSocket client = connectTo(port.value());
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // its connection waits in recv()
        CHECK(stopsAtOnce(server));
        closeClient(client);
    }
    SUBCASE("a phone that asked and keeps the connection open") {
        auto* server = new HttpServer();
        const vd::Result<vd::u16> port = server->start(47950, 30, handler, true);
        REQUIRE(port.ok());
        const ClientSocket client = connectTo(port.value());
        const std::string request = "GET /info.json HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
        REQUIRE(send(client, request.data(), static_cast<int>(request.size()), 0) ==
                static_cast<int>(request.size()));
        char answer[512] = {};
        const int got = static_cast<int>(recv(client, answer, sizeof(answer) - 1, 0));
        REQUIRE(got > 0);
        CHECK(std::string(answer, static_cast<size_t>(got)).rfind("HTTP/1.1 200", 0) == 0);
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // kept alive: waits for the next request
        CHECK(stopsAtOnce(server));
        closeClient(client);
    }
}
