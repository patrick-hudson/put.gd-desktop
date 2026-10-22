#pragma once
// The persistent log's platform-neutral parts (docs/diagnostics.md): record
// schema 1, file names, rotation size, retention selection and the bounded
// collection of helper stderr. The Win32 writer in common/Log.cpp does the IO.
#include "common/Json.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace putgd::logformat {
enum class Level { Debug, Info, Warning, Error, Fatal };
const char* levelName(Level);
std::optional<Level> parseLevel(std::string_view);
// Short Windows category names map onto the Linux ones (putgd.app, putgd.operation ...).
std::string category(std::string_view);

// UTC calendar time without the C library's time zone state.
std::string iso(long long msSinceEpoch);         // 2026-10-22T06:15:00.123Z
long long parseIso(std::string_view);             // -1 when not that form
std::string compactStamp(long long msSinceEpoch); // 20261022T061500123

struct Record {
    long long timeMs = 0;
    Level level = Level::Info;
    std::string category, process;
    long long pid = 0;
    std::string message;
    json::Object fields;
};
// One JSON line with its newline: redacted, message at most 4096 characters,
// operationId/itemId lifted out of fields, at most 16 KiB (fields dropped beyond).
std::string encode(const Record&);

constexpr long long defaultRotationBytes = 5LL * 1024 * 1024;
constexpr int maximumQueuedRecords = 4096;
// 5 MiB, or a quarter of the retention bytes when smaller, never under 64 KiB.
long long rotationLimit(long long retentionBytes);
// putgd-<yyyyMMddTHHmmsszzz>Z-<pid>-<sequence>.jsonl
std::string fileName(long long startMs, long long pid, int sequence);
bool isFileName(std::string_view);
struct FileEntry { std::string name; long long bytes = 0, modifiedMs = 0; };
// The files retention deletes: last written more than `days` ago, then the
// oldest until the rest fit `bytes`. `current` (the file being written) stays.
std::vector<std::string> expired(std::vector<FileEntry>, long long nowMs, int days, long long bytes, const std::string& current);

// A helper's stderr (docs/diagnostics.md, "Helpers"): helpers write encoded
// records to stderr and never open log files. Each line becomes a resident
// record under the helper's process name and pid: a helper record keeps its
// level, category, message and fields; any other text is a warning with
// stream "stderr". At most 64 KiB per helper generation and 2 KiB per line;
// crash-record lines (putgd-crash) are collected outside that bound.
class HelperOutput {
public:
    static constexpr long long generationBytes = 64 * 1024;
    static constexpr size_t lineBytes = 2048, crashBytes = 64 * 1024;
    using Sink = std::function<void(Record)>;
    using Clock = std::function<long long()>;
    HelperOutput(std::string process, long long pid, Sink, Clock);
    void append(std::string_view bytes);
    // End of the stream: the last partial line, the truncation summary and any crash record.
    void finish();
    const std::string& process() const { return process_; }
private:
    void line(std::string text);
    void emit(Level, const std::string& category, const std::string& message, json::Object fields);
    std::string process_, partial_, crash_;
    long long pid_ = 0, logged_ = 0, suppressed_ = 0;
    Sink sink_;
    Clock clock_;
};
// The process name for a child started as `executable mode...`: putgd-capture,
// putgd-ui, putgd-s3-worker, putgd-image-job, putgd-editor, putgd-update-check,
// or the executable's base name (putgd-proton-runtime).
std::string helperProcessName(const std::string& executable, const std::vector<std::string>& arguments);
}
