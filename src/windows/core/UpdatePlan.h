#pragma once
// The portable rules of a Windows update install (#404, docs/updates.md): which
// builds install, which release file they fetch from where, and what the release's
// signed SHA256SUMS must say. The Win32 side is resident/UpdateInstall.cpp.
#include "common/Error.h"
#include <string>

namespace putgd::update {
inline constexpr long long maximumPackageBytes = 256LL * 1024 * 1024;
inline constexpr long long maximumSumsBytes = 1024 * 1024, maximumSignatureBytes = 16 * 1024;
inline constexpr const char* releaseDownloads = "https://github.com/patrick-hudson/put.gd-desktop/releases/download/";
inline constexpr const char* mirrorDownloads = "https://pkg.put.gd/releases/";
// "nightly" for YY.MDD.N-nightly, "stable" for YY.MDD.N, empty for every other
// build (development, dirty, pre-#373): only tagged builds install updates.
std::string releaseChannel(const std::string& version);
// The feed asset key this installation updates from: windows-msi or windows-zip.
std::string packageTarget(bool msi);
// An https URL, or http on a loopback address (the lab and tests), without query,
// fragment or user information; base gets a trailing slash. Empty when invalid.
std::string validBase(std::string base);
// base + tag + "/" + name for a release tag vYY.MDD.N[-nightly] and a plain file name.
Error releaseFile(const std::string& base, const std::string& tag, const std::string& name, std::string& url);
// The SHA-256 SHA256SUMS lists for name (text or binary mode), lowercase; empty when absent or listed twice.
std::string sumsLine(const std::string& sums, const std::string& name);
// The trusted comment the release signer puts on SHA256SUMS.minisig.
std::string trustedComment(const std::string& tag, const std::string& channel);
// The feed's asset for this installation, checked: its name, size and SHA-256.
Error releaseAsset(const json::Object& release, const std::string& target, json::Object& asset);
// Case-insensitive comparison of two Windows directories, ignoring a trailing backslash.
bool sameDirectory(std::string a, std::string b);
}
