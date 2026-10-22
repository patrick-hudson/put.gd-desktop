#pragma once
// The persistent log (docs/diagnostics.md): JSON Lines records of schema 1
// (core/LogFormat.h) under <dataRoot>\logs\putgd-<UTC start>Z-<pid>-<n>.jsonl,
// redacted when written, rotated at 5 MiB (or a quarter of the retention bytes)
// and pruned by age and size. Only the resident opens log files: helpers pass an
// empty directory, write their records to stderr, and the resident records them
// under the helper's name (common/Process.cpp). Records are appended on the
// caller's thread through one handle kept open, so the idle resident has no
// writer thread; a failed write is dropped and counted, never retried, and
// never fails the caller. Never log payloads, credentials or clipboard text.
#include "common/Json.h"
#include "core/LogFormat.h"
#include <string>

namespace putgd::log {
enum class Level { Debug, Info, Warning, Error };
void initialize(const std::string& logDirectory, const std::string& processName, int retentionDays = 14, long long retentionBytes = 52428800LL);
// Retention from the settings; runs cleanup.
void setRetention(int days, long long bytes);
// `expiresAt` is when debug logging turns itself off (ISO UTC), or empty.
void setDebugEnabled(bool, const std::string& expiresAt = {});
bool debugEnabled();
void write(Level, const std::string& category, const std::string& message, const json::Object& fields = {});
// A record another process wrote (a helper's stderr), kept under its process and pid.
void writeRecord(logformat::Record);
inline void debug(const std::string& c, const std::string& m, const json::Object& f = {}) { write(Level::Debug, c, m, f); }
inline void info(const std::string& c, const std::string& m, const json::Object& f = {}) { write(Level::Info, c, m, f); }
inline void warning(const std::string& c, const std::string& m, const json::Object& f = {}) { write(Level::Warning, c, m, f); }
inline void error(const std::string& c, const std::string& m, const json::Object& f = {}) { write(Level::Error, c, m, f); }
// Retention: files last written more than `days` ago, then the oldest beyond `bytes`.
void cleanup();
// logs.clear: deletes every log file, starts a new one with a "Logs cleared"
// record and returns {removedFiles, removedBytes}.
json::Object clearAll();
// diagnostics.logging: available, directory, files, bytes, oldestRecord,
// degraded, degradedReason, droppedRecords, retention, rotationBytes,
// debugLogging, debugLoggingExpiresAt. From the writer's bookkeeping only.
json::Object status();
std::string directory();
std::string currentFile();
// Closes the file (resident shutdown); later records go nowhere.
void close();
long long nowMs();
}
