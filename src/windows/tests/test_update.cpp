// Portable rules of the Windows update install (core/UpdatePlan.h, #404).
#include "core/UpdatePlan.h"
#include <cstdio>
#include <string>

using namespace putgd;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void channelTests() {
    CHECK(update::releaseChannel("26.1022.942-nightly") == "nightly");
    CHECK(update::releaseChannel("26.1101.960") == "stable");
    CHECK(update::releaseChannel("27.105.1") == "stable");
    // Development, dirty, pre-#373 and malformed versions don't install.
    for (const char* v : {"26.1022.985-dev+g0b8dfe0e7278", "26.1022.985-dev+g0b8dfe0e7278.dirty", "0.1.1-nightly.925", "0.1.1", "26.01022.942",
                          "26.1022.0942", "25.1022.1", "26.1022.942-nightly.3", "26.1022.942-rc.1", "26.1022", "", "-nightly"})
        CHECK(update::releaseChannel(v).empty());
    CHECK(update::packageTarget(true) == "windows-msi" && update::packageTarget(false) == "windows-zip");
}
static void urlTests() {
    CHECK(update::validBase("https://pkg.put.gd/releases") == "https://pkg.put.gd/releases/");
    CHECK(update::validBase("http://127.0.0.1:8404/r/") == "http://127.0.0.1:8404/r/");
    CHECK(update::validBase("http://localhost:8404/r/") == "http://localhost:8404/r/");
    for (const char* bad : {"http://pkg.put.gd/releases/", "https://user@pkg.put.gd/", "https://pkg.put.gd/?a=1", "https://pkg.put.gd/#x", "ftp://pkg.put.gd/", "not a url", ""})
        CHECK(update::validBase(bad).empty());
    std::string url;
    CHECK(!update::releaseFile("https://github.com/o/r/releases/download", "v26.1022.942-nightly", "SHA256SUMS", url));
    CHECK(url == "https://github.com/o/r/releases/download/v26.1022.942-nightly/SHA256SUMS");
    CHECK(!update::releaseFile("https://x/", "v26.1022.942", "putgd-26.1022.942-windows-x86_64.msi", url));
    CHECK(update::releaseFile("https://x/", "26.1022.942", "SHA256SUMS", url).code == "update_install_failed");
    CHECK(update::releaseFile("https://x/", "v26.1022.942-dev", "SHA256SUMS", url).code == "update_install_failed");
    CHECK(update::releaseFile("https://x/", "v26.1022.942", "../SHA256SUMS", url).code == "update_install_failed");
    CHECK(update::releaseFile("https://x/", "v26.1022.942", "a/b", url).code == "update_install_failed");
    CHECK(update::releaseFile("http://x/", "v26.1022.942", "SHA256SUMS", url).code == "update_download_failed");
}
static void sumsTests() {
    const std::string a(64, 'a'), b(64, 'B');
    const std::string sums = a + "  putgd-1-windows-x86_64.msi\n" + b + " *putgd-1-windows-x86_64.zip\r\n" + a + "  other\n";
    CHECK(update::sumsLine(sums, "putgd-1-windows-x86_64.msi") == a);
    CHECK(update::sumsLine(sums, "putgd-1-windows-x86_64.zip") == std::string(64, 'b'));
    CHECK(update::sumsLine(sums, "putgd-1-windows-x86_64").empty());
    CHECK(update::sumsLine(sums + a + "  other\n", "other").empty()); // listed twice
    CHECK(update::sumsLine(std::string(64, 'g') + "  x\n", "x").empty());
    CHECK(update::trustedComment("v26.1022.942-nightly", "nightly") == "put.gd v26.1022.942-nightly nightly");
}
static void assetTests() {
    const json::Object release{{"assets", json::Object{
        {"windows-msi", json::Object{{"name", "putgd-26.1022.942-nightly-windows-x86_64.msi"}, {"size", 3000000}, {"sha256", std::string(64, 'a')}}},
        {"windows-zip", json::Object{{"name", "putgd-26.1022.942-nightly-windows-x86_64.zip"}, {"size", 2800000}}},
        {"appimage", json::Object{{"name", "putgd-26.1022.942-nightly-x86_64.AppImage"}, {"size", 1}}}}}};
    json::Object asset;
    CHECK(!update::releaseAsset(release, "windows-msi", asset) && asset.value("size").toInteger() == 3000000);
    CHECK(!update::releaseAsset(release, "windows-zip", asset));
    CHECK(update::releaseAsset(json::Object{{"assets", json::Object{}}}, "windows-msi", asset).details.value("reason") == "no_package");
    const json::Object wrongSuffix{{"assets", json::Object{{"windows-msi", json::Object{{"name", "putgd.zip"}, {"size", 1}}}}}};
    CHECK(update::releaseAsset(wrongSuffix, "windows-msi", asset).code == "update_install_unavailable");
    const json::Object huge{{"assets", json::Object{{"windows-zip", json::Object{{"name", "putgd.zip"}, {"size", 300.0 * 1024 * 1024}}}}}};
    CHECK(update::releaseAsset(huge, "windows-zip", asset).code == "update_verification_failed");
    const json::Object badSum{{"assets", json::Object{{"windows-zip", json::Object{{"name", "putgd.zip"}, {"size", 1}, {"sha256", "xyz"}}}}}};
    CHECK(update::releaseAsset(badSum, "windows-zip", asset).code == "update_feed_invalid");
}
static void directoryTests() {
    CHECK(update::sameDirectory("C:\\Users\\lab\\AppData\\Local\\Programs\\put.gd\\", "c:\\users\\LAB\\AppData\\Local\\Programs\\put.gd"));
    CHECK(!update::sameDirectory("C:\\putgd", "C:\\putgd2"));
    CHECK(!update::sameDirectory("", "C:\\putgd"));
}
int main() {
    channelTests(); urlTests(); sumsTests(); assetTests(); directoryTests();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("windows update plan: ok\n");
    return 0;
}
