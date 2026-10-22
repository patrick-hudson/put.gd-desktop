#include "resident/UpdateInstall.h"
#include "common/BuildInfo.h"
#include "common/Files.h"
#include "common/Log.h"
#include "common/Minisign.h"
#include "common/Paths.h"
#include "common/Process.h"
#include "common/Sha256.h"
#include "common/Text.h"
#include "common/Uuid.h"
#include "core/UpdatePlan.h"
#include "resident/UpdateService.h"
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <windows.h>
#include <winhttp.h>

// The release signer's minisign keys, compiled in from packaging/linux/repo/keys/.
#ifndef PUTGD_MINISIGN_NIGHTLY_KEY
#define PUTGD_MINISIGN_NIGHTLY_KEY ""
#endif
#ifndef PUTGD_MINISIGN_STABLE_KEY
#define PUTGD_MINISIGN_STABLE_KEY ""
#endif

namespace putgd::update {
namespace {
// What a portable folder holds; the MSI installs the same files.
const std::vector<std::string> portableFiles = {"putgd.exe", "putgd-cli.exe", "build-info.json", "LICENSE.txt"};
// The Proton runtime folder (#408) moves as one entry: the new putgd.exe runs only the
// helper its build names, and a package without it takes the old one aside too.
const std::string protonFolder = "proton";
std::string environment(const wchar_t* name, const std::string& fallback) {
    wchar_t buffer[2048]; const DWORD n = GetEnvironmentVariableW(name, buffer, 2048);
    return n > 0 && n < 2048 ? paths::narrow(buffer) : fallback;
}
std::string registryString(const wchar_t* key, const wchar_t* name) {
    wchar_t buffer[1024]; DWORD size = sizeof buffer;
    if (RegGetValueW(HKEY_CURRENT_USER, key, name, RRF_RT_REG_SZ, nullptr, buffer, &size) != ERROR_SUCCESS) return {};
    return paths::narrow(buffer);
}
std::string systemProgram(const wchar_t* name) {
    wchar_t directory[MAX_PATH]; const UINT n = GetSystemDirectoryW(directory, MAX_PATH);
    return n > 0 && n < MAX_PATH ? paths::join(paths::narrow(directory), paths::narrow(name)) : std::string();
}
// Runs a program to completion with no window; -1 when it can't start or runs too long.
long run(const std::string& program, const std::vector<std::string>& arguments, DWORD timeoutMs) {
    std::string command = process::quoteArgument(program);
    for (const auto& a : arguments) command += " " + process::quoteArgument(a);
    auto commandW = paths::wide(command);
    STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(paths::wide(program).c_str(), commandW.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr, nullptr, &si, &pi)) return -1;
    CloseHandle(pi.hThread);
    DWORD code = DWORD(-1);
    if (WaitForSingleObject(pi.hProcess, timeoutMs) == WAIT_OBJECT_0) GetExitCodeProcess(pi.hProcess, &code);
    else TerminateProcess(pi.hProcess, 1);
    CloseHandle(pi.hProcess);
    return long(code);
}
// The first bytes of a file (files::read refuses files over its limit).
std::string head(const std::string& path, DWORD size) {
    HANDLE h = CreateFileW(paths::wide(path).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::string bytes(size, '\0'); DWORD got = 0;
    if (!ReadFile(h, bytes.data(), size, &got, nullptr)) got = 0;
    CloseHandle(h);
    bytes.resize(got);
    return bytes;
}
json::Object transferDetails(const std::string& url, const Transfer& t) {
    return {{"url", url}, {"httpStatus", t.status ? json::Value(double(t.status)) : json::Value()}, {"network", t.failure.empty() ? json::Value() : json::Value(t.failure)}, {"tooLarge", t.tooLarge}};
}
// The new build's manifest and executables must be this release's (as the packager checks them).
Error checkPortable(const std::string& folder, const std::string& version) {
    const auto manifest = json::parseObject(files::read(paths::join(folder, "build-info.json"), 64 * 1024).value_or(std::string()));
    if (!manifest || manifest->value("version") != version.c_str() || manifest->value("dirty").toBool(true))
        return {"update_verification_failed", "The package's build manifest isn't this release's; nothing was changed", {{"check", "manifest"}}};
    const auto commit = manifest->value("gitCommit").toString();
    for (const auto& name : portableFiles) {
        if (!files::isRegularFile(paths::join(folder, name))) return {"update_verification_failed", "The package is missing " + name + "; nothing was changed", {{"check", "contents"}}};
        if (!text::endsWith(name, ".exe")) continue;
        const auto bytes = files::read(paths::join(folder, name), 64 * 1024 * 1024).value_or(std::string());
        if (bytes.compare(0, 2, "MZ") != 0 || !text::contains(bytes, version) || commit.size() != 40 || !text::contains(bytes, commit))
            return {"update_verification_failed", name + " in the package isn't this release's build; nothing was changed", {{"check", "identity"}}};
    }
    return {};
}
// Moves the new files over the running installation; the old ones go to backup.
// Windows lets a running executable be renamed, not replaced.
Error swapPortable(const std::string& from, const std::string& directory, const std::string& backup) {
    files::removeDirectoryRecursively(backup);
    if (!files::makeDirectories(backup)) return {"update_image_not_writable", "Cannot create the backup folder beside putgd.exe", {{"directory", directory}}};
    std::vector<std::string> saved, placed;
    auto rollback = [&] {
        for (const auto& name : placed) {
            const auto path = paths::join(directory, name);
            if (files::isDirectory(path)) files::removeDirectoryRecursively(path); else files::remove(path);
        }
        for (const auto& name : saved) files::rename(paths::join(backup, name), paths::join(directory, name), false);
    };
    auto entries = portableFiles;
    entries.push_back(protonFolder);
    for (const auto& name : entries) {
        const auto current = paths::join(directory, name), incoming = paths::join(from, name);
        if (name == protonFolder && !files::exists(current) && !files::exists(incoming)) continue;
        // A running helper keeps its folder from moving; the update then changes nothing.
        if (files::exists(current)) {
            if (!files::rename(current, paths::join(backup, name), false)) { rollback(); return {"update_install_failed", "Cannot move " + name + " aside; nothing was changed", {{"error", double(GetLastError())}}}; }
            saved.push_back(name);
        }
        if (name == protonFolder && !files::isDirectory(incoming)) continue;
        if (!files::rename(incoming, current, false)) { rollback(); return {"update_install_failed", "Cannot put the new " + name + " in place; nothing was changed", {{"error", double(GetLastError())}}}; }
        placed.push_back(name);
    }
    return {};
}
}

Installation installation() {
    Installation result;
    result.directory = paths::executableDirectory();
    result.msi = update::sameDirectory(registryString(L"Software\\put.gd\\Installer", L"InstallLocation"), result.directory);
    result.target = packageTarget(result.msi);
    return result;
}
std::string statePath() { return paths::join(paths::dataRoot(), "update-install.json"); }
json::Object readState() { return json::parseObject(files::read(statePath(), 256 * 1024).value_or(std::string())).value_or(json::Object{}); }
bool writeState(const json::Object& state) { return !files::writeAtomically(statePath(), json::serialize(state) + "\n"); }

std::string httpGet(const std::string& url, long long limit, Transfer& transfer, const std::function<bool(const char*, size_t)>& sink) {
    const auto parsed = text::parseUrl(url);
    if (!parsed.valid) { transfer.failure = "invalid URL"; return {}; }
    HINTERNET session = WinHttpOpen(L"put.gd-update/1", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) session = WinHttpOpen(L"put.gd-update/1", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) { transfer.failure = "WinHttpOpen failed"; return {}; }
    // No bytes for a minute ends a transfer, as on Linux.
    WinHttpSetTimeouts(session, 15000, 15000, 60000, 60000);
    const bool secure = parsed.scheme == "https";
    HINTERNET connection = WinHttpConnect(session, paths::wide(parsed.host).c_str(), INTERNET_PORT(parsed.port >= 0 ? parsed.port : (secure ? 443 : 80)), 0);
    std::string body;
    if (connection) {
        const auto path = (parsed.path.empty() ? std::string("/") : parsed.path) + (parsed.hasQuery ? "?" + parsed.query : "");
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", paths::wide(path).c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, secure ? WINHTTP_FLAG_SECURE : 0);
        // GitHub's release downloads redirect to its asset host; never from https to http.
        DWORD policy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
        if (request) WinHttpSetOption(request, WINHTTP_OPTION_REDIRECT_POLICY, &policy, sizeof policy);
        DWORD noCookies = WINHTTP_DISABLE_COOKIES;
        if (request) WinHttpSetOption(request, WINHTTP_OPTION_DISABLE_FEATURE, &noCookies, sizeof noCookies);
        if (request && WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) && WinHttpReceiveResponse(request, nullptr)) {
            DWORD size = sizeof transfer.status;
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &transfer.status, &size, WINHTTP_NO_HEADER_INDEX);
            std::string chunk(64 * 1024, '\0'); DWORD got = 0;
            while (transfer.status == 200) {
                if (!WinHttpReadData(request, chunk.data(), DWORD(chunk.size()), &got)) { transfer.failure = "read failed (" + text::toString((long long)GetLastError()) + ")"; break; }
                if (got == 0) break;
                transfer.bytes += got;
                if (transfer.bytes > limit) { transfer.tooLarge = true; break; }
                if (sink) { if (!sink(chunk.data(), got)) { transfer.failure = "Cannot write the download"; break; } }
                else body.append(chunk.data(), got);
            }
        } else transfer.failure = "request failed (" + text::toString((long long)GetLastError()) + ")";
        if (request) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
    } else transfer.failure = "connect failed";
    WinHttpCloseHandle(session);
    return body;
}

