#include "resident/UpdateService.h"
#include "common/BuildInfo.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Process.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "resident/Resident.h"
#include <cstdio>
#include <windows.h>
#include <winhttp.h>

namespace putgd {
namespace {
constexpr long long dayMs = 24LL * 3600 * 1000;
struct Version { int major = 0, minor = 0, patch = 0; int kind = 2; int number = 0; bool valid = false; }; // kind: 0 nightly, 1 rc, 2 release
Version parseVersion(const std::string& text) {
    Version v;
    const auto dash = text.find('-');
    const auto base = text.substr(0, dash);
    if (std::sscanf(base.c_str(), "%d.%d.%d", &v.major, &v.minor, &v.patch) != 3) return v;
    if (dash != std::string::npos) {
        const auto rest = text.substr(dash + 1);
        // "nightly.N", a bare "nightly" (calendar builds), "rc.N" and local "dev.N" stamps.
        if (text::startsWith(rest, "nightly")) { v.kind = 0; v.number = rest.size() > 8 ? std::atoi(rest.c_str() + 8) : 0; }
        else if (text::startsWith(rest, "rc")) { v.kind = 1; v.number = rest.size() > 3 ? std::atoi(rest.c_str() + 3) : 0; }
        else if (text::startsWith(rest, "dev.")) { v.kind = 0; v.number = std::atoi(rest.c_str() + 4); }
        else return v;
    }
    v.valid = true;
    return v;
}
std::string feedBase() {
    wchar_t buffer[2048]; const DWORD n = GetEnvironmentVariableW(L"PUTGD_UPDATE_FEED_URL", buffer, 2048);
    return n > 0 && n < 2048 ? paths::narrow(buffer) : std::string("https://app.put.gd/update/v1/");
}
std::string httpGet(const std::string& url, DWORD& status, std::string& failure) {
    const auto parsed = text::parseUrl(url);
    if (!parsed.valid) { failure = "update_feed_invalid"; return {}; }
    HINTERNET session = WinHttpOpen(L"put.gd/windows", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(L"put.gd/windows", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { failure = "update_check_failed"; return {}; }
    WinHttpSetTimeouts(session, 10000, 10000, 20000, 20000);
    const bool secure = parsed.scheme == "https";
    HINTERNET connection = WinHttpConnect(session, paths::wide(parsed.host).c_str(), INTERNET_PORT(parsed.port >= 0 ? parsed.port : (secure ? 443 : 80)), 0);
    std::string body;
    if (connection) {
        const auto path = parsed.path.empty() ? std::string("/") : parsed.path;
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", paths::wide(path + (parsed.hasQuery ? "?" + parsed.query : "")).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = sizeof status; WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &size, WINHTTP_NO_HEADER_INDEX);
            char chunk[16384]; DWORD got = 0;
            while (WinHttpReadData(request, chunk, sizeof chunk, &got) && got > 0) { body.append(chunk, got); if (body.size() > 256 * 1024) { failure = "update_feed_invalid"; break; } }
        } else failure = "update_feed_unavailable";
        if (request) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
    } else failure = "update_feed_unavailable";
    WinHttpCloseHandle(session);
    return body;
}
}
std::string UpdateService::buildChannel() { return text::contains(build::version, "nightly") || text::contains(build::version, "dev.") ? "nightly" : "stable"; }
int UpdateService::compareVersions(const std::string& a, const std::string& b) {
    const auto x = parseVersion(a), y = parseVersion(b);
    if (!x.valid || !y.valid) return 0;
    if (x.major != y.major) return x.major < y.major ? -1 : 1;
    if (x.minor != y.minor) return x.minor < y.minor ? -1 : 1;
    if (x.patch != y.patch) return x.patch < y.patch ? -1 : 1;
    if (x.kind != y.kind) return x.kind < y.kind ? -1 : 1;
    if (x.number != y.number) return x.number < y.number ? -1 : 1;
    return 0;
}
int UpdateService::childMain(const std::vector<std::string>& args) {
    const std::string channel = args.size() > 1 ? args[1] : buildChannel();
    const std::string outputPath = args.size() > 2 ? args[2] : std::string();
    auto emit = [&outputPath](const json::Object& o) {
        const auto line = json::serialize(o) + "\n";
        if (!outputPath.empty()) files::writeAtomically(outputPath, line);
        else { std::fwrite(line.data(), 1, line.size(), stdout); std::fflush(stdout); }
    };
    if (channel != "stable" && channel != "nightly") { emit({{"ok", false}, {"error", Error{"update_check_unavailable", "Unknown update channel", {}}.toJson()}}); return 1; }
    DWORD status = 0; std::string failure;
    const auto body = httpGet(feedBase() + channel + ".json", status, failure);
    if (!failure.empty() || status != 200) { emit({{"ok", false}, {"error", Error{failure.empty() ? "update_feed_unavailable" : failure, "The update feed could not be fetched", {{"httpStatus", double(status)}}}.toJson()}}); return 1; }
    const auto feed = json::parseObject(body);
    if (!feed || !json::integer(feed->value("schema"), 1, 1) || feed->value("channel") != channel.c_str()) { emit({{"ok", false}, {"error", Error{"update_feed_invalid", "The update feed is not a valid version-1 document", {}}.toJson()}}); return 1; }
    json::Object result{{"channel", channel}, {"generatedAt", feed->value("generatedAt")}};
    const auto& release = feed->value("release");
    if (release.isObject()) {
        const auto version = release["version"].toString();
        if (release["tag"].toString() != "v" + version || (channel == "nightly") != text::contains(version, "nightly")) { emit({{"ok", false}, {"error", Error{"update_feed_invalid", "The feed names a release outside its channel", {}}.toJson()}}); return 1; }
        result["latest"] = version; result["tag"] = release["tag"]; result["publishedAt"] = release["publishedAt"]; result["notesUrl"] = release["notesUrl"];
        result["assets"] = release["assets"];
    } else result["latest"] = json::Value();
    emit({{"ok", true}, {"result", result}});
    return 0;
}
UpdateService::UpdateService(std::string root, SettingsService& settings) : root_(std::move(root)), statePath_(paths::join(root_, "update-check.json")), settings_(settings) { load(); }
std::string UpdateService::channel() const { const auto s = settings_.snapshot().updateChannel; return s == "build" ? buildChannel() : s; }
void UpdateService::load() {
    const auto bytes = files::read(statePath_, 256 * 1024);
    if (!bytes) return;
    const auto o = json::parseObject(*bytes).value_or(json::Object{});
    result_ = o.value("result").toObject(); notified_ = o.value("notified").toObject(); checkedAt_ = o.value("checkedAt").toString(); attemptedAt_ = o.value("attemptedAt").toString();
}
void UpdateService::save() const { files::writeAtomically(statePath_, json::serialize(json::Object{{"result", result_}, {"notified", notified_}, {"checkedAt", checkedAt_}, {"attemptedAt", attemptedAt_}})); }
void UpdateService::schedule() {
    if (timer_) { Loop::instance().stopTimer(timer_); timer_ = 0; }
    if (!settings_.snapshot().checkForUpdates) { nextCheckAt_.clear(); return; }
    const long long last = checkedAt_.empty() ? -1 : files::parseIsoUtcMs(checkedAt_);
    const long long now = files::parseIsoUtcMs(files::nowIsoUtc());
    long long delay = last < 0 ? 180000 : std::max(60000LL, last + dayMs - now);
    nextCheckAt_ = files::isoUtcFromMs(now + delay);
    timer_ = Loop::instance().startTimer(int(std::min(delay, 3600000LL)), [this] { timer_ = 0; const long long n = files::parseIsoUtcMs(files::nowIsoUtc()); if (files::parseIsoUtcMs(nextCheckAt_) <= n) check(false, {}); else schedule(); }, false);
}
void UpdateService::start() { schedule(); settings_.onChanged([this] { schedule(); }); }
json::Object UpdateService::status() const {
    const auto snapshot = settings_.snapshot();
    const auto current = channel();
    json::Object build{{"version", build::version}, {"kind", "portable"}, {"channel", buildChannel()}, {"packageTarget", "windows"}, {"supported", false}, {"reason", "Windows builds are updated by replacing the executables; download the release from the link"}};
    std::string state = "not_checked";
    if (result_.value("channel") == current.c_str()) {
        if (!result_.value("latest").isString()) state = "no_release";
        else state = compareVersions(result_.value("latest").stringRef(), build::version) > 0 ? "update_available" : "up_to_date";
    }
    return {{"build", build}, {"check", snapshot.checkForUpdates}, {"channelSetting", snapshot.updateChannel}, {"channel", current}, {"enabled", snapshot.checkForUpdates}, {"notifies", true},
        {"checking", checking_}, {"attemptedAt", attemptedAt_}, {"checkedAt", checkedAt_}, {"nextCheckAt", nextCheckAt_}, {"result", result_}, {"state", state}, {"error", lastError_.toJson()}};
}
void UpdateService::check(bool manual, std::function<void(Error, json::Object)> reply) {
    if (checking_) { if (reply) reply({"update_check_busy", "An update check is already running", {}}, {}); return; }
    checking_ = true;
    attemptedAt_ = files::nowIsoUtc();
    const auto channelName = channel();
    auto exitCode = std::make_shared<int>(-1);
    const auto outputPath = paths::join(root_, "update-check-" + uuid::create() + ".json");
    Loop::instance().background([exitCode, channelName, outputPath] {
        auto child = process::start(paths::executablePath(), {"--check-update", channelName, outputPath}, true);
        if (!child) return;
        child->channel->close();
        child->waitExit(90000);
        if (child->running()) child->terminate();
        *exitCode = child->exitCode();
    }, [this, exitCode, reply, channelName, manual, outputPath] {
        checking_ = false;
        // The child writes one JSON object to the private output file.
        const auto bytes = files::read(outputPath, 256 * 1024).value_or(std::string());
        files::remove(outputPath);
        const auto parsed = json::parseObject(bytes);
        if (!parsed || !parsed->value("ok").isBool()) lastError_ = {"update_check_failed", "The update check did not complete", {{"exitCode", double(*exitCode)}}};
        else if (!parsed->value("ok").toBool()) lastError_ = Error::fromJson(parsed->value("error").toObject());
        else {
            lastError_ = {}; result_ = parsed->value("result").toObject(); checkedAt_ = files::nowIsoUtc();
            if (result_.value("latest").isString() && compareVersions(result_.value("latest").stringRef(), build::version) > 0 && notified_.value(channelName) != result_.value("latest") && onUpdateAvailable) {
                notified_[channelName] = result_.value("latest"); onUpdateAvailable(result_);
            }
        }
        save();
        schedule();
        log::info("updates", "Update check finished", {{"channel", channelName}, {"manual", manual}, {"error", lastError_.code}});
        if (reply) { if (lastError_) reply(lastError_, {}); else reply({}, status()); }
    });
}
}
