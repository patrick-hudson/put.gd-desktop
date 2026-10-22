// Portable checks for the Windows logging and diagnostics core (#125): the log's
// redaction and record schema, rotation and retention, helper stderr collection,
// crash records, unclean-exit detection and the diagnostics package, with
// synthetic canaries the way the Linux test_logging/test_diagnostics_bundle do.
#include "common/Redact.h"
#include "common/Text.h"
#include "core/CrashRecord.h"
#include "core/DiagnosticsPackage.h"
#include "core/LogFormat.h"
#include "core/UncleanExit.h"
#include <cstdio>

using namespace putgd;
static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::fprintf(stderr, "FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); ++failures; } } while (0)
static bool has(const std::string& s, const std::string& needle) { return s.find(needle) != std::string::npos; }

// Synthetic values only: none of these is a real credential.
static const std::string secretKey = "wJalrXUtnFEMI/K7MDENG/bPxRfiCYEXAMPLEKEY"; // 40 chars, mixed case
static const std::string accessKey = "AKIAIOSFODNN7EXAMPLE";

static void redactionTests() {
    CHECK(redact::text("Authorization: AWS4-HMAC-SHA256 Credential=x/y, Signature=abc\nnext") == "Authorization: <redacted>\nnext");
    CHECK(redact::text("proxy-authorization=Basic Zm9vOmJhcg==") == "proxy-authorization=<redacted>");
    const auto presigned = redact::text("GET https://bucket.s3.amazonaws.com/a/b.png?X-Amz-Signature=deadbeef&X-Amz-Credential=AKIA/x&response=1 HTTP");
    CHECK(text::startsWith(presigned, "GET https://bucket.s3.amazonaws.com/a/b.png?<redacted>") && !has(presigned, "deadbeef") && !has(presigned, "AKIA/x") && text::endsWith(presigned, " HTTP"));
    CHECK(redact::text("GET https://bucket.s3.amazonaws.com/a/b.png?response-content-type=x HTTP") == "GET https://bucket.s3.amazonaws.com/a/b.png?<redacted> HTTP");
    CHECK(redact::text("share https://drive.proton.me/urls/ABC#sharePassword1") == "share https://drive.proton.me/urls/ABC?<redacted>");
    CHECK(redact::text("X-Amz-Security-Token: FQoGZXIvYXdzE") == "X-Amz-Security-Token: <redacted>");
    CHECK(redact::text("{\"password\": \"hunter22\", \"user\":\"lab\"}") == "{\"password\": \"<redacted>\", \"user\":\"lab\"}");
    CHECK(redact::text("aws_secret_access_key=abc123 region=us-east-1") == "aws_secret_access_key=<redacted> region=us-east-1");
    CHECK(redact::text("client_secret: s3cr3t;") == "client_secret: <redacted>;");
    CHECK(redact::text("Bearer abcdefgh12345678") == "Bearer <redacted>");
    CHECK(redact::text("key " + accessKey + " used") == "key <access-key-id> used");
    CHECK(redact::text("secret " + secretKey + " end") == "secret <redacted> end");
    CHECK(redact::text("commit 0123456789abcdef0123456789abcdef01234567") == "commit 0123456789abcdef0123456789abcdef01234567"); // lowercase hex stays
    CHECK(redact::text(std::string(130, 'A') + " tail") == "<redacted-blob> tail");
    CHECK(redact::text("-----BEGIN RSA PRIVATE KEY-----\nMIIEpA\n-----END RSA PRIVATE KEY----- after") == "<redacted-private-key> after");
    CHECK(redact::text("-----BEGIN PRIVATE KEY-----\nMIIEpA unterminated") == "<redacted-private-key>");
    CHECK(redact::text("tokenizer=fine") == "tokenizer=fine");
    CHECK(redact::text("plain text with a path C:\\Users\\lab\\x.png") == "plain text with a path C:\\Users\\lab\\x.png");
    // A very long line is scanned without recursion.
    std::string longLine; for (int i = 0; i < 40000; ++i) longLine += "word ";
    CHECK(redact::text(longLine) == longLine);
    CHECK(redact::sensitiveKey("secretKey") && redact::sensitiveKey("session_token") && redact::sensitiveKey("Clipboard-Text") && !redact::sensitiveKey("profileId"));
    const auto o = redact::object({{"text", "clipboard canary"}, {"nested", json::Object{{"password", "x"}, {"note", "Bearer abcdefgh12345678"}}}, {"code", "s3_access_denied"}});
    CHECK(o.value("text") == "<redacted>");
    CHECK(o.value("nested")["password"] == "<redacted>");
    CHECK(o.value("nested")["note"] == "Bearer <redacted>");
    CHECK(o.value("code") == "s3_access_denied");
}
static void recordTests() {
    CHECK(logformat::iso(0) == "1970-01-01T00:00:00.000Z");
    const auto t = logformat::parseIso("2026-10-22T06:15:00.123Z");
    CHECK(t > 0 && logformat::iso(t) == "2026-10-22T06:15:00.123Z");
    CHECK(logformat::parseIso("2026-10-22T06:15:00Z") == t - 123);
    CHECK(logformat::parseIso("2026-13-22T06:15:00Z") == -1 && logformat::parseIso("yesterday") == -1);
    CHECK(logformat::compactStamp(t) == "20261022T061500123");
    logformat::Record r; r.timeMs = t; r.level = logformat::Level::Warning; r.category = "operations"; r.process = "putgd"; r.pid = 4242;
    r.message = "Upload failed for https://b.example/k.png?X-Amz-Signature=abc"; r.fields = {{"operationId", "op-1"}, {"itemId", "it-1"}, {"secretKey", secretKey}, {"code", "s3_access_denied"}};
    const auto line = logformat::encode(r);
    CHECK(line.back() == '\n');
    const auto parsed = json::parseObject(line);
    CHECK(parsed && parsed->value("v").toInt() == 1 && parsed->value("pid").toInt() == 4242 && parsed->value("category") == "putgd.operation" && parsed->value("level") == "warning");
    CHECK(parsed && parsed->value("operationId") == "op-1" && parsed->value("itemId") == "it-1" && !parsed->value("fields").objectRef().contains("operationId"));
    CHECK(parsed && parsed->value("fields")["secretKey"] == "<redacted>" && parsed->value("fields")["code"] == "s3_access_denied");
    CHECK(!has(line, secretKey) && !has(line, "X-Amz-Signature=abc"));
    r.message.clear(); for (int i = 0; i < 2500; ++i) r.message += "m "; std::string big; for (int i = 0; i < 10000; ++i) big += "b "; r.fields = {{"big", big}};
    const auto capped = json::parseObject(logformat::encode(r));
    CHECK(capped && capped->value("message").stringRef().size() == 4096 && capped->value("fields")["truncated"].toBool());
    CHECK(logformat::category("session") == "putgd.ipc" && logformat::category("putgd.s3") == "putgd.s3" && logformat::category("startup") == "putgd.app");
}
static void fileTests() {
    const auto t = logformat::parseIso("2026-10-22T06:15:00.123Z");
    const auto name = logformat::fileName(t, 4242, 3);
    CHECK(name == "putgd-20261022T061500123Z-4242-3.jsonl");
    CHECK(logformat::isFileName(name) && !logformat::isFileName("putgd-20261022.log") && !logformat::isFileName("putgd-20261022T061500123Z-4242-.jsonl") && !logformat::isFileName("other.jsonl"));
    CHECK(logformat::rotationLimit(52428800) == 5LL * 1024 * 1024 && logformat::rotationLimit(1048576) == 262144 && logformat::rotationLimit(100000) == 65536);
    const long long day = 86400000;
    std::vector<logformat::FileEntry> entries{{"putgd-20261001T000000000Z-1-1.jsonl", 100, t - 20 * day}, {"putgd-20261020T000000000Z-1-1.jsonl", 600, t - 2 * day},
        {"putgd-20261021T000000000Z-1-1.jsonl", 600, t - day}, {"putgd-20261022T000000000Z-1-1.jsonl", 600, t}, {"notes.txt", 999999, 0}};
    const auto removed = logformat::expired(entries, t, 14, 1300, "putgd-20261022T000000000Z-1-1.jsonl");
    CHECK(removed.size() == 2 && removed[0] == "putgd-20261001T000000000Z-1-1.jsonl" && removed[1] == "putgd-20261020T000000000Z-1-1.jsonl");
    // The file being written stays even when it alone exceeds the limit.
    CHECK(logformat::expired({{"putgd-20261022T000000000Z-1-1.jsonl", 5000, t}}, t, 14, 1000, "putgd-20261022T000000000Z-1-1.jsonl").empty());
    CHECK(logformat::helperProcessName("C:\\putgd\\putgd.exe", {"--capture", "region"}) == "putgd-capture");
    CHECK(logformat::helperProcessName("C:\\putgd\\proton\\putgd-proton-runtime.exe", {"--stdio"}) == "putgd-proton-runtime");
}
static void helperTests() {
    std::vector<logformat::Record> out;
    logformat::HelperOutput h("putgd-capture", 77, [&](logformat::Record r) { out.push_back(std::move(r)); }, [] { return 1000LL; });
    logformat::Record child; child.timeMs = 500; child.level = logformat::Level::Info; child.category = "capture"; child.process = ""; child.pid = 77; child.message = "Selection finished"; child.fields = {{"operationId", "op-9"}};
    h.append(logformat::encode(child));
    h.append("plain warning from a library\r\npartial");
    h.append(" line\n");
    CHECK(out.size() == 3);
    CHECK(out[0].message == "Selection finished" && out[0].level == logformat::Level::Info && out[0].process == "putgd-capture" && out[0].pid == 77 && out[0].fields.value("operationId") == "op-9" && out[0].timeMs == 500);
    CHECK(out[1].message == "plain warning from a library" && out[1].level == logformat::Level::Warning && out[1].fields.value("stream") == "stderr");
    CHECK(out[2].message == "partial line");
    out.clear();
    h.append(std::string(5000, 'z') + "\n");
    CHECK(out.size() == 1 && out[0].message.size() == logformat::HelperOutput::lineBytes);
    for (int i = 0; i < 100; ++i) h.append(std::string(1000, 'q') + "\n");
    h.append("putgd-crash\tbegin\t1\tputgd-capture\t77\t78\nputgd-crash\texception\t0xC0000005\tEXCEPTION_ACCESS_VIOLATION\t0x0\tread\nputgd-crash\tend\t0\n");
    h.finish();
    bool truncated = false, crashed = false;
    long long kept = 0;
    for (const auto& r : out) { if (r.message == "Helper output truncated") truncated = r.fields.value("suppressedBytes").toInteger() > 0; if (r.message == "Helper crash record") crashed = r.fields.value("crash")["exceptionCode"] == "0xC0000005"; if (r.fields.value("stream") == "stderr") kept += (long long)r.message.size() + 1; }
    CHECK(truncated && crashed && kept <= logformat::HelperOutput::generationBytes + 2048);
}
static const char* crashText =
    "putgd-crash\tbegin\t1\tputgd\t4242\t4243\n"
    "putgd-crash\tversion\t26.1022.990-dev\n"
    "putgd-crash\texception\t0xC0000005\tEXCEPTION_ACCESS_VIOLATION\t0x0000000000000000\twrite\n"
    "putgd-crash\ttime\t1792650000\n"
    "putgd-crash\tpc\t0x00007ff6a1b21234\n"
    "putgd-crash\tframe\t0\t0x00007ff6a1b21234\t0\t0x21234\n"
    "putgd-crash\tframe\t1\t0x00007ffb10002000\t-\t0x0\n"
    "putgd-crash\tmodule\t0\t0x00007ff6a1b00000\t6717a2b0b4000\tC:\\putgd\\putgd.exe\n"
    "putgd-crash\tend\t2\n";
static void crashTests() {
    const auto r = crash::parseRecord(crashText);
    CHECK(r.value("process") == "putgd" && r.value("pid").toInt() == 4242 && r.value("thread").toInt() == 4243 && r.value("version") == "26.1022.990-dev");
    CHECK(r.value("exceptionCode") == "0xC0000005" && r.value("access") == "write" && r.value("complete").toBool());
    CHECK(r.value("frames").arrayRef().size() == 2 && r.value("frames")[1]["module"].isNull() && r.value("modules")[0]["imageId"] == "6717a2b0b4000");
    CHECK(crash::parseRecord("nothing here").empty());
    CHECK(!crash::parseRecord("putgd-crash\tbegin\t1\tputgd\t1\t2\n").value("complete").toBool());
    CHECK(std::string(crash::exceptionName(0xC00000FDUL)) == "EXCEPTION_STACK_OVERFLOW");
}
static void uncleanTests() {
    unclean::Previous p;
    CHECK(unclean::evaluate(p, "boot-1", "2026-10-22T07:00:00.000Z").empty());
    p.present = true; p.marker = unclean::marker(4242, "2026-10-22T06:00:00.000Z", "26.1022.990-dev", "boot-1");
    p.alive = true;
    CHECK(unclean::evaluate(p, "boot-1", "now").empty());
    p.alive = false;
    auto r = unclean::evaluate(p, "boot-1", "2026-10-22T07:00:00.000Z");
    CHECK(r.value("cause") == "unknown" && r.value("previousPid").toInt() == 4242 && r.value("sameBoot").toBool() && r.value("previousVersion") == "26.1022.990-dev");
    CHECK(unclean::level(r) == logformat::Level::Warning);
    p.note = unclean::note("session_end", "logoff", "2026-10-22T06:59:00.000Z");
    r = unclean::evaluate(p, "boot-2", "now");
    CHECK(r.value("cause") == "signal" && r.value("event") == "session_end" && r.value("detail") == "logoff" && !r.value("sameBoot").toBool());
    CHECK(unclean::level(r) == logformat::Level::Info);
    p.crashText = crashText;
    r = unclean::evaluate(p, "boot-1", "now");
    CHECK(r.value("cause") == "crash" && r.value("exceptionCode") == "0xC0000005" && r.value("crash")["frames"].arrayRef().size() == 2 && unclean::level(r) == logformat::Level::Error);
    // A crash record from another process id is not this resident's.
    p.marker = unclean::marker(1, "x", "v", "boot-1");
    CHECK(unclean::evaluate(p, "boot-1", "now").value("cause") == "signal");
    json::Array recent;
    for (int i = 0; i < 12; ++i) recent = unclean::remember(recent, json::Object{{"n", i}});
    CHECK(recent.size() == 10 && recent[0]["n"].toInt() == 11 && recent[9]["n"].toInt() == 2);
}
static Operation failedOperation(const std::string& id, const std::string& created) {
    Operation op; op.id = id; op.action = "region"; op.state = "failed"; op.created = created;
    op.destination.providerId = "s3"; op.destination.profileId = "work-s3";
    OperationItem item; item.artifact.id = "item-" + id; item.artifact.name = "Quarterly Secret Plan.png"; item.artifact.mime = "image/png"; item.artifact.bytes = 1234;
    item.artifact.path = "C:\\Users\\lab\\AppData\\Local\\put.gd\\artifacts\\" + id + "\\item.png";
    item.candidate = "Quarterly Secret Plan.png"; item.uploadStatus = "failed"; item.objectReference = {{"key", "shots/Quarterly Secret Plan.png"}};
    item.local.status = "saved"; item.local.path = "C:\\Users\\lab\\Documents\\put.gd\\Quarterly Secret Plan.png";
    item.error = {"s3_access_denied", "Access Denied", {{"httpStatus", 403}, {"requestId", "REQ123"}}};
    op.items.push_back(item); op.error = item.error;
    return op;
}
static std::string logLine(long long ms, const std::string& message, json::Object fields) {
    logformat::Record r; r.timeMs = ms; r.category = "operations"; r.process = "putgd"; r.pid = 9; r.message = message; r.fields = std::move(fields);
    return logformat::encode(r);
}
static void packageTests() {
    diagnostics::Request req;
    CHECK(!diagnostics::parse({{"since", "24h"}, {"includeNames", true}, {"output", "D:\\out"}}, req) && req.sinceMs == 86400000 && req.includeNames && req.output == "D:\\out");
    diagnostics::Request bad;
    CHECK(diagnostics::parse({{"since", "0d"}}, bad).code == "invalid_request");
    CHECK(diagnostics::parse({{"since", "366d"}}, bad).code == "invalid_request");
    CHECK(diagnostics::parse({{"copyLink", true}}, bad).code == "invalid_request");
    CHECK(diagnostics::parse({{"preview", true}, {"upload", true}}, bad).code == "invalid_request");
    CHECK(diagnostics::parse({{"preview", true}, {"notify", true}}, bad).code == "invalid_request");
    CHECK(diagnostics::parse({{"output", "relative\\x.zip"}}, bad).code == "invalid_request");
    CHECK(diagnostics::parse({{"surprise", 1}}, bad).code == "invalid_request");

    const long long now = logformat::parseIso("2026-10-22T12:00:00.000Z"), hour = 3600000;
    diagnostics::Inputs in;
    in.nowMs = now; in.key = std::string(32, '\x5a');
    in.build = {{"version", "26.1022.990-dev"}};
    in.home = "C:\\Users\\lab";
    in.roots = {{"C:\\Users\\lab\\AppData\\Local\\put.gd", "<data>"}, {"C:\\Users\\lab\\Documents\\put.gd", "<archive>"}};
    in.settings = {{"schemaVersion", 1}, {"archive", json::Object{{"root", "C:\\Users\\lab\\Documents\\put.gd"}, {"keepUploads", true}}},
        {"profiles", json::Array{json::Object{{"id", "work-s3"}, {"label", "Team bucket"}, {"provider", "s3"},
            {"settings", json::Object{{"bucket", "acme-private-bucket"}, {"region", "us-east-1"}, {"publicUrlBase", "https://cdn.acme.example/shots"}}},
            {"credentials", json::Object{{"mode", "keychain"}, {"reference", "putgd/work-s3-ref"}}}}}}};
    in.diagnostics = {{"logging", json::Object{{"directory", "C:\\Users\\lab\\AppData\\Local\\put.gd\\logs"}}}};
    in.environment = {{"os", json::Object{{"build", 26200}}}, {"displays", 1}};
    in.crashDumps = {{"available", true}, {"dumps", json::Array{json::Object{{"file", "C:\\Users\\lab\\AppData\\Local\\put.gd\\crashdumps\\putgd.exe.1234.dmp"}, {"bytes", 100}}}}};
    in.operations = {failedOperation("op-a", logformat::iso(now - 2 * hour))};
    in.uploadedPackages = {"op-diag"};
    std::string log;
    log += logLine(now - 30 * 24 * hour, "Too old", {});
    log += logLine(now - 2 * hour, "Upload attempt", {{"operationId", "op-a"}, {"itemId", "item-op-a"}, {"name", "Quarterly Secret Plan.png"}});
    log += logLine(now - 2 * hour + 1, "Upload failed", {{"operationId", "op-a"}, {"itemId", "item-op-a"}, {"path", "C:\\Users\\lab\\Documents\\put.gd\\Quarterly Secret Plan.png"},
        {"message", "PUT https://acme-private-bucket.s3.amazonaws.com/shots/Quarterly%20Secret%20Plan.png?X-Amz-Signature=abc denied"}});
    log += logLine(now - hour, "Diagnostics package uploaded", {{"operationId", "op-diag"}});
    log += logLine(now - hour + 5, "Clipboard text canary", {{"text", "synthetic-private clipboard"}, {"secretKey", secretKey}});
    in.logs = {{"putgd-20261022T000000000Z-1-1.jsonl", log}};
    in.defaultFolder = in.previewFolder = "C:\\Users\\lab\\Documents\\put.gd\\diagnostics";

    diagnostics::Request hashed;
    const auto pkg = diagnostics::build(in, hashed);
    std::string all; for (const auto& f : pkg.files) all += f.second;
    CHECK(!pkg.zip.empty() && pkg.zip.compare(0, 4, "PK\x03\x04") == 0 && pkg.zip.find("PK\x05\x06") != std::string::npos);
    CHECK(diagnostics::fileNames(pkg).size() == 7);
    CHECK(!has(all, "Quarterly Secret Plan") && !has(all, "acme-private-bucket") && !has(all, "Team bucket") && !has(all, "cdn.acme.example") && !has(all, "putgd/work-s3-ref"));
    CHECK(!has(all, secretKey) && !has(all, "synthetic-private") && !has(all, "X-Amz-Signature=abc") && !has(all, "C:\\\\Users\\\\lab") && !has(all, "\"Too old\""));
    CHECK(!has(all, "Diagnostics package uploaded"));
    CHECK(has(all, "<archive>\\\\<path:") && has(all, "<data>\\\\crashdumps") && has(all, "\"mode\": \"keychain\"") && has(all, "us-east-1") && has(all, "REQ123"));
    CHECK(pkg.records == 3 && pkg.droppedRecords == 0 && pkg.operationList.size() == 1 && pkg.operationList[0]["items"][0]["attempts"].toInt() == 1);
    const auto config = json::parseObject(pkg.files[2].second);
    CHECK(config && config->value("archive")["root"] == "<home>\\Documents\\put.gd");
    CHECK(pkg.manifest.value("redaction") == "hashed" && pkg.manifest.value("files").arrayRef().size() == 6 && pkg.manifest.value("scope")["since"] == "10080m");
    // One value, one token, within a package; a different key gives different tokens.
    diagnostics::Redactor a(false, "k1"), b(false, "k2");
    CHECK(a.token("name", "x.png") == a.token("name", "x.png") && a.token("name", "x.png") != b.token("name", "x.png") && text::endsWith(a.token("name", "x.png"), ".png>"));
    CHECK(a.text("see https://github.com/patrick-hudson/put.gd-desktop/releases.") == "see https://github.com/patrick-hudson/put.gd-desktop/releases.");

    diagnostics::Request names; names.includeNames = true;
    const auto named = diagnostics::build(in, names);
    std::string allNamed; for (const auto& f : named.files) allNamed += f.second;
    CHECK(has(allNamed, "Quarterly Secret Plan") && !has(allNamed, secretKey) && !has(allNamed, "putgd/work-s3-ref") && named.manifest.value("redaction") == "names");

    diagnostics::Request op; op.operationId = "op-a";
    const auto scoped = diagnostics::build(in, op);
    CHECK(scoped.scope.value("operationId") == "op-a" && scoped.scope.value("to") == logformat::iso(now - 2 * hour + 1 + 600000) && scoped.records == 2);

    diagnostics::Request preview; preview.preview = true;
    const auto shown = diagnostics::build(in, preview);
    CHECK(shown.zip.empty() && shown.preview.value("preview").toBool() && shown.preview.value("logRecords").toInt() == 3 && shown.preview.value("operationList")[0]["items"].toInt() == 1);

    // The size limit drops the oldest records first.
    diagnostics::Inputs big = in; std::string many, padding; for (int i = 0; i < 450; ++i) padding += "p ";
    big.maximumBytes = 1024 * 1024;
    for (int i = 0; i < 3000; ++i) many += logLine(now - 3 * hour + i, "Filler " + std::to_string(i), {{"pad", padding}});
    big.logs = {{"putgd-20261022T000000000Z-1-1.jsonl", many}};
    const auto limited = diagnostics::build(big, hashed);
    CHECK(limited.droppedRecords > 0 && limited.zip.size() <= size_t(big.maximumBytes) && limited.eligibleRecords == 3000);
    CHECK(has(limited.files.back().second, "Filler 2999") && !has(limited.files.back().second, "\"Filler 0\""));

    CHECK(diagnostics::baseName(now) == "putgd-diagnostics-20261022-120000Z");
    CHECK(diagnostics::isPackageName("putgd-diagnostics-20261022-120000Z.zip") && diagnostics::isPackageName("putgd-diagnostics-20261022-120000Z-2.zip") && !diagnostics::isPackageName("putgd-diagnostics-x.zip") && !diagnostics::isPackageName("notes.zip"));
    std::vector<diagnostics::Saved> saved;
    for (int i = 0; i < 12; ++i) saved.push_back({"putgd-diagnostics-20261022-1200" + std::string(i < 10 ? "0" : "") + std::to_string(i) + "Z.zip", now - i * hour});
    saved.push_back({"putgd-diagnostics-20260101-000000Z.zip", now - 40 * 24 * hour});
    saved.push_back({"mine.zip", 0});
    const auto removed = diagnostics::pruned(saved, now);
    CHECK(removed.size() == 3);

    CHECK(diagnostics::credentialOrConfigurationError("s3_access_denied") && diagnostics::credentialOrConfigurationError("credential_setup_required") && diagnostics::credentialOrConfigurationError("proton_auth_failed"));
    CHECK(!diagnostics::credentialOrConfigurationError("s3_network") && !diagnostics::credentialOrConfigurationError(""));
    const auto t = diagnostics::target("p", {{"label", "Lab"}, {"provider", "proton-drive"}});
    CHECK(t.value("shareExpirationDays").toInt() == 7 && t.value("publicLink").toBool());
}
int main() {
    redactionTests(); recordTests(); fileTests(); helperTests(); crashTests(); uncleanTests(); packageTests();
    if (failures) { std::fprintf(stderr, "%d failure(s)\n", failures); return 1; }
    std::puts("windows diagnostics contract: ok");
    return 0;
}
