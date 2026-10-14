// Phase 22 (setup): the QR code, the network addresses, adb's outputs, child processes, the digests' cache
// and the APK's manifest.
#include "Adb.h"
#include "ApkInfo.h"
#include "ChildProcess.h"
#include "HashCache.h"
#include "NetInterfaces.h"
#include "QrText.h"
#include "SetupCommon.h"
#include "core/Sha256.h"

#include <doctest/doctest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ostream> // doctest prints std::string values through operator<<
#include <string>
#include <thread>
#include <vector>

using namespace vd::setup;

namespace {

std::filesystem::path tempDir(const char* name) {
    const std::filesystem::path dir = std::filesystem::temp_directory_path() / "vd_setup_tests" / name;
    std::error_code error;
    std::filesystem::remove_all(dir, error);
    std::filesystem::create_directories(dir, error);
    return dir;
}

// Code points of a UTF-8 line.
size_t columns(std::string_view line) {
    size_t count = 0;
    for (const char c : line) {
        count += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    }
    return count;
}

} // namespace

TEST_CASE("Setup: the QR code in half blocks") {
    const vd::Result<QrMatrix> small = encodeQr("HELLO WORLD");
    REQUIRE(small.ok());
    CHECK(small.value().size == 21); // version 1
    // The pairing URL: version 4 or 5 (33 or 37 modules), small enough for a terminal.
    const std::string url = "http://192.168.100.200:47800/?k=0123456789abcdef0123456789abcdef";
    const vd::Result<QrMatrix> qr = encodeQr(url);
    REQUIRE(qr.ok());
    CHECK(qr.value().size >= 33);
    CHECK(qr.value().size <= 37);
    // The three finder patterns: dark corners.
    const int n = qr.value().size;
    CHECK(qr.value().dark(0, 0));
    CHECK(qr.value().dark(n - 1, 0));
    CHECK(qr.value().dark(0, n - 1));
    CHECK_FALSE(qr.value().dark(n - 1, n - 1 + 1)); // outside: light

    for (const bool ansi : {false, true}) {
        const std::string text = renderQrHalfBlocks(qr.value(), 2, ansi);
        std::vector<std::string> lines;
        size_t begin = 0;
        while (begin < text.size()) {
            const size_t end = text.find('\n', begin);
            lines.push_back(text.substr(begin, end - begin));
            begin = end + 1;
        }
        CHECK(lines.size() == static_cast<size_t>((n + 4 + 1) / 2));
        if (!ansi) {
            for (const std::string& line : lines) {
                CHECK(columns(line) == static_cast<size_t>(n + 4));
            }
            // The quiet zone's first row: light modules drawn as full blocks.
            CHECK(lines.front().rfind("\xE2\x96\x88\xE2\x96\x88", 0) == 0);
        } else {
            CHECK(text.find("\x1B[30;40m") != std::string::npos); // two dark modules
        }
    }
    CHECK_FALSE(encodeQr(std::string(3000, 'x')).ok());
}

TEST_CASE("Setup: the PC's addresses") {
    vd::u32 address = 0;
    CHECK(parseIpv4("192.168.1.20", address));
    CHECK(address == 0xC0A80114u);
    CHECK_FALSE(parseIpv4("192.168.1", address));
    CHECK_FALSE(parseIpv4("192.168.1.256", address));
    CHECK_FALSE(parseIpv4("a.b.c.d", address));
    CHECK_FALSE(parseIpv4("1..2.3", address));
    CHECK(looksVirtual("vEthernet (WSL)"));
    CHECK(looksVirtual("VirtualBox Host-Only Network"));
    CHECK(looksVirtual("Tailscale"));
    CHECK_FALSE(looksVirtual("Wi-Fi"));
    CHECK_FALSE(looksVirtual("Ethernet 2"));

    std::vector<LocalAddress> list(4);
    list[0] = {0xAC1C5001u, "vEthernet (WSL)", false, false, true}; // 172.28.80.1
    list[1] = {0x64650203u, "Tailscale", false, false, true};       // 100.101.2.3
    list[2] = {0xC0A80114u, "Wi-Fi", true, true, false};            // 192.168.1.20
    list[3] = {0xA9FE0102u, "Ethernet", false, false, false};       // 169.254.1.2
    const LocalAddress* best = pickLanAddress(list);
    REQUIRE(best != nullptr);
    CHECK(best->text() == "192.168.1.20");
    CHECK(pickLanAddress({}) == nullptr);
    // The machine's own list: whatever it holds, no loopback.
    for (const LocalAddress& own : localAddresses()) {
        CHECK((own.address >> 24) != 127u);
    }
}

