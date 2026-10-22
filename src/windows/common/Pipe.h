#pragma once
// The session endpoint: a named pipe reachable only by the current user
// (\\.\pipe\putgd-<sid hash>), carrying one newline-terminated JSON request and
// one newline-terminated JSON reply per connection, like the Linux Unix socket
// (docs/cli.md: 1 MiB request, 10 s framing deadline, 64 clients, 16 MiB reply).
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <windows.h>

namespace putgd::pipe {
inline constexpr size_t maximumRequestBytes = 1024 * 1024;
inline constexpr size_t maximumReplyBytes = 16 * 1024 * 1024;
inline constexpr int framingDeadlineMs = 10000;
inline constexpr int maximumClients = 64;
enum class Probe { Connected, Absent, Unknown };
// Can a client connect? A pipe with no server does not exist on Windows, so
// Absent means no resident owns this session.
Probe probe(const std::string& path, int timeoutMs);
// One request/reply exchange; nullopt on connection, framing or deadline failure.
// `error` receives session_unavailable, transport_failed, response_too_large or invalid_response.
std::optional<std::string> request(const std::string& path, const std::string& requestLine, int replyTimeoutMs, std::string* error = nullptr);
// Called on a pipe thread with the raw request line (without the newline); the
// handler must eventually call reply exactly once, from any thread.
using Handler = std::function<void(std::string request, std::function<void(std::string replyLine)> reply)>;
class Server {
public:
    Server();
    ~Server();
    Server(const Server&) = delete;
    // Starts accepting; false when the pipe name is already served or cannot be created.
    bool listen(const std::string& path, Handler);
    void stop();
    const std::string& path() const { return path_; }
private:
    struct State;
    std::shared_ptr<State> state_;
    std::string path_;
};
}
