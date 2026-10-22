// putgd-cli.exe: the JSON session client (docs/cli.md) over the named pipe.
#include "common/BuildInfo.h"
#include "common/Error.h"
#include "common/Json.h"
#include "common/Paths.h"
#include "common/Pipe.h"
#include "common/Protocol.h"
#include "common/Uuid.h"
#include <cstdio>
#include <string>
#include <vector>
#include <windows.h>

namespace {
using namespace putgd;
std::string response(const std::string& id, const Error& error, const json::Object& result) {
    return json::serialize(json::Object{{"schemaVersion", 1}, {"requestId", id}, {"ok", !error}, {"result", error ? json::Object{} : result}, {"error", error.toJson()}}) + "\n";
}
int replyDeadlineMs(const std::string& method) {
    if (method == "diagnostics.bundle" || method == "diagnostics.uploadTarget") return 10 * 60 * 1000;
    if (method == "update.check") return 60000;
    if (method == "history.preview") return 50000;
    if (method == "setup.testDestination") return 20000;
    return 15000;
}
void emit(const std::string& line) { std::fwrite(line.data(), 1, line.size(), stdout); std::fflush(stdout); }
}

int wmain(int argc, wchar_t** argv) {
    std::vector<std::string> args;
    for (int i = 1; i < argc; ++i) args.push_back(paths::narrow(argv[i]));
    if (args.size() == 1 && args[0] == "--version-json") { std::printf("%s\n", json::serialize(protocol::buildIdentity()).c_str()); return 0; }
    if (args.size() == 1 && (args[0] == "--version" || args[0] == "-v")) { std::printf("putgd-cli %s\n", build::version); return 0; }
    std::string session, requestId, method, params = "{}";
    json::Object bundleOptions; bool bundleFlags = false;
    std::vector<std::string> positional;
    for (size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        auto value = [&](std::string& out) { if (i + 1 >= args.size()) return false; out = args[++i]; return true; };
        if (a == "--socket" || a == "--pipe") { if (!value(session)) return (emit(response("", {"invalid_request", "--socket needs a pipe path", {}}, {})), 1); }
        else if (a == "--request-id") { if (!value(requestId)) return (emit(response("", {"invalid_request", "--request-id needs a value", {}}, {})), 1); }
        else if (a == "--help" || a == "-h") { std::printf("Usage: putgd-cli [--socket PIPE] [--request-id ID] METHOD [JSON_PARAMS]\nJSON client for the resident put.gd session (see docs/cli.md).\n"); return 0; }
        else if (a == "--operation" || a == "--since" || a == "--output" || a == "--profile") { std::string v; if (!value(v)) return (emit(response("", {"invalid_request", a + " needs a value", {}}, {})), 1); bundleFlags = true;
            bundleOptions[a == "--operation" ? "operation" : a == "--since" ? "since" : a == "--profile" ? "profileId" : "output"] = v; }
        else if (a == "--include-names") { bundleFlags = true; bundleOptions["includeNames"] = true; }
        else if (a == "--upload") { bundleFlags = true; bundleOptions["upload"] = true; }
        else if (a == "--copy-link") { bundleFlags = true; bundleOptions["copyLink"] = true; }
        else positional.push_back(a);
    }
    if (requestId.empty()) requestId = uuid::create();
    auto fail = [&](const std::string& code, const std::string& message) { emit(response(requestId, {code, message, {}}, {})); return 1; };
    if (positional.empty() || positional.size() > 2) return fail("invalid_request", "Supply a method and optional JSON parameter object");
    method = positional[0];
    if (method == "setup.storeCredentials" || method == "setup.storeSharePassword") return fail("credentials_require_settings", "Enter credentials through Settings.");
    if (method == "setup.revealCredentialSecret") return fail("credentials_require_settings", "View credentials through Settings.");
    if (positional.size() == 2) params = positional[1];
    if (params.size() > pipe::maximumRequestBytes) return fail("request_too_large", "Request exceeds 1 MiB");
    auto parsed = json::parseObject(params);
    if (!parsed) return fail("invalid_json", "Parameters must be a JSON object");
    if (bundleFlags) {
        if (method != "diagnostics.bundle" || positional.size() == 2) return fail("invalid_request", "--operation, --since, --include-names, --output, --upload, --profile and --copy-link belong to diagnostics.bundle without a JSON parameter object");
        if ((bundleOptions.contains("profileId") || bundleOptions.contains("copyLink")) && !bundleOptions.contains("upload")) return fail("invalid_request", "--profile and --copy-link need --upload");
        if (bundleOptions.contains("output") && !paths::isAbsolute(bundleOptions.value("output").toString())) {
            wchar_t full[32768]; GetFullPathNameW(paths::wide(bundleOptions.value("output").toString()).c_str(), 32768, full, nullptr); bundleOptions["output"] = paths::narrow(full);
        }
        parsed = bundleOptions;
    }
    if (session.empty()) session = paths::sessionPipePath();
    const auto request = json::serialize(json::Object{{"schemaVersion", 1}, {"requestId", requestId}, {"session", session}, {"method", method}, {"params", *parsed}});
    std::string error;
    const auto reply = pipe::request(session, request, replyDeadlineMs(method), &error);
    if (!reply) {
        if (error == "session_unavailable") return fail("session_unavailable", "Start the resident put.gd app in this user session");
        if (error == "response_too_large") return fail("response_too_large", "Resident response exceeds 16 MiB");
        return fail("transport_failed", "Resident session did not return a bounded response");
    }
    const auto object = json::parseObject(*reply);
    if (!object || !json::integer(object->value("schemaVersion"), 1, 1) || object->value("requestId") != requestId.c_str() || !object->value("ok").isBool())
        return fail("invalid_response", "Resident response does not match request schema and ID");
    emit(json::serialize(*object) + "\n");
    return object->value("ok").toBool() ? 0 : 1;
}