TEST_CASE("Setup: adb's outputs") {
    const std::vector<AdbDevice> devices = parseAdbDevices(
        "* daemon not running; starting now at tcp:5037\n* daemon started successfully\nList of devices "
        "attached\n"
        "adb-R5CT00TEST-AbCdEf._adb-tls-connect._tcp device product:q5qxeea model:SM_F946B device:q5q "
        "transport_id:3\r\n192.168.1.50:41235     unauthorized transport_id:4\n\n");
    REQUIRE(devices.size() == 2);
    CHECK(devices[0].serial == "adb-R5CT00TEST-AbCdEf._adb-tls-connect._tcp");
    CHECK(devices[0].state == "device");
    CHECK(devices[0].model == "SM_F946B");
    CHECK(devices[1].state == "unauthorized");
    CHECK(parseAdbDevices("List of devices attached\n\n").empty());

    const std::string a(64, 'a');
    const std::string b(64, 'B');
    const auto sums = parseSha256sum(a + "  models.vdpk\n" + b +
                                     "  /sdcard/x/db.vdpk\n"
                                     "sha256sum: missing.vdpk: No such file or directory\n");
    REQUIRE(sums.size() == 2);
    CHECK(sums.at("models.vdpk") == a);
    CHECK(sums.at("db.vdpk") == std::string(64, 'b'));
    const auto sizes = parseStatSizes("12345 models.vdpk\n0 manifest.json\nstat: x: No such file\n");
    CHECK(sizes.at("models.vdpk") == 12345u);
    CHECK(sizes.at("manifest.json") == 0u);
    CHECK(sizes.size() == 2);

    CHECK(shellQuote("a b") == "'a b'");
    CHECK(shellQuote("it's") == "'it'\\''s'");
    CHECK(remoteFilesDir("com.cer0labs.vanilladroid") ==
          "/sdcard/Android/data/com.cer0labs.vanilladroid/files");
    CHECK(explainAdbFailure("adb: failed to install x.apk: Failure [INSTALL_FAILED_UPDATE_INCOMPATIBLE: ...]")
              .key == "VD_SETUP_ADB_SIGNATURE");
    CHECK(explainAdbFailure("error: device unauthorized.\nThis adb server's $ADB_VENDOR_KEYS").key ==
          "VD_SETUP_ADB_UNAUTHORIZED");
    const Message unknown = explainAdbFailure("something\nlast words\n");
    CHECK(unknown.key == "VD_SETUP_ADB_FAILED");
    CHECK(unknown.args == std::vector<std::string>{"last words"});
    CHECK(platformToolsUrl().rfind("https://dl.google.com/android/repository/platform-tools-latest-", 0) ==
          0);
}

TEST_CASE("Setup: Windows command lines") {
    CHECK(quoteWindowsArgument("plain") == "plain");
    CHECK(quoteWindowsArgument("") == "\"\"");
    CHECK(quoteWindowsArgument("C:/World of Warcraft 1.12") == "\"C:/World of Warcraft 1.12\"");
    CHECK(quoteWindowsArgument("a\"b") == "\"a\\\"b\"");
    CHECK(quoteWindowsArgument("C:\\dir with space\\") == "\"C:\\dir with space\\\\\"");
    CHECK(quoteWindowsArgument("a\\\\\"b") == "\"a\\\\\\\\\\\"b\"");
    CHECK(quoteWindowsArgument(";") == ";");
}

