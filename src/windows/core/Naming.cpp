#include "core/Naming.h"
#include "common/Text.h"
#include "core/Operation.h"
#include "core/Settings.h"
#include <cstdio>
#include <map>

namespace putgd::naming {
namespace {
const std::vector<std::string> tokens{"name", "ext", "yyyy", "MM", "dd", "HH", "mm", "ss", "uuid"};
std::string two(int v) { char b[8]; std::snprintf(b, sizeof b, "%02d", v); return b; }
}
Error checkName(const std::string& name) {
    if (name.empty() || !text::validUtf8(name)) return {"filename_invalid", "A filename is empty or not valid UTF-8", {}};
    if (name == "." || name == ".." || text::containsChar(name, '/') || text::containsChar(name, '\\') || text::containsChar(name, '\0'))
        return {"filename_invalid", "A filename contains a path separator or is a relative directory", {}};
    for (const unsigned char c : name) if (c < 0x20 || c == 0x7F) return {"filename_invalid", "A filename contains control characters", {}};
    return {};
}
Error validateTemplate(const std::string& format) {
    if (auto error = checkName(format)) return error;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] == '}') return {"filename_template_invalid", "A filename template contains an unmatched closing brace", {}};
        if (format[i] != '{') continue;
        const auto end = format.find('}', i + 1);
        if (end == std::string::npos) return {"filename_template_invalid", "A filename template contains an unknown or unclosed token", {}};
        const auto token = format.substr(i + 1, end - i - 1);
        bool known = false; for (const auto& t : tokens) if (t == token) known = true;
        if (!known) return {"filename_template_invalid", "A filename template contains an unknown or unclosed token", {}};
        i = end;
    }
    return {};
}
std::string baseName(const std::string& path) {
    const auto pos = path.find_last_of("/\\");
    return pos == std::string::npos ? path : path.substr(pos + 1);
}
std::string suffix(const std::string& name) {
    const auto dot = name.rfind('.');
    if (dot == std::string::npos || dot == 0 || dot + 1 == name.size()) return {};
    return name.substr(dot + 1);
}
std::string boundedName(const std::string& name, int maximumBytes, const std::string& inserted) {
    const auto ext = suffix(name);
    std::string stem = ext.empty() ? name : name.substr(0, name.size() - ext.size() - 1);
    const std::string tail = inserted + (ext.empty() ? std::string() : "." + ext);
    const int room = maximumBytes - int(tail.size());
    if (int(stem.size()) > room) {
        // Cut at a UTF-8 boundary; keep at least one character of the stem.
        size_t cut = room > 1 ? size_t(room) : 1;
        while (cut > 1 && cut < stem.size() && (static_cast<unsigned char>(stem[cut]) & 0xC0) == 0x80) --cut;
        stem = stem.substr(0, cut);
    }
    return stem + tail;
}
FilenameResult renderTemplate(const std::string& format, const Artifact& artifact, const LocalTime& t, const std::string& unique) {
    if (auto error = validateTemplate(format)) return {{}, error};
    const auto filename = baseName(artifact.name);
    const auto extension = suffix(filename);
    const auto name = extension.empty() ? filename : filename.substr(0, filename.size() - extension.size() - 1);
    const std::map<std::string, std::string> values{{"name", name}, {"ext", extension}, {"yyyy", std::to_string(t.year)}, {"MM", two(t.month)},
        {"dd", two(t.day)}, {"HH", two(t.hour)}, {"mm", two(t.minute)}, {"ss", two(t.second)}, {"uuid", unique}};
    std::string output;
    for (size_t i = 0; i < format.size(); ++i) {
        if (format[i] != '{') { output += format[i]; continue; }
        const auto end = format.find('}', i + 1);
        output += values.at(format.substr(i + 1, end - i - 1));
        i = end;
    }
    if (auto error = checkName(output)) return {{}, error};
    return {output, {}};
}
FilenameResult acquiredName(const Artifact& artifact, const Settings& settings, const std::string& action, const LocalTime& acquired, const std::string& uuid) {
    std::string format;
    if (settings.namingMode == "generated") format = suffix(artifact.name).empty() ? "{uuid}" : "{uuid}.{ext}";
    else if (artifact.originalFile) {
        if (settings.namingMode == "original") { if (auto error = checkName(artifact.name)) return {{}, error}; return {artifact.name, {}}; }
        format = settings.fileTemplate;
    } else if (action == "record") format = settings.recordingTemplate;
    else if (artifact.mime == "image/png") format = action == "clipboard" ? settings.imageTemplate : settings.captureTemplate;
    else format = settings.textTemplate;
    auto result = renderTemplate(format, artifact, acquired, uuid.empty() ? artifact.id : uuid);
    if (!result.error) result.name = boundedName(result.name, 255);
    return result;
}
std::string candidate(const Artifact& artifact, const std::string& mode, const std::string& fresh, bool rename, int maximumBytes) {
    auto name = baseName(artifact.name);
    for (auto& c : name) if (c == '\0') c = '_';
    if (!artifact.namingPrepared && mode == "generated") { const auto ext = suffix(name); name = fresh + (ext.empty() ? std::string() : "." + ext); }
    return rename ? renamedCandidate(name, fresh, maximumBytes) : boundedName(name, maximumBytes);
}
std::string renamedCandidate(const std::string& name, const std::string& fresh, int maximumBytes) { return boundedName(name, maximumBytes, "-" + fresh); }
std::vector<NamedItem> batch(const std::vector<Artifact>& artifacts, const std::string& mode, const std::string& policy, const std::vector<std::string>& uuids, int maximumBytes) {
    std::vector<NamedItem> result;
    std::map<std::string, std::vector<size_t>> groups;
    size_t next = 0;
    auto fresh = [&] { return next < uuids.size() ? uuids[next++] : std::string("rename-") + std::to_string(next++); };
    for (const auto& artifact : artifacts) {
        auto name = candidate(artifact, mode, artifact.id, false, maximumBytes);
        const size_t index = result.size();
        result.push_back({name, "pending"});
        if (groups.count(name)) {
            if (policy == "overwrite") { result[index].status = "conflict"; for (const auto previous : groups[name]) result[previous].status = "conflict"; }
            else if (policy == "skip") result[index].status = "skipped";
            else {
                int attempts = 0;
                do { name = renamedCandidate(result[index].name, fresh(), maximumBytes); ++attempts; } while (groups.count(name) && attempts < 32);
                if (groups.count(name)) result[index].status = "conflict";
                result[index].name = name;
            }
        }
        groups[name].push_back(index);
    }
    return result;
}
std::string category(const std::string& mime) {
    if (text::startsWith(mime, "image/")) return "images";
    if (text::startsWith(mime, "video/")) return "videos";
    if (text::startsWith(mime, "text/")) return "text";
    return "other";
}
}
