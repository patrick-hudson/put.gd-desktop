#pragma once
// Diagnostics packages (docs/diagnostics.md, "Diagnostics package"): the request
// rules, the package redaction and the package contents, without any file system
// or session access, so the Linux-hosted tests check them. The resident's
// resident/Diagnostics.cpp gathers the inputs, writes the file and uploads it.
#include "core/Operation.h"
#include "core/Settings.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace putgd::diagnostics {
constexpr long long maximumBytes = 20LL * 1024 * 1024;
constexpr int keptPackages = 10, keptDays = 30, maximumOperations = 50;
constexpr long long maximumSinceMs = 365LL * 24 * 60 * 60 * 1000;
// A passing destination check is reused this long with the same settings revision.
constexpr long long checkReuseMs = 10LL * 60 * 1000;
// A Proton share link for a package expires after this many days.
constexpr int shareDays = 7;
inline constexpr const char* operationAction = "diagnostics";

struct Request {
    std::string operationId;
    long long sinceMs = 7LL * 24 * 60 * 60 * 1000;
    bool includeNames = false, preview = false, notify = false, upload = false, copyLink = false;
    std::string output, profileId;
};
Error parse(const json::Object& params, Request&);

// Replaces names, object keys, URLs and private paths with per-package tokens
// (one value, one token, throughout a package; never across packages), unless
// names are included. Known folders become placeholders in both modes. The
// log's hard redaction always applies again.
class Redactor {
public:
    // `key` is the package's random HMAC key; it is never stored.
    Redactor(bool includeNames, std::string key);
    // Windows paths compare case-insensitively and with either separator.
    void root(const std::string& path, const std::string& placeholder);
    void literal(const std::string& kind, const std::string& value);
    std::string token(const std::string& kind, const std::string& value) const;
    std::string text(const std::string&) const;
    json::Value json(const json::Value&, const std::string& key = {}) const;
    std::string paths(std::string) const;
    bool includeNames() const { return names_; }
private:
    std::string literals(const std::string&) const;
    std::string placeholders(std::string) const;
    bool names_;
    std::string key_;
    struct Root { std::string path, placeholder; };
    std::vector<Root> roots_;
    std::map<std::string, std::string> literals_;
    std::map<char, std::vector<std::pair<std::string, std::string>>> buckets_; // by first byte, longest first
};

struct LogFile { std::string name, bytes; };
struct Inputs {
    long long nowMs = 0;
    std::string key; // HMAC key for the redactor
    json::Object build, settings, diagnostics, environment, crashDumps;
    std::vector<std::pair<std::string, std::string>> roots; // path -> placeholder
    std::string home;
    // The one operation, or the failed/partial/interrupted ones in the window, newest first.
    std::vector<Operation> operations;
    // Completed diagnostics uploads in the window: their operations and log records stay out.
    std::set<std::string> uploadedPackages;
    std::vector<LogFile> logs; // log files, any order
    std::string defaultFolder, previewFolder;
    bool previewDefaultFolder = true;
    long long maximumBytes = diagnostics::maximumBytes; // smaller in tests
};
struct Package {
    std::vector<std::pair<std::string, std::string>> files; // every file but manifest.json
    json::Object manifest, scope;
    json::Array operationList;
    long long records = 0, eligibleRecords = 0, droppedRecords = 0;
    std::string zip; // empty for a preview
    json::Object preview;
};
// Where the scope starts: ten minutes before the operation was accepted, or
// `since` before now.
long long windowStart(const Request&, long long nowMs, const std::string& operationCreated);
Package build(const Inputs&, const Request&);
std::vector<std::string> fileNames(const Package&);

// putgd-diagnostics-<yyyyMMdd-HHmmss>Z[-N].zip
std::string baseName(long long nowMs);
bool isPackageName(const std::string&);
struct Saved { std::string name; long long modifiedMs = 0; };
// In put.gd's own default folder: the packages beyond the newest ten and any older than 30 days.
std::vector<std::string> pruned(std::vector<Saved>, long long nowMs);

// The destination a package goes to without profileId: the region capture's
// upload destination, else the first action that uploads. Empty if none.
std::string defaultUploadProfile(const Settings&);
// A credential or configuration error, as opposed to a passing network or service problem.
bool credentialOrConfigurationError(const std::string& code);
// The uploadTarget fields for a destination: label, provider, publicLink, warning, shareExpirationDays.
json::Object target(const std::string& profileId, const json::Object& profile);
}
