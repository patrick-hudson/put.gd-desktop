#pragma once
// Installing an update on Windows (#404, docs/updates.md). Two child modes of
// putgd.exe do the work so the resident never holds a download:
//
//   putgd --install-update CHANNEL OUTPUT  fetch the feed, the release's SHA256SUMS
//       and its minisign signature, download and verify this installation's package,
//       then either stage the MSI under %LOCALAPPDATA%\put.gd\updates or swap the
//       portable folder's files (the old ones move to .putgd-update-backup-VERSION).
//       Progress and the result go to OUTPUT as one JSON object.
//   putgd --apply-update MSI PID   a copy of putgd.exe outside the installation waits
//       for the resident PID to exit, runs msiexec /passive and starts put.gd again.
//
// The record of an install is %LOCALAPPDATA%\put.gd\update-install.json.
#include "common/Error.h"
#include <functional>
#include <string>
#include <vector>

namespace putgd::update {
struct Installation {
    bool msi = false;          // installed by the MSI (HKCU\Software\put.gd\Installer names this folder)
    std::string directory;     // the folder putgd.exe runs from
    std::string target;        // windows-msi or windows-zip
};
Installation installation();
std::string statePath();
json::Object readState();
bool writeState(const json::Object&);
// One GET of at most limit bytes; with a sink the body streams into it instead.
struct Transfer { unsigned long status = 0; long long bytes = 0; bool tooLarge = false; std::string failure; bool ok() const { return failure.empty() && !tooLarge && status == 200; } };
std::string httpGet(const std::string& url, long long limit, Transfer&, const std::function<bool(const char*, size_t)>& sink = {});
int installMain(const std::vector<std::string>& args); // --install-update CHANNEL OUTPUT
int applyMain(const std::vector<std::string>& args);   // --apply-update MSI PID
// Starts a process outside this one's job, so it outlives the resident.
bool startIndependent(const std::string& executable, const std::vector<std::string>& arguments);
// --after-update PID: a restarted resident waits for its predecessor to exit.
void waitForExit(unsigned long pid, unsigned long timeoutMs);
}
