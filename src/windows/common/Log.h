#pragma once
// Redacted structured logging: stderr plus a persistent log file under the data
// root (<dataRoot>\logs\putgd-<yyyymmdd>.log). Never log payloads, URLs with
// credentials, secrets or clipboard text.
#include "common/Json.h"
#include <string>

namespace putgd::log {
enum class Level { Debug, Info, Warning, Error };
void initialize(const std::string& logDirectory, const std::string& processName);
void setDebugEnabled(bool);
bool debugEnabled();
void write(Level, const std::string& category, const std::string& message, const json::Object& fields = {});
inline void debug(const std::string& c, const std::string& m, const json::Object& f = {}) { write(Level::Debug, c, m, f); }
inline void info(const std::string& c, const std::string& m, const json::Object& f = {}) { write(Level::Info, c, m, f); }
inline void warning(const std::string& c, const std::string& m, const json::Object& f = {}) { write(Level::Warning, c, m, f); }
inline void error(const std::string& c, const std::string& m, const json::Object& f = {}) { write(Level::Error, c, m, f); }
// Removes log files older than `days` or beyond `bytes` in total, oldest first.
void prune(const std::string& logDirectory, int days, long long bytes);
// Deletes every log file and starts a new one (logs.clear).
void clearAll(const std::string& logDirectory);
std::string currentFile();
}
