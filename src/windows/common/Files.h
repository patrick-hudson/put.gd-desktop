#pragma once
// Owner-only files and directories, atomic replacement, hashing and bounded reads.
#include "common/Error.h"
#include <optional>
#include <string>
#include <vector>

namespace putgd::files {
bool exists(const std::string& path);
bool isDirectory(const std::string& path);
bool isRegularFile(const std::string& path);
long long size(const std::string& path); // -1 when missing
// Creates every missing component; existing directories are left alone.
bool makeDirectories(const std::string& path);
// A directory readable and writable only by the current user (and SYSTEM/Administrators).
bool makePrivateDirectory(const std::string& path);
bool restrictToOwner(const std::string& path);
std::optional<std::string> read(const std::string& path, size_t maximumBytes);
// Writes to a sibling temporary file, flushes, then renames over `path`.
Error writeAtomically(const std::string& path, const std::string& bytes, bool ownerOnly = true);
bool remove(const std::string& path);
bool removeDirectoryRecursively(const std::string& path);
bool rename(const std::string& from, const std::string& to, bool replace);
// Streams the file through SHA-256; empty when unreadable.
std::string sha256(const std::string& path, long long* bytes = nullptr);
std::vector<std::string> list(const std::string& directory); // names, not paths
std::string temporaryName(const std::string& directory, const std::string& prefix);
struct FileIdentity { unsigned long long volume = 0, index = 0; bool valid = false; bool operator==(const FileIdentity&) const = default; };
FileIdentity identity(const std::string& path);
long long freeBytes(const std::string& path);
// Copies in 1 MiB chunks to a temporary name in the target directory, then renames.
Error copyFile(const std::string& from, const std::string& to);
std::string nowIsoUtc();      // 2026-10-22T06:15:00.123Z
std::string isoUtcFromFileTime(unsigned long long fileTime);
// Local calendar fields for naming templates, from an ISO UTC timestamp (or now).
struct LocalFields { int year, month, day, hour, minute, second; };
LocalFields localFields(const std::string& isoUtc);
long long parseIsoUtcMs(const std::string& isoUtc); // ms since epoch; -1 when invalid
std::string isoUtcFromMs(long long ms);
}
