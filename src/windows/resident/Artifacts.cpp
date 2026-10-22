#include "resident/Artifacts.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/Naming.h"
#include "core/SanitizedPng.h"
#include "resident/Helpers.h"
#include <algorithm>
#include <map>
#include <set>
#include <windows.h>

namespace putgd {
namespace {
constexpr long long reserveBytes = 512LL * 1024 * 1024;
std::string sniff(const std::string& path) {
    const auto head = files::read(path, 0); // size check only
    HANDLE h = CreateFileW(paths::wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    char b[16] = {}; DWORD got = 0; ReadFile(h, b, sizeof b, &got, nullptr); CloseHandle(h);
    const std::string_view s(b, got);
    if (s.size() >= 8 && s.substr(0, 8) == png::signature) return "image/png";
    if (s.size() >= 3 && (unsigned char)s[0] == 0xFF && (unsigned char)s[1] == 0xD8 && (unsigned char)s[2] == 0xFF) return "image/jpeg";
    if (s.size() >= 6 && (s.substr(0, 6) == "GIF87a" || s.substr(0, 6) == "GIF89a")) return "image/gif";
    if (s.size() >= 12 && s.substr(0, 4) == "RIFF" && s.substr(8, 4) == "WEBP") return "image/webp";
    if (s.size() >= 4 && s.substr(0, 4) == "%PDF") return "application/pdf";
    if (s.size() >= 4 && s.substr(0, 4) == "PK\x03\x04") return "application/zip";
    if (s.size() >= 12 && s.substr(4, 4) == "ftyp") return "video/mp4";
    if (s.size() >= 4 && s.substr(0, 4) == "\x1A\x45\xDF\xA3") return "video/webm";
    if (s.size() >= 2 && s.substr(0, 2) == "BM") return "image/bmp";
    return {};
}
}
std::string ArtifactStore::mimeFor(const std::string& path) {
    if (auto m = sniff(path); !m.empty()) return m;
    const auto ext = text::lower(naming::suffix(paths::fileName(path)));
    static const std::map<std::string, std::string> table{{"txt", "text/plain"}, {"md", "text/markdown"}, {"json", "application/json"}, {"csv", "text/csv"}, {"html", "text/html"}, {"htm", "text/html"},
        {"xml", "application/xml"}, {"log", "text/plain"}, {"svg", "image/svg+xml"}, {"mp3", "audio/mpeg"}, {"wav", "audio/wav"}, {"mov", "video/quicktime"}, {"mkv", "video/x-matroska"}, {"avi", "video/x-msvideo"}};
    const auto it = table.find(ext);
    return it == table.end() ? "application/octet-stream" : it->second;
}
ArtifactStore::ArtifactStore(std::string dataRoot) : dataRoot_(std::move(dataRoot)), artifacts_(paths::join(dataRoot_, "artifacts")), previews_(paths::join(dataRoot_, "previews")) {
    files::makePrivateDirectory(artifacts_); files::makePrivateDirectory(previews_);
}
Acquisition ArtifactStore::acquire(const std::string& op, const Payload& p, const Settings& settings, const std::string& action, const std::string& created, long long maximumObjectBytes) {
    Acquisition result;
    const auto& limits = settings.limits;
    const std::string staging = paths::join(artifacts_, "." + op + "-" + uuid::create()), target = paths::join(artifacts_, op);
    if (!files::makePrivateDirectory(staging)) { result.error = {"artifact_io", "Cannot create private artifact storage", {}}; return result; }
    auto fail = [&](std::string code, std::string message, int index) { result.error = {std::move(code), std::move(message), {{"itemIndex", index}}}; result.artifacts.clear(); files::removeDirectoryRecursively(staging); return result; };
    const int count = p.kind == "files" ? int(p.files.size()) : 1;
    if (count < 1 || count > limits.maxFiles) return fail("input_excessive", "Invalid batch size", 0);
    long long total = 0; const long long existing = bytes();
    long long required = p.kind == "text" || p.kind == "png" || p.kind == "image" ? static_cast<long long>(p.bytes.size()) : p.kind == "capture" ? files::size(p.imagePath) : 0;
    if (p.kind == "files") for (int i = 0; i < count; ++i) {
        const auto size = files::size(p.files[i]);
        if (size < 0) continue;
        if (maximumObjectBytes > 0 && size > maximumObjectBytes) return fail("destination_object_too_large", text::format("%1 is %2; this destination accepts files of at most %3.", {paths::fileName(p.files[i]).substr(0, 255), text::byteSize(size), text::byteSize(maximumObjectBytes)}), i);
        required += size;
    }
    const auto free = files::freeBytes(artifacts_);
    if (free >= 0 && required > free - reserveBytes) return fail("disk_space_insufficient", text::format("Not enough free disk space to import: it needs %1, and %2 can be used in put.gd's data folder.", {text::byteSize(required), text::byteSize(std::max(0LL, free - reserveBytes))}), 0);
    const auto time = files::localFields(created);
    for (int i = 0; i < count; ++i) {
        Artifact a; a.id = uuid::create(); a.path = paths::join(staging, a.id);
        std::string source; std::string encoded; bool fromFile = false;
        if (p.kind == "files") {
            if (!files::isRegularFile(p.files[i])) return fail("source_unavailable", "Source is not an accessible regular file", i);
            source = p.files[i]; fromFile = true; a.originalFile = true; a.name = paths::fileName(source);
        } else if (p.kind == "text") {
            encoded = p.bytes; a.mime = "text/plain"; a.name = "text.txt";
            if (encoded.empty() || static_cast<long long>(encoded.size()) > limits.clipboardTextBytes) return fail("input_excessive", "Invalid text size", i);
        } else if (p.kind == "png") {
            if (p.bytes.empty()) return fail("source_empty", "Empty source", i);
            encoded = p.bytes; a.mime = "image/png"; a.name = "image.png";
        } else if (p.kind == "image") {
            // Normalize in the image job: the resident never decodes.
            if (p.bytes.empty()) return fail("source_empty", "Empty source", i);
            if (static_cast<long long>(p.bytes.size()) > limits.imageBytes) return fail("input_excessive", "Invalid image size", i);
            const auto raw = paths::join(staging, a.id + ".src");
            if (files::writeAtomically(raw, p.bytes)) return fail("artifact_io", "Cannot stage the clipboard image", i);
            const bool verbatim = p.imageFormat == "png" && !png::inspect(p.bytes);
            if (!helpers::prepareOutputFile(a.path)) return fail("artifact_io", "Cannot create the artifact file", i);
            const auto run = helpers::runSynchronous("image-job", {"--image-job"}, json::Object{{"input", json::Object{{"path", raw}, {"kind", p.imageFormat}}}, {"output", json::Object{{"path", a.path}, {"kind", "png"}}},
                {"limits", json::Object{{"imagePixels", double(limits.imagePixels)}, {"imageBytes", double(limits.imageBytes)}}}, {"reencode", !verbatim}}, 10000, 120000, {});
            files::remove(raw);
            if (run.error) return fail(run.error.code == "helper_unavailable" ? "image_job_io" : run.error.code, run.error.message, i);
            source = a.path; fromFile = true; a.mime = "image/png"; a.name = "image.png";
        } else if (p.kind == "capture") {
            if (!files::isRegularFile(p.imagePath)) return fail("source_unavailable", "The capture file is missing", i);
            if (!files::rename(p.imagePath, a.path, false)) return fail("artifact_io", "Cannot move the capture into artifact storage", i);
            source = a.path; fromFile = true; a.mime = "image/png"; a.name = "capture.png";
        } else return fail("source_unsupported", "Unsupported source", i);
        const auto named = naming::acquiredName(a, settings, action, LocalTime{time.year, time.month, time.day, time.hour, time.minute, time.second}, a.id);
        if (named.error) return fail(named.error.code, named.error.message, i);
        a.name = named.name; a.namingPrepared = true;
        const long long expected = fromFile ? files::size(source) : static_cast<long long>(encoded.size());
        if (expected <= 0 && !fromFile) return fail("source_empty", "Empty source", i);
        if (maximumObjectBytes > 0 && expected > maximumObjectBytes) return fail("destination_object_too_large", text::format("This item is %1; this destination accepts files of at most %2.", {text::byteSize(expected), text::byteSize(maximumObjectBytes)}), i);
        if (expected > limits.maxFileBytes) return fail("input_excessive", text::format("Artifact budget exceeded: this file is %1 and the Largest file limit is %2", {text::byteSize(expected), text::byteSize(limits.maxFileBytes)}), i);
        if (expected > limits.maxBatchBytes - total) return fail("input_excessive", text::format("Artifact budget exceeded: this batch is over the Largest batch limit of %1", {text::byteSize(limits.maxBatchBytes)}), i);
        if (expected > limits.pendingArtifactBytes - existing - total) return fail("input_excessive", text::format("Artifact budget exceeded: waiting uploads would pass the Waiting uploads limit of %1", {text::byteSize(limits.pendingArtifactBytes)}), i);
        if (fromFile && source != a.path) { if (auto e = files::copyFile(source, a.path)) return fail("artifact_io", "Artifact write failed", i); }
        else if (!fromFile) { if (auto e = files::writeAtomically(a.path, encoded)) return fail("artifact_io", "Artifact write failed", i); }
        long long size = 0;
        a.sha256 = files::sha256(a.path, &size);
        if (a.sha256.empty()) return fail("artifact_io", "Artifact hashing failed", i);
        a.bytes = size;
        if (a.bytes == 0) return fail("source_empty", "Empty source", i);
        files::restrictToOwner(a.path);
        if (p.kind == "files") a.mime = mimeFor(a.path);
        if (p.kind == "capture" || p.kind == "image") { if (auto e = png::inspect(files::read(a.path, size_t(limits.imageBytes)).value_or(std::string()))) return fail("encoding_failed", "The image is not a sanitized PNG", i); }
        a.category = naming::category(a.mime);
        total += a.bytes;
        result.artifacts.push_back(a);
    }
    json::Array entries;
    for (const auto& artifact : result.artifacts) { auto stored = artifact; stored.path = paths::join(target, artifact.id); entries.push_back(stored.toJson()); }
    const auto manifest = json::serialize(json::Object{{"schemaVersion", 1}, {"operationId", op}, {"artifacts", entries}});
    if (files::writeAtomically(paths::join(staging, "_manifest.json"), manifest)) return fail("artifact_io", "Cannot commit batch recovery metadata", 0);
    if (files::exists(target)) files::removeDirectoryRecursively(target);
    if (!files::rename(staging, target, false)) return fail("artifact_io", "Cannot commit acquired batch", 0);
    for (auto& a : result.artifacts) a.path = paths::join(target, a.id);
    return result;
}
Error ArtifactStore::verify(const Artifact& a) const {
    long long size = 0;
    const auto digest = files::sha256(a.path, &size);
    if (digest.empty()) return {"artifact_missing", "Retained artifact is missing", {}};
    if (size != a.bytes || digest != a.sha256) return {"artifact_changed", "Retained artifact no longer matches its recorded hash", {}};
    return {};
}
Acquisition ArtifactStore::recover(const std::string& op) const {
    Acquisition result;
    const auto manifest = files::read(paths::join(paths::join(artifacts_, op), "_manifest.json"), 16 * 1024 * 1024);
    if (!manifest) { result.error = {"artifact_missing", "No retained artifacts for this operation", {}}; return result; }
    const auto document = json::parseObject(*manifest).value_or(json::Object{});
    std::set<std::string> ids;
    for (const auto& entry : document.value("artifacts").toArray()) {
        auto artifact = Artifact::fromJson(entry.toObject());
        if (!uuid::valid(artifact.id) || ids.count(artifact.id)) { result.error = {"artifact_recovery_failed", "Invalid retained artifact identity", {}}; result.artifacts.clear(); return result; }
        ids.insert(artifact.id);
        artifact.path = paths::join(paths::join(artifacts_, op), artifact.id);
        if (auto e = verify(artifact)) { result.error = e; result.artifacts.clear(); return result; }
        result.artifacts.push_back(artifact);
    }
    return result;
}
void ArtifactStore::discardIncomplete() {
    for (const auto& name : files::list(artifacts_)) if (!name.empty() && name[0] == '.') files::removeDirectoryRecursively(paths::join(artifacts_, name));
}
Error ArtifactStore::remove(const std::string& op) {
    const auto directory = paths::join(artifacts_, op);
    if (files::exists(directory) && !files::removeDirectoryRecursively(directory)) return {"artifact_io", "Cannot remove retained artifacts", {}};
    return {};
}
std::string ArtifactStore::preview(const Artifact& a, long long budget, long long maximumPixels) {
    if (budget <= 0 || !text::startsWith(a.mime, "image/") || a.mime == "image/svg+xml") return {};
    const auto path = paths::join(previews_, a.id + ".png");
    if (files::isRegularFile(path)) return path;
    if (!helpers::prepareOutputFile(path)) return {};
    const auto run = helpers::runSynchronous("image-job", {"--image-job"}, json::Object{{"input", json::Object{{"path", a.path}, {"kind", a.mime == "image/png" ? "png" : "file"}}},
        {"preview", json::Object{{"path", path}, {"edge", 256}}}, {"limits", json::Object{{"imagePixels", double(maximumPixels)}, {"imageBytes", double(a.bytes + 1)}}}, {"previewOnly", true}, {"reencode", false}}, 10000, 60000, {});
    if (run.error || files::size(path) <= 0) { files::remove(path); log::debug("artifacts", "Preview not generated", {{"code", run.error.code}}); return {}; }
    // Evict the oldest previews beyond the budget.
    struct Entry { std::string path; long long size; unsigned long long written; };
    std::vector<Entry> entries; long long total = 0;
    for (const auto& name : files::list(previews_)) {
        const auto full = paths::join(previews_, name);
        WIN32_FILE_ATTRIBUTE_DATA data; if (!GetFileAttributesExW(paths::wide(full).c_str(), GetFileExInfoStandard, &data)) continue;
        const long long size = (static_cast<long long>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        entries.push_back({full, size, (static_cast<unsigned long long>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime});
        total += size;
    }
    std::sort(entries.begin(), entries.end(), [](const Entry& x, const Entry& y) { return x.written < y.written; });
    for (const auto& entry : entries) { if (total <= budget) break; if (entry.path == path) continue; files::remove(entry.path); total -= entry.size; }
    return path;
}
void ArtifactStore::clearPreviews() { for (const auto& name : files::list(previews_)) files::remove(paths::join(previews_, name)); }
long long ArtifactStore::bytes() const {
    long long total = 0;
    for (const auto& op : files::list(artifacts_)) {
        const auto directory = paths::join(artifacts_, op);
        if (!files::isDirectory(directory)) continue;
        for (const auto& item : files::list(directory)) { const auto size = files::size(paths::join(directory, item)); if (size > 0) total += size; }
    }
    return total;
}
}