int installMain(const std::vector<std::string>& args) {
    if (args.size() != 3) return 2;
    const auto& channel = args[1]; const auto& output = args[2];
    auto lastProgress = std::chrono::steady_clock::time_point();
    std::string lastPhase;
    auto progress = [&](const std::string& phase, long long bytes, long long total) {
        const auto now = std::chrono::steady_clock::now();
        if (phase == lastPhase && now - lastProgress < std::chrono::milliseconds(300) && bytes < total) return;
        lastPhase = phase; lastProgress = now;
        files::writeAtomically(output, json::serialize(json::Object{{"progress", json::Object{{"phase", phase}, {"bytes", bytes}, {"total", total}}}}) + "\n");
    };
    auto finish = [&](const Error& error, const json::Object& result) {
        files::writeAtomically(output, json::serialize(error ? json::Object{{"ok", false}, {"error", error.toJson()}} : json::Object{{"ok", true}, {"result", result}}) + "\n");
        return error ? 1 : 0;
    };
    const auto buildChannel = releaseChannel(build::version);
    if (buildChannel.empty() || std::string(build::dirty) == "true")
        return finish({"update_install_unavailable", "Only release builds install updates", {{"reason", "development_build"}}}, {});
    if (channel != buildChannel)
        return finish({"update_install_unavailable", "Windows builds install updates on their own channel only", {{"reason", "switch_unsupported"}, {"channel", channel}}}, {});
    const auto here = installation();

    progress("checking", 0, 0);
    const auto feedUrl = validBase(environment(L"PUTGD_UPDATE_FEED_URL", "https://app.put.gd/update/v1/"));
    if (feedUrl.empty()) return finish({"update_feed_unavailable", "PUTGD_UPDATE_FEED_URL must be an https URL, or http on a loopback address", {}}, {});
    Transfer feedTransfer;
    const auto body = httpGet(feedUrl + channel + ".json", 256 * 1024, feedTransfer);
    if (!feedTransfer.ok()) return finish({feedTransfer.tooLarge ? "update_feed_invalid" : "update_feed_unavailable", "The update feed could not be fetched", transferDetails(feedUrl + channel + ".json", feedTransfer)}, {});
    const auto feed = json::parseObject(body);
    if (!feed || !json::integer(feed->value("schema"), 1, 1) || feed->value("channel") != channel.c_str()) return finish({"update_feed_invalid", "The update feed is not a valid version-1 document", {}}, {});
    if (!feed->value("release").isObject()) return finish({"update_no_release", "There's no " + channel + " release yet", {{"channel", channel}}}, {});
    const auto release = feed->value("release").toObject();
    const auto version = release.value("version").toString(), tag = release.value("tag").toString();
    if (releaseChannel(version) != channel || tag != "v" + version) return finish({"update_feed_invalid", "The feed names a release outside its channel", {}}, {});
    if (UpdateService::compareVersions(version, build::version) <= 0)
        return finish({"update_not_available", "This is the newest version on the channel", {{"current", build::version}, {"latest", version}}}, {});
    json::Object asset;
    if (auto e = releaseAsset(release, here.target, asset)) return finish(e, {});
    const auto name = asset.value("name").toString();
    const long long size = asset.value("size").toInteger();

    // The release's own list, over TLS from the release, signed by the channel's key.
    progress("verifying", 0, 0);
    const auto releaseBase = environment(L"PUTGD_UPDATE_RELEASE_URL", releaseDownloads), mirrorBase = environment(L"PUTGD_UPDATE_MIRROR_URL", mirrorDownloads);
    std::string sumsUrl, signatureUrl;
    if (auto e = releaseFile(releaseBase, tag, "SHA256SUMS", sumsUrl)) return finish(e, {});
    if (auto e = releaseFile(mirrorBase, tag, "SHA256SUMS.minisig", signatureUrl)) return finish(e, {});
    Transfer sumsTransfer;
    const auto sums = httpGet(sumsUrl, maximumSumsBytes, sumsTransfer);
    if (!sumsTransfer.ok()) return finish({"update_sums_unavailable", "Cannot read the release's SHA256SUMS", transferDetails(sumsUrl, sumsTransfer)}, {});
    Transfer signatureTransfer;
    const auto signature = httpGet(signatureUrl, maximumSignatureBytes, signatureTransfer);
    if (!signatureTransfer.ok()) {
        auto details = transferDetails(signatureUrl, signatureTransfer); details["check"] = "missing";
        return finish({"update_signature_invalid", "The release's SHA256SUMS has no signature; nothing was changed", details}, {});
    }
    std::string trusted;
    if (auto failed = minisign::verify(sums, signature, channel == "nightly" ? PUTGD_MINISIGN_NIGHTLY_KEY : PUTGD_MINISIGN_STABLE_KEY, &trusted)) {
        failed.message = "The release's SHA256SUMS signature isn't valid (" + text::lower(failed.message) + "); nothing was changed";
        return finish(failed, {});
    }
    if (trusted != trustedComment(tag, channel)) return finish({"update_signature_invalid", "The SHA256SUMS signature is for another release; nothing was changed", {{"check", "release"}, {"trustedComment", trusted}}}, {});
    const auto expected = sumsLine(sums, name);
    if (expected.empty()) return finish({"update_sums_unavailable", "The release's SHA256SUMS doesn't list the package", {{"name", name}}}, {});
    if (asset.contains("sha256") && asset.value("sha256") != expected.c_str())
        return finish({"update_verification_failed", "The update feed and the release's SHA256SUMS disagree; nothing was changed", {{"feed", asset.value("sha256")}, {"sums", expected}}}, {});

    // MSI packages wait in the private data folder; a portable update stages
    // beside putgd.exe so the swap is a rename on one volume.
    const auto staging = here.msi ? paths::join(paths::dataRoot(), "updates") : paths::join(here.directory, ".putgd-update-" + uuid::create().substr(0, 8));
    if (!(here.msi ? files::makePrivateDirectory(staging) : files::makeDirectories(staging)))
        return finish({"update_image_not_writable", "Cannot write beside putgd.exe", {{"directory", here.directory}}}, {});
    auto cleanStaging = [&] { if (!here.msi) files::removeDirectoryRecursively(staging); };
    const long long available = files::freeBytes(staging);
    if (available >= 0 && available < size * 3 + 16 * 1024 * 1024) { cleanStaging(); return finish({"update_disk_full", "There isn't enough free space for the update", {{"free", available}, {"needed", size * 3}}}, {}); }
    const auto part = paths::join(staging, name + ".part"), package = paths::join(staging, name);
    json::Array attempts; std::string lastFailure; bool downloaded = false;
    for (const auto& source : {std::string("release"), std::string("mirror")}) {
        std::string url;
        if (releaseFile(source == "release" ? releaseBase : mirrorBase, tag, name, url)) continue;
        HANDLE file = CreateFileW(paths::wide(part).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) { cleanStaging(); return finish({"update_image_not_writable", "Cannot write the download", {{"directory", staging}}}, {}); }
        crypto::Sha256 hash; Transfer transfer;
        progress("download", 0, size);
        httpGet(url, size, transfer, [&](const char* data, size_t n) {
            DWORD written = 0;
            if (!WriteFile(file, data, DWORD(n), &written, nullptr) || written != n) return false;
            hash.update(data, n); progress("download", transfer.bytes, size);
            return true;
        });
        CloseHandle(file);
        attempts.push_back(json::Object{{"source", source}, {"transfer", transferDetails(url, transfer)}});
        if (!transfer.ok() || transfer.bytes != size) {
            lastFailure = transfer.tooLarge ? "larger than the release says" : !transfer.failure.empty() ? transfer.failure : "HTTP " + text::toString((long long)transfer.status);
            continue;
        }
        // A complete file with the wrong bytes is not retried from elsewhere.
        const auto actual = crypto::hex(hash.finish());
        if (actual != expected) {
            files::remove(part); cleanStaging();
            return finish({"update_verification_failed", "The download doesn't match the release's SHA256SUMS; nothing was changed", {{"check", "sha256"}, {"expected", expected}, {"actual", actual}, {"source", source}}}, {});
        }
        downloaded = files::rename(part, package, true);
        if (!downloaded) { files::remove(part); cleanStaging(); return finish({"update_install_failed", "Cannot prepare the download", {}}, {}); }
        break;
    }
    if (!downloaded) { files::remove(part); cleanStaging(); return finish({"update_download_failed", "Cannot download the new version: " + lastFailure, {{"attempts", attempts}}}, {}); }

    progress("installing", size, size);
    json::Object installed{{"version", version}, {"previousVersion", build::version}, {"channel", channel}, {"tag", tag}, {"sha256", expected},
        {"method", here.msi ? "msi" : "portable"}, {"directory", here.directory}, {"installedAt", files::nowIsoUtc()}};
    json::Object state{{"installed", installed}};
    if (here.msi) {
        // An OLE compound file; the SHA-256 already ties it to the signed release.
        if (head(package, 8) != std::string("\xD0\xCF\x11\xE0\xA1\xB1\x1A\xE1", 8)) { files::remove(package); return finish({"update_verification_failed", "The package is not an MSI", {{"check", "format"}}}, {}); }
        installed["package"] = package; installed["pending"] = true;
        state["installed"] = installed;
    } else {
        // tar.exe (bsdtar, in System32 since Windows 10 1803) reads zip archives.
        const auto tar = systemProgram(L"tar.exe");
        const auto unpacked = paths::join(staging, "unpacked");
        files::makeDirectories(unpacked);
        const long code = tar.empty() || !files::exists(tar) ? -2 : run(tar, {"-x", "-f", package, "-C", unpacked}, 5 * 60 * 1000);
        if (code != 0) { cleanStaging(); return finish({"update_install_failed", "Cannot unpack the update", {{"tarExitCode", double(code)}}}, {}); }
        const auto folder = paths::join(unpacked, "put.gd");
        if (auto e = checkPortable(folder, version)) { cleanStaging(); return finish(e, {}); }
        const auto backup = paths::join(here.directory, ".putgd-update-backup-" + std::string(build::version));
        if (auto e = swapPortable(folder, here.directory, backup)) { cleanStaging(); return finish(e, {}); }
        cleanStaging();
        state["backup"] = json::Object{{"path", backup}};
    }
    if (!writeState(state)) return finish({"update_install_failed", "Cannot save the update's state", {}}, {});
    progress("done", size, size);
    return finish({}, installed);
}

