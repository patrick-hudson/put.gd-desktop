#include "common/Process.h"
#include "common/Log.h"
#include "common/Paths.h"
#include "core/CrashRecord.h"
#include "core/LogFormat.h"
#include <atomic>
#include <cstdio>
#include <mutex>

namespace putgd::process {
struct Child::Output {
    std::mutex mutex;
    std::string killReason;
    std::atomic_bool exitLogged{false};
    bool quiet = false;
};
namespace {
// Frequent one-shot helpers start and exit at debug level, as the Linux S3 worker does.
bool quietHelper(const std::string& name) { return name == "putgd-s3-worker" || name == "putgd-image-job" || name == "putgd-update-check"; }
void logExit(const std::string& name, DWORD pid, HANDLE process, Child::Output& output) {
    DWORD code = 0;
    if (output.exitLogged.exchange(true) || !GetExitCodeProcess(process, &code) || code == STILL_ACTIVE) return;
    std::string reason; { std::lock_guard lock(output.mutex); reason = output.killReason; }
    json::Object fields{{"helper", name}, {"pid", double(pid)}};
    if (!reason.empty()) { fields["supervisorKill"] = true; fields["reason"] = reason; fields["exitCode"] = double(code); log::warning("process", "Helper killed by put.gd", fields); return; }
    if (code >= 0xC0000000UL) {
        // An NTSTATUS exit: the process ended on an exception (or another process chose that code).
        char hex[16]; std::snprintf(hex, sizeof hex, "0x%08lX", static_cast<unsigned long>(code));
        fields["exceptionCode"] = hex; fields["exceptionName"] = crash::exceptionName(code);
        log::error("process", "Helper crashed", fields); return;
    }
    fields["exitCode"] = double(code);
    log::write(code == 0 ? (output.quiet ? log::Level::Debug : log::Level::Info) : log::Level::Warning, "process", "Helper exited", fields);
}
}
Child::~Child() {
    if (channel) channel->close();
    if (rawStdin) CloseHandle(rawStdin);
    if (rawStdout) CloseHandle(rawStdout);
    if (process && WaitForSingleObject(process, 0) != WAIT_OBJECT_0) terminate("released");
    if (stderrDrain.joinable()) stderrDrain.join();
    if (process) { if (output) logExit(name, pid, process, *output); CloseHandle(process); }
    if (stderrRead) CloseHandle(stderrRead);
    if (job) CloseHandle(job);
}
bool Child::running() const { return process && WaitForSingleObject(process, 0) == WAIT_TIMEOUT; }
int Child::exitCode() const {
    DWORD code = 0;
    if (!process || WaitForSingleObject(process, 0) != WAIT_OBJECT_0 || !GetExitCodeProcess(process, &code)) return -1;
    return int(code);
}
bool Child::waitExit(int timeoutMs) { return process && WaitForSingleObject(process, timeoutMs < 0 ? INFINITE : DWORD(timeoutMs)) == WAIT_OBJECT_0; }
void Child::terminate(const std::string& reason) {
    if (!process || WaitForSingleObject(process, 0) == WAIT_OBJECT_0) return;
    if (output) { std::lock_guard lock(output->mutex); if (output->killReason.empty()) output->killReason = reason; }
    log::warning("process", "Killing helper", {{"helper", name}, {"pid", double(pid)}, {"reason", reason}});
    TerminateProcess(process, 9);
}

std::string quoteArgument(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string r = "\"";
    size_t backslashes = 0;
    for (const char c : a) {
        if (c == '\\') { ++backslashes; continue; }
        if (c == '"') { r.append(backslashes * 2 + 1, '\\'); r += '"'; backslashes = 0; continue; }
        r.append(backslashes, '\\'); backslashes = 0; r += c;
    }
    r.append(backslashes * 2, '\\');
    return r + "\"";
}
namespace {
bool makePipe(HANDLE& readEnd, HANDLE& writeEnd, bool inheritRead) {
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    if (!CreatePipe(&readEnd, &writeEnd, &sa, 1 << 16)) return false;
    SetHandleInformation(inheritRead ? writeEnd : readEnd, HANDLE_FLAG_INHERIT, 0);
    return true;
}
std::wstring environmentBlock(const std::map<std::string, std::string>& additions, const std::vector<std::string>& dropPrefixes) {
    std::map<std::wstring, std::wstring, bool(*)(const std::wstring&, const std::wstring&)> variables([](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    if (wchar_t* strings = GetEnvironmentStringsW()) {
        for (const wchar_t* p = strings; *p; p += wcslen(p) + 1) {
            const std::wstring entry(p);
            const auto eq = entry.find(L'=', 1);
            if (eq == std::wstring::npos) continue;
            const auto name = entry.substr(0, eq);
            bool drop = false;
            for (const auto& prefix : dropPrefixes) { const auto w = paths::wide(prefix); if (name.size() >= w.size() && _wcsnicmp(name.c_str(), w.c_str(), w.size()) == 0) drop = true; }
            if (!drop) variables[name] = entry.substr(eq + 1);
        }
        FreeEnvironmentStringsW(strings);
    }
    for (const auto& pair : additions) variables[paths::wide(pair.first)] = paths::wide(pair.second);
    std::wstring block;
    for (const auto& pair : variables) { block += pair.first; block += L'='; block += pair.second; block += L'\0'; }
    block += L'\0';
    return block;
}
std::unique_ptr<Child> spawn(const std::string& executable, const std::vector<std::string>& arguments, bool hidden, bool raw,
                             const std::map<std::string, std::string>& environment, const std::vector<std::string>& dropPrefixes) {
    auto child = std::make_unique<Child>();
    HANDLE stdinRead, stdinWrite, stdoutRead, stdoutWrite, stderrWrite;
    if (!makePipe(stdinRead, stdinWrite, true) || !makePipe(stdoutRead, stdoutWrite, false) || !makePipe(child->stderrRead, stderrWrite, false)) return nullptr;
    std::string command = quoteArgument(executable);
    for (const auto& a : arguments) command += " " + quoteArgument(a);
    STARTUPINFOW si{}; si.cb = sizeof si; si.dwFlags = STARTF_USESTDHANDLES; si.hStdInput = stdinRead; si.hStdOutput = stdoutWrite; si.hStdError = stderrWrite;
    PROCESS_INFORMATION pi{};
    child->job = CreateJobObjectW(nullptr, nullptr);
    if (child->job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
        limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(child->job, JobObjectExtendedLimitInformation, &limits, sizeof limits);
    }
    auto commandW = paths::wide(command);
    const DWORD flags = CREATE_UNICODE_ENVIRONMENT | CREATE_SUSPENDED | (hidden ? CREATE_NO_WINDOW : 0);
    std::wstring block; if (raw) block = environmentBlock(environment, dropPrefixes);
    const BOOL created = CreateProcessW(paths::wide(executable).c_str(), commandW.data(), nullptr, nullptr, TRUE, flags, raw ? block.data() : nullptr, nullptr, &si, &pi);
    CloseHandle(stdinRead); CloseHandle(stdoutWrite); CloseHandle(stderrWrite);
    if (!created) { CloseHandle(stdinWrite); CloseHandle(stdoutRead); log::warning("process", "CreateProcess failed", {{"error", double(GetLastError())}}); return nullptr; }
    if (child->job) AssignProcessToJobObject(child->job, pi.hProcess);
    ResumeThread(pi.hThread); CloseHandle(pi.hThread);
    child->process = pi.hProcess; child->pid = pi.dwProcessId;
    child->name = logformat::helperProcessName(executable, arguments);
    child->output = std::make_shared<Child::Output>();
    child->output->quiet = quietHelper(child->name);
    log::write(child->output->quiet ? log::Level::Debug : log::Level::Info, "process", "Helper started", {{"helper", child->name}, {"pid", double(pi.dwProcessId)}});
    if (raw) { child->rawStdin = stdinWrite; child->rawStdout = stdoutRead; }
    else child->channel = std::make_unique<frames::Channel>(stdoutRead, stdinWrite);
    HANDLE err = child->stderrRead, watched = nullptr;
    DuplicateHandle(GetCurrentProcess(), pi.hProcess, GetCurrentProcess(), &watched, SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, 0);
    child->stderrDrain = std::thread([err, watched, raw, output = child->output, name = child->name, pid = pi.dwProcessId] {
        // A raw child is a separate runtime (the Proton helper): its stderr is read
        // and discarded, as on Linux; only its start and exit are recorded.
        logformat::HelperOutput collected(name, pid, [raw](logformat::Record r) { if (!raw) log::writeRecord(std::move(r)); }, [] { return log::nowMs(); });
        char buffer[4096]; DWORD got = 0;
        while (ReadFile(err, buffer, sizeof buffer, &got, nullptr) && got > 0) collected.append(std::string_view(buffer, got));
        // The crash record and the truncation summary come before the exit record.
        collected.finish();
        if (watched) { if (WaitForSingleObject(watched, 3000) == WAIT_OBJECT_0) logExit(name, pid, watched, *output); CloseHandle(watched); }
    });
    return child;
}
}
std::unique_ptr<Child> start(const std::string& executable, const std::vector<std::string>& arguments, bool hidden) { return spawn(executable, arguments, hidden, false, {}, {}); }
std::unique_ptr<Child> startRaw(const std::string& executable, const std::vector<std::string>& arguments, const std::map<std::string, std::string>& environment, const std::vector<std::string>& dropPrefixes) {
    return spawn(executable, arguments, true, true, environment, dropPrefixes);
}
bool launchDetached(const std::string& executable, const std::vector<std::string>& arguments) {
    std::string command = quoteArgument(executable);
    for (const auto& a : arguments) command += " " + quoteArgument(a);
    auto commandW = paths::wide(command);
    STARTUPINFOW si{}; si.cb = sizeof si; PROCESS_INFORMATION pi{};
    if (!CreateProcessW(paths::wide(executable).c_str(), commandW.data(), nullptr, nullptr, FALSE, CREATE_UNICODE_ENVIRONMENT | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) return false;
    CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    return true;
}
}