TEST_CASE("Setup: child processes, their lines and cancelling them") {
#if defined(_WIN32)
    const std::filesystem::path shell = findInPath("cmd");
    REQUIRE_FALSE(shell.empty());
    std::string output;
    vd::Result<ProcessResult> result =
        runProcessCapture(shell, {"/c", "echo first line& echo second line 1>&2& exit /b 3"}, output);
    REQUIRE(result.ok());
    CHECK(result.value().exitCode == 3);
    CHECK(output.find("first line") != std::string::npos);
    CHECK(output.find("second line") != std::string::npos); // stderr too
    const std::filesystem::path waiter = findInPath("ping");
    const std::vector<std::string> waitArgs = {"-n", "30", "127.0.0.1"};
#else
    std::string output;
    vd::Result<ProcessResult> result =
        runProcessCapture("/bin/sh", {"-c", "echo first line; echo second line >&2; exit 3"}, output);
    REQUIRE(result.ok());
    CHECK(result.value().exitCode == 3);
    CHECK(output.find("second line") != std::string::npos);
    const std::filesystem::path waiter = "/bin/sleep";
    const std::vector<std::string> waitArgs = {"30"};
#endif
    CancelToken cancel;
    std::thread stopper([&] {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        cancel.cancel();
    });
    const auto start = std::chrono::steady_clock::now();
    result = runProcessCapture(waiter, waitArgs, output, &cancel);
    stopper.join();
    REQUIRE(result.ok());
    CHECK(result.value().cancelled);
    CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(10));
    CHECK_FALSE(runProcessCapture("this/does/not/exist.exe", {}, output).ok());
}

TEST_CASE("Setup: the digests' cache") {
    const std::filesystem::path dir = tempDir("hashes");
    std::vector<HashJob> jobs;
    for (int i = 0; i < 3; ++i) {
        const std::filesystem::path file = dir / ("file" + std::to_string(i) + ".bin");
        std::ofstream(file, std::ios::binary)
            << std::string(static_cast<size_t>(100000 * (i + 1)), char('a' + i));
        jobs.push_back({file, 0, 0, {}});
    }
    HashCache cache(dir / "hashes.json");
    cache.load();
    std::vector<double> fractions;
    Operation operation;
    operation.progress = [&](const ProgressEvent& event) { fractions.push_back(event.fraction); };
    REQUIRE(hashFiles(jobs, cache, operation).ok());
    for (const HashJob& job : jobs) {
        CHECK(job.sha256 == vd::sha256File(job.file).value());
        CHECK(job.size > 0);
    }
    CHECK_FALSE(fractions.empty());
    CHECK(fractions.back() == doctest::Approx(1.0));
    // A second run reads nothing: the cache (saved and loaded again) answers. A wrong digest planted for the
    // same size and time proves it.
    HashCache again(dir / "hashes.json");
    again.load();
    CHECK(again.find(jobs[0].file, jobs[0].size, jobs[0].time) == jobs[0].sha256);
    again.store(jobs[0].file, jobs[0].size, jobs[0].time, std::string(64, '0'));
    std::vector<HashJob> second = {{jobs[0].file, 0, 0, {}}};
    REQUIRE(hashFiles(second, again, Operation{}).ok());
    CHECK(second[0].sha256 == std::string(64, '0'));
    // Another size: taken again.
    std::ofstream(jobs[0].file, std::ios::binary | std::ios::app) << "more";
    REQUIRE(hashFiles(second, again, Operation{}).ok());
    CHECK(second[0].sha256 == vd::sha256File(jobs[0].file).value());
    CHECK(hashFiles(std::span<HashJob>(), again, Operation{}).ok());
}

TEST_CASE("Setup: the APK's manifest") {
    CHECK_FALSE(readApkInfo(std::vector<vd::u8>(100, 0)).ok());
    CHECK_FALSE(parseBinaryManifest(std::vector<vd::u8>(16, 0)).ok());
    const char* apk = std::getenv("VD_TEST_APK"); // a release APK of the development build
    if (!apk || !*apk) {
        MESSAGE("VD_TEST_APK not set: skipped");
        return;
    }
    const vd::Result<ApkInfo> info = readApkInfo(std::filesystem::path(apk));
    REQUIRE(info.ok());
    CHECK(info.value().package == "dev.cer.vanilladroid");
    CHECK(info.value().versionCode >= 1u);
    CHECK_FALSE(info.value().versionName.empty());
}
