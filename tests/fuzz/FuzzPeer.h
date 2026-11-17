#pragma once
// A scripted peer on a Unix socket, for the targets that read from a socket.
// One accepted connection per run: it reads what the client sends (up to a
// budget), writes the script's bytes in pieces, then closes.
#include <QByteArray>
#include <QString>
#include <algorithm>
#include <cerrno>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <atomic>
#include <thread>
#include <unistd.h>

namespace fuzz {
struct Script {
    QByteArray reply;
    size_t piece = 4096;      // bytes per write
    bool readFirst = true;    // wait for a request before replying
    bool halfClose = false;   // shutdown(SHUT_WR) instead of a full close
};

class Peer {
public:
    // Listens at path (unlinked first); false when the socket cannot be made.
    bool listen(const QString& path) {
        path_ = path.toUtf8();
        ::unlink(path_.constData());
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path_.size() >= qsizetype(sizeof address.sun_path)) return false;
        memcpy(address.sun_path, path_.constData(), size_t(path_.size()) + 1);
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0 || ::listen(fd_, 4) != 0) { ::close(fd_); fd_ = -1; return false; }
        return true;
    }
    // Serves one connection in a thread. Call join() before the next run.
    void serve(Script script) {
        finished_ = false;
        thread_ = std::thread([this, script = std::move(script)] {
            struct Done { std::atomic<bool>& flag; ~Done() { flag = true; } } done{finished_};
            pollfd waiting{fd_, POLLIN, 0};
            if (::poll(&waiting, 1, 2000) <= 0) return;
            const int client = ::accept(fd_, nullptr, nullptr);
            if (client < 0) return;
            if (script.readFirst) {
                pollfd readable{client, POLLIN, 0};
                char buffer[4096];
                if (::poll(&readable, 1, 50) > 0) (void)::recv(client, buffer, sizeof buffer, MSG_DONTWAIT);
            }
            size_t at = 0;
            while (at < size_t(script.reply.size())) {
                const size_t count = std::min(script.piece ? script.piece : size_t(1), size_t(script.reply.size()) - at);
                const auto sent = ::send(client, script.reply.constData() + at, count, MSG_NOSIGNAL);
                if (sent < 0 && errno == EINTR) continue;
                if (sent <= 0) break;
                at += size_t(sent);
            }
            if (script.halfClose) { ::shutdown(client, SHUT_WR); pollfd drain{client, POLLIN, 0}; (void)::poll(&drain, 1, 50); }
            ::close(client);
        });
    }
    // True once the thread has written everything and closed.
    bool finished() const { return finished_; }
    void join() {
        if (thread_.joinable()) thread_.join();
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
        if (!path_.isEmpty()) ::unlink(path_.constData());
    }
    ~Peer() { join(); }
private:
    QByteArray path_;
    int fd_ = -1;
    std::thread thread_;
    std::atomic<bool> finished_{true};
};
}
