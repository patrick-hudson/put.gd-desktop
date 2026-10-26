#include "common/Pipe.h"
#include "common/Log.h"
#include "common/Paths.h"
#include <atomic>
#include <memory>
#include <sddl.h>
#include <thread>
#include <vector>

namespace putgd::pipe {
namespace {
bool readLine(HANDLE h, std::string& line, size_t maximum, int timeoutMs, bool& tooLarge) {
    const ULONGLONG start = GetTickCount64();
    char buffer[65536];
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(h, nullptr, 0, nullptr, &available, nullptr)) return false;
        if (available == 0) {
            if (GetTickCount64() - start >= ULONGLONG(timeoutMs)) return false;
            Sleep(5);
            continue;
        }
        DWORD got = 0;
        if (!ReadFile(h, buffer, DWORD(std::min<DWORD>(available, sizeof buffer)), &got, nullptr) || got == 0) return false;
        line.append(buffer, got);
        if (line.size() > maximum + 1) { tooLarge = true; return false; }
        const auto newline = line.find('\n');
        if (newline != std::string::npos) { line.resize(newline); return true; }
    }
}
bool writeAll(HANDLE h, const std::string& bytes) {
    size_t offset = 0;
    while (offset < bytes.size()) { DWORD written = 0; if (!WriteFile(h, bytes.data() + offset, DWORD(bytes.size() - offset), &written, nullptr) || written == 0) return false; offset += written; }
    return FlushFileBuffers(h) || true;
}
}
Probe probe(const std::string& path, int timeoutMs) {
    const auto wide = paths::wide(path);
    HANDLE h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h != INVALID_HANDLE_VALUE) { CloseHandle(h); return Probe::Connected; }
    const DWORD error = GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) return Probe::Absent;
    if (error == ERROR_PIPE_BUSY) return WaitNamedPipeW(wide.c_str(), DWORD(timeoutMs)) || GetLastError() == ERROR_SEM_TIMEOUT ? Probe::Connected : Probe::Unknown;
    return Probe::Unknown;
}
std::optional<std::string> request(const std::string& path, const std::string& line, int replyTimeoutMs, std::string* error) {
    const auto wide = paths::wide(path);
    HANDLE h = INVALID_HANDLE_VALUE;
    const ULONGLONG start = GetTickCount64();
    while (true) {
        h = CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) break;
        const DWORD last = GetLastError();
        if (last == ERROR_PIPE_BUSY && GetTickCount64() - start < 3000) { WaitNamedPipeW(wide.c_str(), 500); continue; }
        if (error) *error = "session_unavailable";
        return std::nullopt;
    }
    std::string reply; bool tooLarge = false;
    const bool ok = writeAll(h, line + "\n") && readLine(h, reply, maximumReplyBytes, replyTimeoutMs, tooLarge);
    CloseHandle(h);
    if (!ok) { if (error) *error = tooLarge ? "response_too_large" : "transport_failed"; return std::nullopt; }
    return reply;
}

struct Server::State {
    std::atomic_bool stopping{false};
    std::atomic_int clients{0};
    std::wstring path;
    Handler handler;
    std::thread acceptor;
    HANDLE current = INVALID_HANDLE_VALUE; // the instance currently waiting in ConnectNamedPipe
    PSECURITY_DESCRIPTOR descriptor = nullptr;
    HANDLE createInstance() {
        SECURITY_ATTRIBUTES sa{sizeof sa, descriptor, FALSE};
        return CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE * 0, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 20, 0, &sa);
    }
    void serve(std::shared_ptr<State> self, HANDLE client) {
        std::string line; bool tooLarge = false;
        auto finish = [client](const std::string& reply) { writeAll(client, reply + "\n"); FlushFileBuffers(client); DisconnectNamedPipe(client); CloseHandle(client); };
        if (!readLine(client, line, maximumRequestBytes, framingDeadlineMs, tooLarge)) {
            if (tooLarge) finish(R"({"schemaVersion":1,"requestId":"","ok":false,"result":{},"error":{"code":"request_too_large","message":"Request exceeds 1 MiB","details":{}}})");
            else { DisconnectNamedPipe(client); CloseHandle(client); }
            --clients; return;
        }
        handler(std::move(line), [finish, self](std::string reply) { finish(reply); --self->clients; });
    }
};
Server::Server() : state_(std::make_shared<State>()) {}
Server::~Server() { stop(); }
bool Server::listen(const std::string& path, Handler handler) {
    path_ = path;
    state_->path = paths::wide(path);
    state_->handler = std::move(handler);
    const auto sid = paths::userSidString();
    if (sid.empty()) return false;
    const std::string sddl = "D:P(A;;GA;;;SY)(A;;GA;;;" + sid + ")";
    if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(paths::wide(sddl).c_str(), SDDL_REVISION_1, &state_->descriptor, nullptr)) return false;
    // Refuse to shadow a live resident: the first instance must be ours.
    SECURITY_ATTRIBUTES sa{sizeof sa, state_->descriptor, FALSE};
    HANDLE first = CreateNamedPipeW(state_->path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                    PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 20, 0, &sa);
    if (first == INVALID_HANDLE_VALUE) return false;
    auto state = state_;
    state->current = first;
    state->acceptor = std::thread([state, first] {
        HANDLE instance = first;
        while (!state->stopping) {
            const BOOL connected = ConnectNamedPipe(instance, nullptr) || GetLastError() == ERROR_PIPE_CONNECTED;
            if (state->stopping) { CloseHandle(instance); break; }
            if (!connected) { CloseHandle(instance); instance = state->createInstance(); state->current = instance; if (instance == INVALID_HANDLE_VALUE) break; continue; }
            HANDLE client = instance;
            instance = state->createInstance();
            state->current = instance;
            if (state->clients.load() >= maximumClients) { DisconnectNamedPipe(client); CloseHandle(client); }
            else { ++state->clients; std::thread([state, client] { state->serve(state, client); }).detach(); }
            if (instance == INVALID_HANDLE_VALUE) break;
        }
    });
    return true;
}
void Server::stop() {
    if (!state_ || state_->stopping.exchange(true)) return;
    // Wake the acceptor by connecting to the waiting instance, then join it.
    HANDLE wake = CreateFileW(state_->path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    if (wake != INVALID_HANDLE_VALUE) CloseHandle(wake);
    if (state_->acceptor.joinable()) state_->acceptor.join();
    if (state_->descriptor) { LocalFree(state_->descriptor); state_->descriptor = nullptr; }
}
}
