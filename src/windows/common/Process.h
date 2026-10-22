#pragma once
// Supervised child processes with a framed control channel. Children run the
// same executable in a helper mode; they die with the parent (job object) and
// their stderr is drained to the log without being parsed.
#include "common/Frames.h"
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace putgd::process {
struct Child {
    HANDLE process = nullptr;
    HANDLE job = nullptr;
    DWORD pid = 0;
    std::unique_ptr<frames::Channel> channel; // parent's view of the child's stdin/stdout (framed helpers)
    HANDLE rawStdin = nullptr, rawStdout = nullptr; // the same pipes for a raw (newline-JSON) child
    HANDLE stderrRead = nullptr;
    std::thread stderrDrain;
    ~Child();
    bool running() const;
    // Exit code once exited; -1 while running.
    int exitCode() const;
    bool waitExit(int timeoutMs);
    void terminate();
};
// Starts `executable args...` with piped stdin/stdout/stderr and no inherited
// environment additions. `hidden` creates no console window.
std::unique_ptr<Child> start(const std::string& executable, const std::vector<std::string>& arguments, bool hidden = true);
// Like start, but the child's stdin/stdout stay raw handles (rawStdin/rawStdout)
// and its environment is this process' environment without variables whose names
// start with one of `dropPrefixes`, plus `environment`.
std::unique_ptr<Child> startRaw(const std::string& executable, const std::vector<std::string>& arguments,
                                const std::map<std::string, std::string>& environment, const std::vector<std::string>& dropPrefixes);
// Fire-and-forget launch (ShellExecute-free): used for `putgd --background` relaunch.
bool launchDetached(const std::string& executable, const std::vector<std::string>& arguments);
std::string quoteArgument(const std::string&);
}
