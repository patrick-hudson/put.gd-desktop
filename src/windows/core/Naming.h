#pragma once
// Upload and archive filenames: templates, generated names, bounded UTF-8
// lengths and batch collision handling (docs/configuration.md, Filename templates).
#include "common/Error.h"
#include <string>
#include <vector>

namespace putgd {
struct Artifact;
struct Settings;
struct LocalTime { int year = 1970, month = 1, day = 1, hour = 0, minute = 0, second = 0; };
struct NamedItem { std::string name, status = "pending"; };
struct FilenameResult { std::string name; Error error; };
namespace naming {
Error validateTemplate(const std::string&);
Error checkName(const std::string&);
std::string baseName(const std::string& path);   // last path component
std::string suffix(const std::string& name);     // extension without the dot
// Bytes-bounded truncation of the stem that keeps the extension and any suffix.
std::string boundedName(const std::string& name, int maximumBytes, const std::string& inserted = {});
FilenameResult renderTemplate(const std::string& format, const Artifact&, const LocalTime&, const std::string& unique);
FilenameResult acquiredName(const Artifact&, const Settings&, const std::string& action, const LocalTime&, const std::string& uuidForGenerated);
std::string candidate(const Artifact&, const std::string& mode, const std::string& freshUuid, bool rename = false, int maximumBytes = 255);
std::string renamedCandidate(const std::string& name, const std::string& freshUuid, int maximumBytes = 255);
// Chooses names for a batch; `uuids` supplies fresh identities for renames.
std::vector<NamedItem> batch(const std::vector<Artifact>&, const std::string& mode, const std::string& policy, const std::vector<std::string>& uuids, int maximumBytes = 255);
std::string category(const std::string& mime); // images, videos, text, other
}
}
