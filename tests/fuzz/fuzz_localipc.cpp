// The session socket's transport (app/LocalIpc.cpp): the client calls that read one
// reply frame from the resident (and from whatever listens at the path), and the
// resident's side that reads a request from any process that connects. The peer
// is a thread that sends the fuzzer's bytes in pieces and then closes.
#include "FuzzPeer.h"
#include "FuzzSupport.h"
#include "app/LocalIpc.h"
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <atomic>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
using namespace putgd;

namespace {
QString socketPath;
bool frameOrError(const QByteArray& response, const QString& error, qsizetype limit) {
    if (!error.isEmpty()) return response.isEmpty();
    return response.endsWith('\n') && response.indexOf('\n') == response.size() - 1 && response.size() <= limit;
}
}

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    fuzz::application(argc, argv);
    static QTemporaryDir directory("/tmp/pgfz-XXXXXX");
    socketPath = directory.filePath("s");
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    const auto mode = in.byte();
    fuzz::Script script;
    script.piece = 1 + in.number(300);
    script.halfClose = mode & 8;
    fuzz::Peer peer;
    if (mode % 3 != 2 && !peer.listen(socketPath)) return 0;
    switch (mode % 3) {
    case 0: {
        // exchangeLocalSession: a request out, one frame back, at most `limit` bytes.
        const auto request = in.part();
        const qsizetype limit = 1 + in.number(6000);
        script.reply = in.rest();
        script.readFirst = !request.isEmpty();
        peer.serve(script);
        const auto result = exchangeLocalSession(socketPath, request, 1500, limit);
        peer.join();
        FUZZ_REQUIRE(result.connected);
        FUZZ_REQUIRE(frameOrError(result.response, result.errorCode, limit));
        break;
    }
    case 1: {
        const auto request = in.part();
        const bool admit = in.byte() & 1;
        script.reply = in.rest();
        script.readFirst = !request.isEmpty();
        peer.serve(script);
        const auto result = activateLocalSession(socketPath, request, [admit](const LocalPeer& p) { return admit && p.connected; });
        peer.join();
        if (!result.connected) break;
        FUZZ_REQUIRE(result.refused == !admit);
        if (!result.refused && !request.isEmpty()) FUZZ_REQUIRE(frameOrError(result.response, result.errorCode, 16 * 1024));
        break;
    }
    default: {
        // The resident's side: a client sends bytes; the server reads, replies and closes.
        LocalIpcServer server;
        if (!server.listen(socketPath)) break;
        const qsizetype limit = 1 + in.number(70000);
        const auto request = in.rest();
        std::atomic<bool> clientDone{false};
        std::thread client([&] {
            const int fd = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
            sockaddr_un address{};
            address.sun_family = AF_UNIX;
            const auto name = socketPath.toUtf8();
            memcpy(address.sun_path, name.constData(), size_t(name.size()) + 1);
            if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&address), sizeof address) != 0) { if (fd >= 0) ::close(fd); clientDone = true; return; }
            size_t at = 0;
            while (at < size_t(request.size())) {
                const auto sent = ::send(fd, request.constData() + at, std::min(script.piece, size_t(request.size()) - at), MSG_NOSIGNAL);
                if (sent <= 0) break;
                at += size_t(sent);
            }
            if (script.halfClose) ::shutdown(fd, SHUT_WR);
            char buffer[256];
            pollfd readable{fd, POLLIN, 0};
            if (::poll(&readable, 1, 40) > 0) (void)::recv(fd, buffer, sizeof buffer, 0);
            ::close(fd);
            clientDone = true;
        });
        QElapsedTimer clock;
        clock.start();
        LocalIpcConnection* connection = nullptr;
        bool done = false;
        QByteArray got;
        int after = 0;
        while (!done && clock.elapsed() < 600 && after < 4) {
            if (clientDone) ++after;
            QCoreApplication::processEvents(QEventLoop::AllEvents, 20);
            if (!connection && (connection = server.nextPendingConnection())) {
                connection->setReadBufferSize(limit);
                QObject::connect(connection, &LocalIpcConnection::disconnected, [&] { done = true; });
                QObject::connect(connection, &LocalIpcConnection::readyRead, [&] {
                    const auto chunk = connection->readAll();
                    FUZZ_REQUIRE(chunk.size() <= limit);
                    got += chunk;
                    if (got.size() >= 1 && got.contains('\n')) { connection->stopReading(); connection->sendAndClose("{\"ok\":true}\n"); }
                });
            }
        }
        client.join();
        // Deleting the connection with a pending reply, or after the peer left, is part of the contract.
        delete connection;
        break;
    }
    }
    peer.join();
    return 0;
}
