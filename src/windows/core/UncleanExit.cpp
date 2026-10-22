#include "core/UncleanExit.h"
#include "common/Text.h"
#include "core/CrashRecord.h"

namespace putgd::unclean {
json::Object marker(long long pid, const std::string& started, const std::string& version, const std::string& bootId) {
    return {{"pid", double(pid)}, {"started", started}, {"version", version}, {"bootId", bootId}};
}
std::string note(const std::string& event, const std::string& detail, const std::string& at) {
    return json::serialize(json::Object{{"event", event}, {"detail", detail}, {"at", at}}) + "\n";
}
json::Object evaluate(const Previous& p, const std::string& bootId, const std::string& now) {
    if (!p.present || p.alive) return {};
    json::Object noted;
    for (const auto& line : text::split(p.note, '\n')) if (auto o = json::parseObject(text::trim(line)); o && o->value("event").isString()) noted = *o;
    const auto crash = crash::parseRecord(p.crashText);
    const bool crashed = !crash.empty() && p.marker.value("pid").isNumber() && crash.value("pid").toDouble() == p.marker.value("pid").toDouble();
    const auto previousBoot = p.marker.value("bootId").toString();
    json::Object record{{"detectedAt", now}, {"previousPid", p.marker.value("pid")}, {"previousStart", p.marker.value("started")}, {"previousVersion", p.marker.value("version")},
        {"sameBoot", !bootId.empty() && previousBoot == bootId}, {"cause", crashed ? "crash" : !noted.empty() ? "signal" : "unknown"}};
    if (crashed) { record["exceptionCode"] = crash.value("exceptionCode"); record["exceptionName"] = crash.value("exceptionName"); record["crash"] = crash; }
    else if (!noted.empty()) { record["event"] = noted.value("event").stringRef().substr(0, 64); record["detail"] = noted.value("detail").toString().substr(0, 128); }
    return record;
}
json::Array remember(json::Array recent, const json::Object& record) {
    json::Array result{record};
    for (auto& value : recent) { if (int(result.size()) >= retainedRecords) break; if (value.isObject()) result.push_back(std::move(value)); }
    return result;
}
logformat::Level level(const json::Object& r) {
    const auto cause = r.value("cause").toString();
    // A logoff's WM_ENDSESSION is expected; anything else ended without a clean shutdown.
    return cause == "crash" ? logformat::Level::Error : cause == "signal" ? logformat::Level::Info : logformat::Level::Warning;
}
std::string message(const json::Object& r) {
    const auto cause = r.value("cause").toString();
    return cause == "crash" ? "The previous resident crashed" : cause == "signal" ? "The previous resident was stopped by the session or a stop request"
                                                                                : "The previous resident did not shut down cleanly";
}
}