bool startIndependent(const std::string& executable, const std::vector<std::string>& arguments) {
    std::string command = process::quoteArgument(executable);
    for (const auto& a : arguments) command += " " + process::quoteArgument(a);
    for (const DWORD extra : {DWORD(CREATE_BREAKAWAY_FROM_JOB), DWORD(0)}) {
        auto commandW = paths::wide(command);
        STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
        if (CreateProcessW(paths::wide(executable).c_str(), commandW.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS | extra, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
            return true;
        }
    }
    return false;
}
void waitForExit(unsigned long pid, unsigned long timeoutMs) {
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!process) return;
    WaitForSingleObject(process, timeoutMs);
    CloseHandle(process);
}

int applyMain(const std::vector<std::string>& args) {
    if (args.size() != 3) return 2;
    log::initialize(paths::join(paths::dataRoot(), "logs"), "putgd-update");
    const auto& package = args[1];
    waitForExit(std::strtoul(args[2].c_str(), nullptr, 10), 60000);
    const auto logPath = package + ".log";
    const auto msiexec = systemProgram(L"msiexec.exe");
    log::info("updates", "Running the installer", {{"package", paths::fileName(package)}});
    const long code = run(msiexec, {"/i", package, "/passive", "/norestart", "/l*v", logPath}, 30 * 60 * 1000);
    // 3010 and 1641: installed, and Windows wants a restart for something else.
    const bool ok = code == 0 || code == 3010 || code == 1641;
    auto state = readState();
    auto installed = state.value("installed").toObject();
    installed["applied"] = json::Object{{"ok", ok}, {"exitCode", double(code)}, {"at", files::nowIsoUtc()}, {"log", logPath}};
    state["installed"] = installed;
    writeState(state);
    if (ok) log::info("updates", "The installer finished", {{"exitCode", double(code)}});
    else log::warning("updates", "The installer failed; the installed version is unchanged", {{"exitCode", double(code)}});
    // Windows Installer rolls a failed install back, so the same entry starts either version.
    auto location = registryString(L"Software\\put.gd\\Installer", L"InstallLocation");
    if (location.empty()) location = installed.value("directory").toString();
    while (!location.empty() && location.back() == '\\') location.pop_back();
    const auto executable = paths::join(location, "putgd.exe");
    if (!files::exists(executable) || !startIndependent(executable, {"--background"})) log::warning("updates", "Cannot start put.gd again after the installer", {});
    return ok ? 0 : 1;
}
}
