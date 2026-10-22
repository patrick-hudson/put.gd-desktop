#include "core/LogFormat.h"
#include "common/Redact.h"
#include "common/Text.h"
#include "core/CrashRecord.h"
#include <algorithm>
#include <cstdio>
#include <map>

namespace putgd::logformat {
namespace {
long long daysFromCivil(long long y, unsigned m, unsigned d) {
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = unsigned(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long long)doe - 719468;
}
void civilFromDays(long long z, long long& y, unsigned& m, unsigned& d) {
    z += 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = unsigned(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}
struct Fields { long long year; unsigned month, day, hour, minute, second, millisecond; };
Fields split(long long ms) {
    long long days = ms / 86400000, rest = ms % 86400000;
    if (rest < 0) { rest += 86400000; --days; }
    Fields f{};
    civilFromDays(days, f.year, f.month, f.day);
    f.hour = unsigned(rest / 3600000); f.minute = unsigned(rest / 60000 % 60); f.second = unsigned(rest / 1000 % 60); f.millisecond = unsigned(rest % 1000);
    return f;
}
bool digits(std::string_view s, size_t from, size_t count, unsigned& value) {
    value = 0;
    if (from + count > s.size()) return false;
    for (size_t i = from; i < from + count; ++i) { if (s[i] < '0' || s[i] > '9') return false; value = value * 10 + unsigned(s[i] - '0'); }
    return true;
}
// Keeps whole UTF-8 sequences; invalid bytes become U+FFFD.
std::string utf8(std::string_view s, size_t maximumCodePoints) {
    std::string out; size_t points = 0;
    for (size_t i = 0; i < s.size() && points < maximumCodePoints; ++points) {
        const auto c = static_cast<unsigned char>(s[i]);
        size_t length = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        if (length && i + length <= s.size() && text::validUtf8(s.substr(i, length))) { out.append(s.substr(i, length)); i += length; }
        else { out += "\xEF\xBF\xBD"; ++i; }
    }
    return out;
}
}
const char* levelName(Level l) {
    switch (l) { case Level::Debug: return "debug"; case Level::Info: return "info"; case Level::Warning: return "warning"; case Level::Error: return "error"; default: return "fatal"; }
}
std::optional<Level> parseLevel(std::string_view s) {
    if (s == "debug") return Level::Debug;
    if (s == "info") return Level::Info;
    if (s == "warning") return Level::Warning;
    if (s == "error") return Level::Error;
    if (s == "fatal") return Level::Fatal;
    return std::nullopt;
}
std::string category(std::string_view name) {
    if (text::startsWith(name, "putgd.") || text::startsWith(name, "qt.") || name == "default") return std::string(name);
    static const std::map<std::string, std::string, std::less<>> table{
        {"operations", "putgd.operation"}, {"artifacts", "putgd.operation"}, {"history", "putgd.history"}, {"settings", "putgd.settings"},
        {"session", "putgd.ipc"}, {"ui", "putgd.ui"}, {"editor", "putgd.ui"}, {"tray", "putgd.ui"}, {"capture", "putgd.capture"},
        {"image-job", "putgd.capture"}, {"s3", "putgd.s3"}, {"s3-worker", "putgd.s3"}, {"proton", "putgd.proton"}, {"recording", "putgd.recording"}};
    const auto it = table.find(name);
    return it == table.end() ? std::string("putgd.app") : it->second;
}
std::string iso(long long ms) {
    const auto f = split(ms);
    char buffer[40];
    std::snprintf(buffer, sizeof buffer, "%04lld-%02u-%02uT%02u:%02u:%02u.%03uZ", f.year, f.month, f.day, f.hour, f.minute, f.second, f.millisecond);
    return buffer;
}
std::string compactStamp(long long ms) {
    const auto f = split(ms);
    char buffer[32];
    std::snprintf(buffer, sizeof buffer, "%04lld%02u%02uT%02u%02u%02u%03u", f.year, f.month, f.day, f.hour, f.minute, f.second, f.millisecond);
    return buffer;
}
long long parseIso(std::string_view s) {
    unsigned y, mo, d, h, mi, se, ms = 0;
    if (!digits(s, 0, 4, y) || s.size() < 20 || s[4] != '-' || !digits(s, 5, 2, mo) || s[7] != '-' || !digits(s, 8, 2, d) || s[10] != 'T' ||
        !digits(s, 11, 2, h) || s[13] != ':' || !digits(s, 14, 2, mi) || s[16] != ':' || !digits(s, 17, 2, se)) return -1;
    if (s.size() == 24) { if (s[19] != '.' || !digits(s, 20, 3, ms) || s[23] != 'Z') return -1; }
    else if (s.size() != 20 || s[19] != 'Z') return -1;
    if (mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || se > 60) return -1;
    return daysFromCivil(y, mo, d) * 86400000 + (long long)h * 3600000 + (long long)mi * 60000 + (long long)se * 1000 + ms;
}
std::string encode(const Record& r) {
    json::Object o{{"v", 1}, {"time", iso(r.timeMs)}, {"level", levelName(r.level)}, {"category", utf8(category(r.category), 64)}, {"process", utf8(r.process, 64)},
        {"pid", double(r.pid)}, {"message", utf8(redact::text(r.message), 4096)}};
    auto fields = redact::object(r.fields);
    for (const auto* key : {"operationId", "itemId"}) if (fields.value(key).isString()) { o[key] = utf8(fields.value(key).stringRef(), 128); fields.remove(key); }
    if (!fields.empty()) o["fields"] = fields;
    auto line = json::serialize(o);
    if (line.size() > 16384) { o["fields"] = json::Object{{"truncated", true}}; line = json::serialize(o); }
    return line + "\n";
}
long long rotationLimit(long long retentionBytes) { return std::clamp(retentionBytes / 4, 64LL * 1024, defaultRotationBytes); }
std::string fileName(long long startMs, long long pid, int sequence) {
    return "putgd-" + compactStamp(startMs) + "Z-" + std::to_string(pid) + "-" + std::to_string(sequence) + ".jsonl";
}
bool isFileName(std::string_view n) {
    // ^putgd-\d{8}T\d{9}Z-\d+-\d+\.jsonl$
    unsigned ignored;
    if (!text::startsWith(n, "putgd-") || !text::endsWith(n, ".jsonl") || n.size() < 6 + 18 + 1 + 3 + 6) return false;
    if (!digits(n, 6, 8, ignored) || n[14] != 'T' || !digits(n, 15, 9, ignored) || n[24] != 'Z' || n[25] != '-') return false;
    const auto rest = n.substr(26, n.size() - 26 - 6);
    const auto dash = rest.find('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= rest.size()) return false;
    for (size_t i = 0; i < rest.size(); ++i) if (i != dash && (rest[i] < '0' || rest[i] > '9')) return false;
    return true;
}
std::vector<std::string> expired(std::vector<FileEntry> entries, long long nowMs, int days, long long bytes, const std::string& current) {
    std::sort(entries.begin(), entries.end(), [](const FileEntry& a, const FileEntry& b) { return a.name < b.name; });
    std::vector<std::string> removed;
    std::vector<FileEntry> kept;
    const long long cutoff = nowMs - (long long)days * 86400000LL;
    long long total = 0;
    for (const auto& e : entries) {
        if (!isFileName(e.name)) continue;
        if (e.name != current && e.modifiedMs < cutoff) { removed.push_back(e.name); continue; }
        kept.push_back(e); total += e.bytes;
    }
    for (const auto& e : kept) {
        if (total <= bytes) break;
        if (e.name == current) continue;
        removed.push_back(e.name); total -= e.bytes;
    }
    return removed;
}

HelperOutput::HelperOutput(std::string process, long long pid, Sink sink, Clock clock) : process_(std::move(process)), pid_(pid), sink_(std::move(sink)), clock_(std::move(clock)) {}
void HelperOutput::emit(Level level, const std::string& cat, const std::string& message, json::Object fields) {
    Record r; r.timeMs = clock_(); r.level = level; r.category = cat; r.process = process_; r.pid = pid_; r.message = message; r.fields = std::move(fields);
    sink_(std::move(r));
}
void HelperOutput::append(std::string_view bytes) {
    partial_.append(bytes);
    for (auto newline = partial_.find('\n'); newline != std::string::npos; newline = partial_.find('\n')) { line(partial_.substr(0, newline)); partial_.erase(0, newline + 1); }
    // A helper record can be up to 16 KiB; anything longer without a newline is cut here.
    if (partial_.size() > 16384 + 64) { line(partial_); partial_.clear(); }
}
void HelperOutput::line(std::string s) {
    if (!s.empty() && s.back() == '\r') s.pop_back();
    if (text::trim(s).empty()) return;
    if (crash::isRecordLine(s)) {
        if (crash_.size() + s.size() < crashBytes) crash_ += s + "\n";
        if (text::startsWith(s, "putgd-crash\tend")) {
            const auto record = crash::parseRecord(crash_); crash_.clear();
            if (!record.empty()) emit(Level::Error, "putgd.app", "Helper crash record", {{"helper", process_}, {"crash", record}});
        }
        return;
    }
    if (logged_ >= generationBytes) { suppressed_ += (long long)s.size() + 1; return; }
    if (!s.empty() && s.front() == '{') {
        const auto record = json::parseObject(s);
        if (record && record->value("v").toInt() == 1 && record->value("message").isString()) {
            logged_ += (long long)s.size() + 1;
            auto fields = record->value("fields").toObject();
            for (const auto* key : {"operationId", "itemId"}) if (record->value(key).isString()) fields[key] = record->value(key);
            Record r; r.timeMs = parseIso(record->value("time").toString()); if (r.timeMs < 0) r.timeMs = clock_();
            r.level = parseLevel(record->value("level").toString()).value_or(Level::Warning);
            r.category = record->value("category").toString("putgd.app"); r.process = process_; r.pid = pid_;
            r.message = record->value("message").stringRef(); r.fields = std::move(fields);
            sink_(std::move(r));
            return;
        }
    }
    s = utf8(s, lineBytes);
    if (s.size() > lineBytes) s = utf8(s.substr(0, lineBytes), lineBytes);
    logged_ += (long long)s.size() + 1;
    emit(Level::Warning, "putgd.app", s, {{"stream", "stderr"}});
}
void HelperOutput::finish() {
    if (!partial_.empty()) { line(partial_); partial_.clear(); }
    if (!crash_.empty()) { // cut short: keep what arrived
        const auto record = crash::parseRecord(crash_); crash_.clear();
        if (!record.empty()) emit(Level::Error, "putgd.app", "Helper crash record", {{"helper", process_}, {"crash", record}});
    }
    if (suppressed_ > 0) emit(Level::Warning, "putgd.app", "Helper output truncated", {{"suppressedBytes", double(suppressed_)}, {"keptBytes", double(logged_)}});
    suppressed_ = logged_ = 0;
}
std::string helperProcessName(const std::string& executable, const std::vector<std::string>& arguments) {
    auto base = executable.substr(executable.find_last_of("\\/") + 1);
    if (text::endsWith(text::lower(base), ".exe")) base.resize(base.size() - 4);
    if (text::lower(base) != "putgd" || arguments.empty()) return base;
    static const std::map<std::string, std::string> modes{{"--capture", "putgd-capture"}, {"--image-job", "putgd-image-job"}, {"--s3-worker", "putgd-s3-worker"},
        {"--ui", "putgd-ui"}, {"--edit-image", "putgd-editor"}, {"--check-update", "putgd-update-check"}, {"--record", "putgd-record"}};
    const auto it = modes.find(arguments.front());
    return it == modes.end() ? base : it->second;
}
}
