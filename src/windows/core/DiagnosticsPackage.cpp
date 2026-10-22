#include "core/DiagnosticsPackage.h"
#include "common/Redact.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include "core/LogFormat.h"
#include "core/Zip.h"
#include <algorithm>
#include <map>

namespace putgd::diagnostics {
namespace {
const std::vector<std::string> nameKeys{"name", "candidate", "fileName", "folderName", "label", "notificationText"};
const std::vector<std::string> urlKeys{"url", "directUrl", "publicUrl", "publicUrlBase", "link", "shareUrl"};
const std::vector<std::string> objectKeys{"key", "objectKey", "objectPrefix"};
const std::vector<std::string> idKeys{"accountRootUid", "folderUid", "shareUid", "linkUid", "volumeId", "nodeUid"};
const std::vector<std::string> pathKeys{"path", "localPath", "previewPath", "directory", "stagingPath", "root", "from", "to", "modulePath", "configPath", "file", "folder"};
bool in(const std::vector<std::string>& list, const std::string& key) { return std::find(list.begin(), list.end(), key) != list.end(); }
char lower(char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : c; }
bool separator(char c) { return c == '\\' || c == '/'; }
bool space(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
// Windows paths: case-insensitive, either separator.
bool samePathAt(const std::string& s, size_t at, const std::string& path) {
    if (at + path.size() > s.size()) return false;
    for (size_t i = 0; i < path.size(); ++i) {
        const char a = s[at + i], b = path[i];
        if (separator(a) && separator(b)) continue;
        if (lower(a) != lower(b)) return false;
    }
    return true;
}
bool absolute(const std::string& p) {
    return (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && separator(p[2])) || (p.size() > 2 && separator(p[0]) && separator(p[1]));
}
std::string trimSeparators(std::string p) { while (p.size() > 3 && separator(p.back())) p.pop_back(); return p; }
std::string indented(const json::Value& v) { return json::pretty(v); }
std::string extension(const std::string& value) {
    const auto slash = value.find_last_of("\\/");
    const auto name = slash == std::string::npos ? value : value.substr(slash + 1);
    const auto dot = name.find_last_of('.');
    if (dot == std::string::npos || dot + 1 >= name.size() || name.size() - dot - 1 > 8) return {};
    for (size_t i = dot + 1; i < name.size(); ++i) if (!std::isalnum(static_cast<unsigned char>(name[i]))) return {};
    return name.substr(dot);
}
json::Object itemSummary(const OperationItem& item, int attempts) {
    json::Object r{{"id", item.artifact.id}, {"mime", item.artifact.mime}, {"category", item.artifact.category}, {"bytes", double(item.artifact.bytes)},
        {"name", item.artifact.name}, {"candidate", item.candidate}, {"uploadStatus", item.uploadStatus}, {"phase", item.phase},
        {"local", json::Object{{"status", item.local.status}, {"path", item.local.path}, {"error", item.local.error.toJson()}}},
        {"shareStatus", item.shareStatus}, {"remoteStatus", item.remoteStatus}, {"attempts", attempts}};
    if (item.error) r["error"] = item.error.toJson();
    if (!item.receipt.directUrl.empty()) r["directUrl"] = item.receipt.directUrl;
    if (item.objectReference.contains("key")) r["objectKey"] = item.objectReference.value("key");
    return r;
}
// A field of an encoded log record without parsing it: records are compact JSON
// with sorted keys, and time and IDs never contain quotes.
std::string_view field(const std::string& line, std::string_view name) {
    const auto key = "\"" + std::string(name) + "\":\"";
    const auto at = line.find(key);
    if (at == std::string::npos) return {};
    const auto start = at + key.size(), end = line.find('"', start);
    return end == std::string::npos ? std::string_view() : std::string_view(line).substr(start, end - start);
}
}
Error parse(const json::Object& p, Request& r) {
    const Error invalid{"invalid_request", "diagnostics.bundle takes operation (an ID), since (1m to 365d, such as 7d or 24h), includeNames, output (an absolute .zip path or folder), preview, notify, and upload with profileId and copyLink", {}};
    static const std::vector<std::string> allowed{"operation", "since", "includeNames", "output", "preview", "notify", "upload", "profileId", "copyLink"};
    for (const auto& pair : p) if (!in(allowed, pair.first)) return invalid;
    auto flag = [&](const char* key, bool& out) { if (!p.contains(key)) return true; if (!p.value(key).isBool()) return false; out = p.value(key).toBool(); return true; };
    if (!flag("upload", r.upload) || !flag("includeNames", r.includeNames) || !flag("preview", r.preview) || !flag("notify", r.notify)) return invalid;
    if (p.contains("copyLink") && (!flag("copyLink", r.copyLink) || !r.upload)) return invalid;
    if (p.contains("profileId")) { const auto& v = p.value("profileId"); if (!v.isString() || v.stringRef().empty() || v.stringRef().size() > 64 || !r.upload) return invalid; r.profileId = v.stringRef(); }
    if (p.contains("operation")) { const auto& v = p.value("operation"); if (!v.isString() || v.stringRef().empty() || v.stringRef().size() > 128) return invalid; r.operationId = v.stringRef(); }
    if (p.contains("since")) {
        const auto& v = p.value("since");
        if (!v.isString()) return invalid;
        const auto& s = v.stringRef();
        if (s.size() < 2 || s.size() > 7 || s[0] < '1' || s[0] > '9') return invalid;
        for (size_t i = 0; i + 1 < s.size(); ++i) if (s[i] < '0' || s[i] > '9') return invalid;
        const char unit = s.back();
        if (unit != 'm' && unit != 'h' && unit != 'd') return invalid;
        r.sinceMs = std::stoll(s.substr(0, s.size() - 1)) * (unit == 'm' ? 60000LL : unit == 'h' ? 3600000LL : 86400000LL);
        if (r.sinceMs > maximumSinceMs) return invalid;
    }
    if (p.contains("output")) {
        const auto& v = p.value("output");
        if (!v.isString() || v.stringRef().empty() || v.stringRef().size() > 4096 || !absolute(v.stringRef())) return invalid;
        r.output = trimSeparators(v.stringRef());
    }
    // A preview writes nothing, so it uploads and notifies nothing either.
    if (r.preview && (p.contains("upload") || p.contains("copyLink") || p.contains("profileId") || p.contains("notify"))) return invalid;
    return {};
}

Redactor::Redactor(bool includeNames, std::string key) : names_(includeNames), key_(std::move(key)) {}
void Redactor::root(const std::string& raw, const std::string& placeholder) {
    const auto path = trimSeparators(raw);
    if (path.size() <= 3 || !absolute(path)) return;
    for (const auto& existing : roots_) if (existing.path.size() == path.size() && samePathAt(existing.path, 0, path)) return;
    roots_.push_back({path, placeholder});
    std::sort(roots_.begin(), roots_.end(), [](const Root& a, const Root& b) { return a.path.size() > b.path.size(); });
}
void Redactor::literal(const std::string& kind, const std::string& value) {
    if (names_ || value.size() < 4 || literals_.count(value)) return;
    const auto& t = literals_[value] = token(kind, value);
    auto& bucket = buckets_[value[0]];
    bucket.emplace_back(value, t);
    std::stable_sort(bucket.begin(), bucket.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
}
std::string Redactor::token(const std::string& kind, const std::string& value) const {
    const auto digest = crypto::hex(crypto::hmacSha256(key_, kind + "\n" + value)).substr(0, 12);
    const auto suffix = kind == "name" || kind == "key" || kind == "path" ? extension(value) : std::string();
    return "<" + kind + ":" + digest + suffix + ">";
}
// Known folders become placeholders; in hashed mode the rest of a path below the
// home folder or the archive becomes one token.
std::string Redactor::placeholders(std::string s) const {
    for (const auto& root : roots_) {
        std::string out;
        for (size_t i = 0; i < s.size();) {
            if (samePathAt(s, i, root.path)) {
                const size_t end = i + root.path.size();
                if (end == s.size() || separator(s[end]) || space(s[end]) || std::string("\"'<>,;:)]}").find(s[end]) != std::string::npos) { out += root.placeholder; i = end; continue; }
            }
            out += s[i++];
        }
        s = std::move(out);
    }
    return s;
}
std::string Redactor::paths(std::string s) const {
    s = placeholders(std::move(s));
    if (names_) return s;
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const char* placeholder = s.compare(i, 6, "<home>") == 0 ? "<home>" : s.compare(i, 9, "<archive>") == 0 ? "<archive>" : nullptr;
        if (placeholder) {
            const size_t length = std::char_traits<char>::length(placeholder);
            if (i + length < s.size() && separator(s[i + length])) {
                size_t k = i + length + 1;
                while (k < s.size() && !space(s[k]) && std::string("\"'<>,;)]}").find(s[k]) == std::string::npos) ++k;
                if (k > i + length + 1) { out += std::string(placeholder) + s[i + length] + token("path", s.substr(i + length + 1, k - i - length - 1)); i = k; continue; }
            }
        }
        out += s[i++];
    }
    return out;
}
std::string Redactor::literals(const std::string& s) const {
    if (literals_.empty()) return s;
    // Leftmost match, longest literal first, as one alternation would.
    std::string out;
    for (size_t i = 0; i < s.size();) {
        const auto it = buckets_.find(s[i]);
        bool replaced = false;
        if (it != buckets_.end()) for (const auto& pair : it->second) if (s.compare(i, pair.first.size(), pair.first) == 0) { out += pair.second; i += pair.first.size(); replaced = true; break; }
        if (!replaced) out += s[i++];
    }
    return out;
}
std::string Redactor::text(const std::string& input) const {
    auto s = paths(redact::text(input));
    if (names_) return s;
    s = literals(s);
    std::string out;
    for (size_t i = 0; i < s.size();) {
        size_t scheme = 0;
        const bool boundary = i == 0 || !(std::isalnum(static_cast<unsigned char>(s[i - 1])) || s[i - 1] == '_');
        if (boundary && (lower(s[i]) == 'h' || lower(s[i]) == 'w' || lower(s[i]) == 's')) for (const std::string_view c : {"https://", "http://", "wss://", "ws://", "s3://"}) {
            bool same = s.size() >= i + c.size();
            for (size_t k = 0; same && k < c.size(); ++k) same = lower(s[i + k]) == c[k];
            if (same) { scheme = c.size(); break; }
        }
        if (!scheme) { out += s[i++]; continue; }
        size_t k = i + scheme;
        while (k < s.size() && !space(s[k]) && std::string("\"'<>").find(s[k]) == std::string::npos) ++k;
        if (k == i + scheme) { out += s[i++]; continue; }
        auto value = s.substr(i, k - i);
        // Trailing punctuation belongs to the sentence, not the URL.
        while (!value.empty() && std::string(".,;:)]}").find(value.back()) != std::string::npos) value.pop_back();
        // put.gd's own release links identify a build, not a user.
        out += text::startsWith(value, "https://github.com/patrick-hudson/") ? value : token("url", value);
        i += value.size();
    }
    return out;
}
json::Value Redactor::json(const json::Value& v, const std::string& key) const {
    if (v.isString()) {
        const auto& s = v.stringRef();
        if (s.empty()) return v;
        if (!names_) {
            if (in(nameKeys, key)) return token("name", s);
            if (in(urlKeys, key)) return token("url", s);
            if (in(objectKeys, key)) return token("key", s);
            if (in(idKeys, key)) return token("id", s);
            if (key == "bucket") return token("bucket", s);
            if (key == "endpoint") return token("endpoint", s);
            if (in(pathKeys, key) && absolute(s)) {
                // A whole path value keeps its known folder and hides the rest, spaces included.
                const auto placed = placeholders(redact::text(s));
                for (const std::string p : {"<home>", "<archive>"})
                    if (text::startsWith(placed, p) && placed.size() > p.size() + 1 && separator(placed[p.size()])) return placed.substr(0, p.size() + 1) + token("path", placed.substr(p.size() + 1));
            }
        }
        return text(s);
    }
    if (v.isArray()) { json::Array a; for (const auto& entry : v.arrayRef()) a.push_back(json(entry, key)); return a; }
    if (v.isObject()) {
        const auto& o = v.objectRef();
        // A credential reference names a Credential Manager entry; keep only how it is stored.
        if (key == "credentials" || key == "sharePasswordReference") return json::Object{{"mode", o.value("mode").toString("<reference>")}};
        json::Object r;
        for (const auto& pair : o) r[pair.first] = json(pair.second, pair.first);
        return r;
    }
    if (key == "sharePasswordReference" && !v.isNull()) return json::Value("<reference>");
    return v;
}

long long windowStart(const Request& r, long long now, const std::string& created) {
    if (r.operationId.empty()) return now - r.sinceMs;
    const auto at = logformat::parseIso(created);
    return (at >= 0 ? at : now) - 600000;
}
Package build(const Inputs& in, const Request& request) {
    Package pkg;
    Redactor redactor(request.includeNames, in.key);
    for (const auto& root : in.roots) redactor.root(root.first, root.second);
    if (!in.home.empty()) redactor.root(in.home, "<home>");
    // Everything free text may name: destinations from settings, items from history.
    for (const auto& value : in.settings.value("profiles").toArray()) {
        const auto profile = value.toObject(); const auto settings = profile.value("settings").toObject();
        redactor.literal("label", profile.value("label").toString());
        redactor.literal("bucket", settings.value("bucket").toString()); redactor.literal("endpoint", settings.value("endpoint").toString());
        redactor.literal("url", settings.value("publicUrlBase").toString()); redactor.literal("name", settings.value("folderName").toString());
        for (const auto* key : {"accountRootUid", "folderUid"}) redactor.literal("id", settings.value(key).toString());
    }
    for (const auto& op : in.operations) for (const auto& item : op.items) {
        redactor.literal("name", item.artifact.name); redactor.literal("path", item.artifact.path); redactor.literal("path", item.artifact.previewPath);
        redactor.literal("name", item.candidate); redactor.literal("key", item.objectReference.value("key").toString());
        redactor.literal("url", item.receipt.directUrl); redactor.literal("path", item.local.path);
    }
    const long long from = windowStart(request, in.nowMs, in.operations.empty() ? std::string() : in.operations.front().created);
    long long to = in.nowMs;
    std::vector<std::string> needles;
    for (const auto& id : in.uploadedPackages) if (id != request.operationId) needles.push_back("\"operationId\":\"" + id + "\"");
    auto uploaded = [&needles](const std::string& line) { for (const auto& n : needles) if (line.find(n) != std::string::npos) return true; return false; };
    // The log files newest first, each split into lines.
    auto files = in.logs;
    std::sort(files.begin(), files.end(), [](const LogFile& a, const LogFile& b) { return a.name > b.name; });
    std::vector<std::vector<std::string>> lines;
    for (const auto& f : files) lines.push_back(text::split(f.bytes, '\n'));
    if (!request.operationId.empty()) {
        // Ten minutes after the operation's last record.
        const auto needle = "\"operationId\":\"" + request.operationId + "\"";
        std::string last;
        for (const auto& file : lines) for (const auto& line : file) if (line.find(needle) != std::string::npos) last = std::max(last, std::string(field(line, "time")));
        const auto end = logformat::parseIso(last);
        to = end >= 0 ? std::min(in.nowMs, end + 600000) : in.nowMs;
    }
    const auto fromText = logformat::iso(from), toText = logformat::iso(to);
    std::map<std::string, int> attempts;
    for (const auto& file : lines) for (const auto& line : file) {
        if (line.empty() || uploaded(line)) continue;
        const auto time = field(line, "time");
        if (time.empty() || time < fromText || time > toText) continue;
        if (line.find("\"message\":\"Upload attempt\"") != std::string::npos) attempts[std::string(field(line, "itemId"))] += 1;
    }
    for (const auto& op : in.operations) {
        if (in.uploadedPackages.count(op.id) && op.id != request.operationId) continue;
        json::Array items; for (const auto& item : op.items) items.push_back(itemSummary(item, attempts[item.artifact.id]));
        json::Object summary{{"id", op.id}, {"action", op.action}, {"state", op.state}, {"phase", op.phase}, {"created", op.created}, {"cancelled", op.cancelled},
            {"copyStatus", op.copyStatus}, {"provider", op.destination.providerId}, {"profileId", op.destination.profileId}, {"items", items}};
        if (op.error) summary["error"] = op.error.toJson();
        pkg.operationList.push_back(summary);
    }
    // Fixed parts first: the log records get what is left of the size limit.
    auto settings = in.settings;
    auto archive = settings.value("archive").toObject();
    auto home = [&](const std::string& path) {
        if (in.home.empty() || path.size() < in.home.size() || !samePathAt(path, 0, in.home)) return path;
        if (path.size() > in.home.size() && !separator(path[in.home.size()])) return path;
        return "<home>" + path.substr(in.home.size());
    };
    archive["root"] = home(archive.value("root").toString());
    auto folders = archive.value("folders").toObject();
    for (auto& pair : folders) if (pair.second.isString()) pair.second = home(pair.second.stringRef());
    archive["folders"] = folders;
    settings.remove("archive");
    auto config = redactor.json(settings).toObject(); config["archive"] = archive;
    auto crashDumps = in.crashDumps; crashDumps["since"] = fromText;
    const auto environmentBytes = indented(redactor.json(in.environment));
    const auto diagnosticsBytes = indented(redactor.json(in.diagnostics));
    const auto configBytes = indented(config);
    const auto crashBytes = indented(redactor.json(crashDumps));
    const auto operationsBytes = indented(redactor.json(pkg.operationList, "operations"));
    const long long fixed = (long long)(environmentBytes.size() + diagnosticsBytes.size() + configBytes.size() + crashBytes.size()) + std::min(256LL * 1024, in.maximumBytes / 80);
    const long long budget = in.maximumBytes - fixed - (long long)operationsBytes.size();
    std::vector<std::string> selected; long long used = 0;
    for (const auto& file : lines) for (auto it = file.rbegin(); it != file.rend(); ++it) {
        if (it->empty() || uploaded(*it)) continue;
        const auto time = field(*it, "time");
        if (time.empty() || time < fromText || time > toText) continue;
        ++pkg.eligibleRecords;
        // Newest first: once the limit is reached, the older records are dropped.
        if (used >= budget) { ++pkg.droppedRecords; continue; }
        const auto o = json::parseObject(*it);
        if (!o) { ++pkg.droppedRecords; continue; }
        auto bytes = json::serialize(redactor.json(*o)) + "\n";
        if (used + (long long)bytes.size() > budget) { ++pkg.droppedRecords; used = budget; continue; }
        used += (long long)bytes.size(); selected.push_back(std::move(bytes));
    }
    // Chosen newest first; written oldest first, in file order.
    std::reverse(selected.begin(), selected.end());
    std::string logBytes; for (const auto& line : selected) logBytes += line;
    pkg.records = (long long)selected.size();
    pkg.files = {{std::string("environment.json"), environmentBytes}, {std::string("diagnostics.json"), diagnosticsBytes}, {std::string("config.redacted.json"), configBytes},
        {std::string("operations.json"), operationsBytes}, {std::string("crashdumps.json"), crashBytes}, {std::string("logs/putgd.jsonl"), logBytes}};
    json::Array manifestFiles;
    for (const auto& [path, bytes] : pkg.files) manifestFiles.push_back(json::Object{{"path", path}, {"bytes", double(bytes.size())}, {"sha256", crypto::Sha256::hexDigest(bytes)}});
    pkg.scope = {{"from", fromText}, {"to", toText}};
    if (!request.operationId.empty()) pkg.scope["operationId"] = request.operationId; else pkg.scope["since"] = std::to_string(request.sinceMs / 60000) + "m";
    const std::string redaction = request.includeNames ? "names" : "hashed";
    pkg.manifest = {{"schemaVersion", 1}, {"created", logformat::iso(in.nowMs)}, {"build", in.build}, {"redaction", redaction}, {"scope", pkg.scope}, {"files", manifestFiles},
        {"operations", double(pkg.operationList.size())},
        {"logs", json::Object{{"records", double(pkg.records)}, {"eligibleRecords", double(pkg.eligibleRecords)}, {"droppedRecords", double(pkg.droppedRecords)}, {"sourceFiles", double(files.size())}}},
        {"maximumBytes", double(in.maximumBytes)}};
    if (request.preview) {
        json::Array summaries;
        for (const auto& v : pkg.operationList) { const auto& op = v.objectRef(); summaries.push_back(json::Object{{"id", op.value("id")}, {"action", op.value("action")}, {"state", op.value("state")}, {"created", op.value("created")}, {"items", double(op.value("items").arrayRef().size())}}); }
        pkg.preview = {{"preview", true}, {"files", json::stringArray(fileNames(pkg))}, {"redaction", redaction}, {"scope", pkg.scope}, {"operations", double(pkg.operationList.size())},
            {"operationList", summaries}, {"logRecords", double(pkg.records)}, {"droppedLogRecords", double(pkg.droppedRecords)}, {"folder", in.previewFolder},
            {"defaultFolder", in.previewDefaultFolder}, {"maximumBytes", double(in.maximumBytes)}};
        return pkg;
    }
    Zip zip(in.nowMs);
    zip.add("manifest.json", indented(pkg.manifest));
    for (const auto& [path, bytes] : pkg.files) zip.add(path, bytes);
    pkg.zip = zip.finish();
    return pkg;
}
std::vector<std::string> fileNames(const Package& pkg) {
    std::vector<std::string> names{"manifest.json"};
    for (const auto& entry : pkg.files) names.push_back(entry.first);
    return names;
}
std::string baseName(long long now) {
    const auto stamp = logformat::compactStamp(now); // yyyyMMddTHHmmsszzz
    return "putgd-diagnostics-" + stamp.substr(0, 8) + "-" + stamp.substr(9, 6) + "Z";
}
bool isPackageName(const std::string& n) {
    const std::string prefix = "putgd-diagnostics-";
    if (!text::startsWith(n, prefix) || !text::endsWith(n, ".zip")) return false;
    const auto rest = n.substr(prefix.size(), n.size() - prefix.size() - 4); // yyyyMMdd-HHmmssZ[-N]
    if (rest.size() < 16) return false;
    for (size_t i = 0; i < 15; ++i) if (i != 8 && !std::isdigit(static_cast<unsigned char>(rest[i]))) return false;
    if (rest[8] != '-' || rest[15] != 'Z') return false;
    if (rest.size() == 16) return true;
    if (rest[16] != '-' || rest.size() == 17) return false;
    for (size_t i = 17; i < rest.size(); ++i) if (!std::isdigit(static_cast<unsigned char>(rest[i]))) return false;
    return true;
}
std::vector<std::string> pruned(std::vector<Saved> saved, long long now) {
    saved.erase(std::remove_if(saved.begin(), saved.end(), [](const Saved& s) { return !isPackageName(s.name); }), saved.end());
    std::sort(saved.begin(), saved.end(), [](const Saved& a, const Saved& b) { return a.modifiedMs > b.modifiedMs; });
    std::vector<std::string> removed;
    const long long cutoff = now - (long long)keptDays * 86400000LL;
    for (size_t i = 0; i < saved.size(); ++i) if (int(i) >= keptPackages || saved[i].modifiedMs < cutoff) removed.push_back(saved[i].name);
    return removed;
}
std::string defaultUploadProfile(const Settings& settings) {
    for (const auto& action : {std::string("region"), std::string("window"), std::string("desktop"), std::string("clipboard"), std::string("edit"), Settings::recordingActionName()}) {
        const auto& route = settings.action(action);
        if (route.destination == "upload" && settings.profiles.count(route.profileId)) return route.profileId;
    }
    return {};
}
bool credentialOrConfigurationError(const std::string& code) {
    if (code.empty()) return false;
    if (text::startsWith(code, "credential_") || text::startsWith(code, "config_")) return true;
    for (const char* part : {"auth", "permission_denied", "access_denied", "mfa_required", "bucket_not_found", "bucket_invalid", "region_mismatch", "settings_invalid", "acl_incompatible",
                             "setup_required", "account_changed", "folder_unavailable", "destination_unavailable", "keychain_unavailable", "provider_unavailable", "unsupported_capability"})
        if (code.find(part) != std::string::npos) return true;
    return false;
}
json::Object target(const std::string& profileId, const json::Object& profile) {
    const auto provider = profile.value("provider").toString();
    const bool proton = provider == "proton-drive";
    json::Object t{{"profileId", profileId}, {"label", profile.value("label").toString()}, {"provider", provider}, {"publicLink", true},
        {"warning", proton ? "Anyone with the link can download this package until the link expires in " + std::to_string(shareDays) + " days."
                           : std::string("On a public-read bucket, anyone with the link can download this package. Delete it from History when it is no longer needed.")}};
    if (proton) t["shareExpirationDays"] = shareDays;
    return t;
}
}
