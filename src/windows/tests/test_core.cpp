// Portable contract checks for the Windows core: JSON, settings, naming, S3 rules.
#include "common/Json.h"
#include "common/Sha256.h"
#include "common/Shortcut.h"
#include "common/Text.h"
#include "core/Naming.h"
#include "core/Operation.h"
#include "core/ProtonSettings.h"
#include "core/S3Settings.h"
#include "core/SanitizedPng.h"
#include "core/Settings.h"
#include "core/SettingsPatch.h"
#include <cstdio>
#include <cstdlib>

using namespace putgd;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)

static void jsonTests() {
    json::ParseError e;
    auto v = json::parse(R"({"b":1,"a":[true,null,"x\u00e9\ud83d\ude00"],"n":-2.5e3,"big":2147483648})", &e);
    CHECK(!e.failed);
    CHECK(json::serialize(v) == R"({"a":[true,null,"xé😀"],"b":1,"big":2147483648,"n":-2500})");
    CHECK(v["big"].toInteger() == 2147483648LL);
    CHECK(v["missing"].isNull());
    json::parse("{\"a\":1,}", &e); CHECK(e.failed);
    json::parse("[1] x", &e); CHECK(e.failed);
    json::parse("\"just a string\"", &e); CHECK(e.failed);
    json::parse("{\"a\":\"\\ud800\"}", &e); CHECK(e.failed);
    json::parse("{\"a\":NaN}", &e); CHECK(e.failed);
    CHECK(json::serialize(json::Value(0.1)) == "0.1");
    CHECK(json::serialize(json::Value(1e21)) == "1e+21");
    json::Object o{{"z", 1}, {"a", 2}};
    CHECK(json::serialize(o) == R"({"a":2,"z":1})");
    CHECK(json::pretty(json::Value(o)) == "{\n  \"a\": 2,\n  \"z\": 1\n}\n");
}
static void shaTests() {
    CHECK(crypto::Sha256::hexDigest("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(crypto::Sha256::hexDigest("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    std::string longText(1000, 'a'); CHECK(crypto::Sha256::hexDigest(longText) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    CHECK(crypto::hex(crypto::hmacSha256("key", "The quick brown fox jumps over the lazy dog")) == "f7bc83f430538424b13298e6aa6fb143ef4d59a14946175997479dbc2d1a3cd8");
    CHECK(text::base64("hello") == "aGVsbG8=");
    CHECK(*text::fromBase64("aGVsbG8=") == "hello");
    CHECK(text::percentEncode("a b/c~") == "a%20b%2Fc~");
}
static void shortcutTests() {
    auto c = shortcut::parse("Ctrl+Shift+1"); CHECK(c && c->modifiers == (shortcut::modControl | shortcut::modShift) && c->virtualKey == '1');
    c = shortcut::parse("Meta+Print"); CHECK(c && c->modifiers == shortcut::modWin && c->virtualKey == 0x2C);
    CHECK(shortcut::parse("Ctrl++") && shortcut::parse("Ctrl++")->virtualKey == 0xBB);
    CHECK(!shortcut::parse("Ctrl+Shift")); CHECK(!shortcut::parse("Ctrl+A+B")); CHECK(!shortcut::parse("")); CHECK(!shortcut::parse("Ctrl+Bogus"));
    CHECK(shortcut::toPortableText(*shortcut::parse("shift+ctrl+f4")) == "Ctrl+Shift+F4");
}
static void settingsTests() {
    const auto defaults = Settings::defaults("C:\\Users\\lab\\Documents");
    auto s3 = [](const std::string& p, const json::Object& o) { return p == "s3" ? s3::validateSettings(o) : Error{"unknown_provider", "Unknown", {}}; };
    CHECK(!Settings::validate(defaults, s3));
    CHECK(defaults.value("archive").objectRef().value("root") == "C:\\Users\\lab\\Documents\\put.gd");
    auto settings = Settings::fromValidated(defaults);
    CHECK(settings.action("region").shortcut == "Ctrl+Shift+1");
    CHECK(settings.action("record").destination == "upload" && settings.action("record").shortcut.empty());
    CHECK(settings.limits.maxFileBytes == ResourceLimits::unlimitedBytes);
    const auto dev = Settings::developmentTemplate("/home/lab/Documents");
    CHECK(!Settings::validate(dev, s3));
    CHECK(Settings::fromValidated(dev).action("clipboard").profileId == "development-s3");
    auto broken = defaults; broken["extra"] = 1; CHECK(Settings::validate(broken, s3).code == "config_invalid");
    auto bad = defaults; { auto actions = bad.value("actions").toObject(); auto region = actions.value("region").toObject(); region["shortcut"] = "Ctrl+Nope"; actions["region"] = region; bad["actions"] = actions; }
    CHECK(Settings::validate(bad, s3).details.value("path") == "/actions/region/shortcut");
    // Legacy 2 GiB limits migrate to "auto"; legacy documents without edit/record normalize.
    auto legacy = defaults; { auto limits = legacy.value("limits").toObject(); limits["maxFileBytes"] = 2147483648.0; legacy["limits"] = limits; auto actions = legacy.value("actions").toObject(); actions.remove("edit"); actions.remove("record"); legacy["actions"] = actions; }
    CHECK(!Settings::validate(legacy, s3));
    CHECK(Settings::normalized(legacy).value("limits").objectRef().value("maxFileBytes") == "auto");
    CHECK(Settings::normalized(legacy).value("actions").objectRef().contains("edit"));
    // Inherited routes need captureDefaults.
    auto inherited = defaults; { auto actions = inherited.value("actions").toObject(); auto region = actions.value("region").toObject(); region["destination"] = "default"; actions["region"] = region; inherited["actions"] = actions; }
    CHECK(Settings::validate(inherited, s3));
    inherited["captureDefaults"] = json::Object{{"destination", "local"}, {"profileId", json::Value()}, {"publicationMode", "default"}};
    CHECK(!Settings::validate(inherited, s3));
    CHECK(Settings::effectiveAction(inherited, "region").value("destination") == "local");
    // Patch semantics: null is a value, arrays replace.
    const auto patched = settings::applyPatch(defaults, json::parseObject(R"({"retention":{"recentActivityLimit":77},"archive":{"folders":{"images":null}}})").value());
    CHECK(patched.value("retention").objectRef().value("recentActivityLimit").toInt() == 77);
    CHECK(patched.value("archive").objectRef().value("folders").objectRef().value("text").isNull());
    CHECK(!Settings::validate(patched, s3));
    json::Object reordered; CHECK(Settings::reorderProfiles(dev, json::Array{"development-s3"}, reordered).code.empty());
    CHECK(Settings::reorderProfiles(dev, json::Array{"x"}, reordered).code == "profile_order_invalid");
}
static void namingTests() {
    Artifact a; a.id = "0123456789abcdef"; a.name = "photo.PNG"; a.mime = "image/png";
    LocalTime t{2026, 10, 8, 4, 27, 21};
    CHECK(naming::renderTemplate("Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}", a, t, "u").name == "Screen-Capture-2026-10-08-04-27-21.PNG");
    CHECK(naming::validateTemplate("{bogus}").code == "filename_template_invalid");
    CHECK(naming::validateTemplate("a/b").code == "filename_invalid");
    Settings s = Settings::fromValidated(Settings::defaults("/d"));
    CHECK(naming::acquiredName(a, s, "region", t, "").name == "Screen-Capture-2026-10-08-04-27-21.PNG");
    CHECK(naming::acquiredName(a, s, "clipboard", t, "").name == "Image-2026-10-08-04-27-21.PNG");
    a.originalFile = true; a.name = "report.pdf"; CHECK(naming::acquiredName(a, s, "clipboard", t, "").name == "report.pdf");
    s.namingMode = "generated"; CHECK(naming::acquiredName(a, s, "clipboard", t, "").name == "0123456789abcdef.pdf");
    CHECK(naming::boundedName(std::string(300, 'x') + ".png", 255).size() == 255);
    CHECK(naming::renamedCandidate("a.txt", "deadbeef") == "a-deadbeef.txt");
    Artifact b = a; b.namingPrepared = true; std::vector<Artifact> batch{b, b, b};
    auto named = naming::batch(batch, "original", "rename", {"r1", "r2"});
    CHECK(named[0].name == "report.pdf" && named[1].name == "report-r1.pdf" && named[2].name == "report-r2.pdf");
    named = naming::batch(batch, "original", "skip", {}); CHECK(named[1].status == "skipped" && named[2].status == "skipped");
    named = naming::batch(batch, "original", "overwrite", {}); CHECK(named[0].status == "conflict" && named[2].status == "conflict");
    CHECK(naming::category("video/mp4") == "videos" && naming::category("application/zip") == "other");
}
static void protonTests() {
    const std::string node = std::string(44, 'A') + std::string(44, 'b') + "~" + std::string(22, 'z');
    CHECK(proton::nodeUid(node)); CHECK(proton::nodeUid(std::string(22, '_') + "~" + std::string(108, '=')));
    CHECK(!proton::nodeUid("abc~def")); CHECK(!proton::nodeUid(node + "~x")); CHECK(!proton::nodeUid(std::string(22, 'a') + "~" + std::string(22, '!')));
    auto s = proton::defaults();
    CHECK(!proton::validateSettings(s));
    s["folderUid"] = node; CHECK(proton::validateSettings(s).code == "config_invalid");          // folder without account
    s["accountRootUid"] = node; CHECK(!proton::validateSettings(s));
    s["folderName"] = "Screenshots"; CHECK(!proton::validateSettings(s));
    s["folderName"] = ""; CHECK(proton::validateSettings(s)); s.remove("folderName");
    s["linkExpirationDays"] = 366; CHECK(proton::validateSettings(s)); s["linkExpirationDays"] = 365; CHECK(!proton::validateSettings(s));
    s["requestTimeoutMs"] = 999; CHECK(proton::validateSettings(s)); s["requestTimeoutMs"] = proton::maximumRequestTimeoutMs; CHECK(!proton::validateSettings(s));
    s["sharePasswordReference"] = ""; CHECK(proton::validateSettings(s)); s["sharePasswordReference"] = "putgd/proton/link/abc"; CHECK(!proton::validateSettings(s));
    s["extra"] = 1; CHECK(proton::validateSettings(s)); s.remove("extra");
    CHECK(!proton::validateCredentials({{"mode", "keychain"}, {"reference", "putgd/proton/x"}}));
    CHECK(proton::validateCredentials({{"mode", "session"}, {"reference", json::Value()}}).code == "config_invalid");
    CHECK(proton::serializationKey(s, {}, "a.png") == crypto::Sha256::hexDigest(json::serialize(json::Array{node, node, "a.png"})));
    CHECK(proton::serializationKey(s, {{"accountRootUid", "r"}, {"folderUid", "f"}, {"name", "n"}}, "ignored") == crypto::Sha256::hexDigest(json::serialize(json::Array{"r", "f", "n"})));
    json::Object credentials{{"mode", "keychain"}, {"reference", "putgd/proton/x"}};
    json::Object profile{{"id", "p"}, {"provider", "proton-drive"}, {"credentials", credentials}, {"settings", s}};
    auto accepted = s; accepted["requestTimeoutMs"] = 30000;
    CHECK(proton::retryRequestTimeout("p", "proton-drive", credentials, accepted, profile) == proton::maximumRequestTimeoutMs);
    auto shorter = profile; { auto cs = s; cs["requestTimeoutMs"] = 5000; shorter["settings"] = cs; }
    CHECK(proton::retryRequestTimeout("p", "proton-drive", credentials, accepted, shorter) == 30000); // never shortens the accepted budget
    auto other = profile; { auto cs = s; cs["folderUid"] = node; cs["accountRootUid"] = std::string(22, 'q') + "~" + std::string(22, 'q'); other["settings"] = cs; }
    CHECK(!proton::retryRequestTimeout("p", "proton-drive", credentials, accepted, other));
    CHECK(!proton::retryRequestTimeout("other", "proton-drive", credentials, accepted, profile));
}
static void s3Tests() {
    auto settings = Settings::developmentTemplate("/d").value("profiles").arrayRef()[0].objectRef().value("settings").toObject();
    CHECK(!s3::validateSettings(settings));
    CHECK(s3::publicObjectUrl(settings, "a b/c.png") == "https://put.gd/a%20b/c.png");
    settings["publicUrlBase"] = json::Value();
    CHECK(s3::publicObjectUrl(settings, "x.png") == "https://s3.us-east-2.amazonaws.com/put.gd/x.png");
    CHECK(s3::requestHost(settings) == "s3.us-east-2.amazonaws.com");
    CHECK(s3::requestPath(settings, "x.png") == "/put.gd/x.png");
    settings["bucket"] = "plainbucket"; settings["addressing"] = "auto";
    CHECK(s3::virtualAddressing(settings));
    CHECK(s3::publicObjectUrl(settings, "x.png") == "https://plainbucket.s3.us-east-2.amazonaws.com/x.png");
    CHECK(s3::requestHost(settings) == "plainbucket.s3.us-east-2.amazonaws.com" && s3::requestPath(settings, "d/x.png") == "/d/x.png");
    settings["addressing"] = "virtual"; settings["bucket"] = "dotted.bucket"; CHECK(s3::validateSettings(settings));
    settings["bucket"] = "okay"; settings["endpoint"] = "http://127.0.0.1:9000"; CHECK(s3::validateSettings(settings)); // virtual + literal IP
    settings["addressing"] = "path"; CHECK(!s3::validateSettings(settings));
    settings["endpoint"] = "http://example.com"; CHECK(s3::validateSettings(settings));
    settings["endpoint"] = "https://user@example.com"; CHECK(s3::validateSettings(settings));
    settings["endpoint"] = "https://example.com"; settings["objectPrefix"] = "shots"; CHECK(s3::objectKey(settings, "a.png") == "shots/a.png");
    settings["objectPrefix"] = "shots/"; CHECK(s3::objectKey(settings, "a.png") == "shots/a.png");
}
static std::string chunk(const std::string& type, const std::string& data) {
    std::string out; unsigned long n = data.size(); for (int i = 3; i >= 0; --i) out += char((n >> (8 * i)) & 0xFF);
    const std::string body = type + data; out += body; const auto crc = png::crc32(body); for (int i = 3; i >= 0; --i) out += char((crc >> (8 * i)) & 0xFF); return out;
}
static void pngTests() {
    const std::string ihdr = std::string("\0\0\0\x02\0\0\0\x03", 8) + std::string("\x08\x06\0\0\0", 5);
    std::string good = std::string(png::signature) + chunk("IHDR", ihdr) + chunk("IDAT", "xx") + chunk("IEND", "");
    int w = 0, h = 0; CHECK(!png::inspect(good, &w, &h) && w == 2 && h == 3);
    std::string text = std::string(png::signature) + chunk("IHDR", ihdr) + chunk("tEXt", "Comment\0secret") + chunk("IDAT", "xx") + chunk("IEND", "");
    CHECK(png::inspect(text).code == "png_chunk_refused");
    CHECK(png::stripDisallowedChunks(text) == good);
    CHECK(png::inspect(good + "trailing").code == "png_invalid");
    auto corrupt = good; corrupt[corrupt.size() - 1] ^= 1; CHECK(png::inspect(corrupt).code == "png_invalid");
    CHECK(png::crc32("IEND") == 0xAE426082UL);
}
static void operationTests() {
    Operation op; op.id = "op"; op.action = "region"; op.created = "2026-10-22T00:00:00.000Z"; op.invocation = 7;
    OperationItem item; item.artifact.id = "a"; item.artifact.path = "C:\\private\\a"; item.uploadStatus = "uploaded"; item.receipt.directUrl = "https://x/y"; op.items.push_back(item);
    const auto round = Operation::fromJson(op.toJson());
    CHECK(round.invocation == 7 && round.items.size() == 1 && round.items[0].linkAvailable());
    CHECK(!op.publicJson().contains("settings"));
    CHECK(!op.publicJson().value("items").arrayRef()[0].objectRef().value("artifact").objectRef().contains("path"));
    item.remoteStatus = "removed"; CHECK(!item.linkAvailable());
    // The multipart resume journal is durable on the item but never public.
    op.items[0].uploadJournal = json::Object{{"schemaVersion", 1}, {"partBytes", 16777216.0}, {"uploadId", "abc"}};
    CHECK(Operation::fromJson(op.toJson()).items[0].uploadJournal.value("uploadId").toString() == "abc");
    CHECK(!op.publicJson().value("items").arrayRef()[0].objectRef().contains("uploadJournal"));
    CHECK(Operation::activeState("editing") && !Operation::activeState("completed"));
}
int main() {
    jsonTests(); shaTests(); shortcutTests(); settingsTests(); namingTests(); s3Tests(); protonTests(); operationTests(); pngTests();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::printf("windows core contract: ok\n");
    return 0;
}
