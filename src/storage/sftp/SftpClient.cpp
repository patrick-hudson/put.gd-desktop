#include "SftpClient.h"
#include "SftpSsh.h"
#include <QElapsedTimer>
#include <cerrno>
#include <poll.h>
#include <unistd.h>
namespace putgd::sftp {
namespace {
// No reply for this long means the connection is gone; ssh's own keepalive ends a dead one sooner.
constexpr int replyDeadlineMs = 120000;
// What one pump() reads at most before it parses: a server can't make put.gd buffer more.
constexpr qsizetype receiveBudget = 1024 * 1024;
// put.gd's own cap on one read or write request's data.
constexpr quint64 maximumChunk = 64 * 1024;
// A WRITE or READ packet's bytes besides its data: length, type, id, handle string, offset, length.
quint64 overhead(quint64 handleBytes) { return 4 + 1 + 4 + 4 + handleBytes + 8 + 4 + 64; }
Error cancelledError() { return {"cancelled", "Cancelled", {}}; }
}
Error Client::lost() {
    broken_ = true;
    if (!protocolError_.isEmpty()) return {"sftp_protocol", protocolError_, {}};
    // ssh's own words stay out of the error: they can name hosts, users and paths.
    return {"sftp_connection_lost", "The connection to the server ended before it answered.", {}};
}
Error Client::pump(int milliseconds) {
    if (broken_) return lost();
    if (eof_) return {};
    pollfd fds[2]{{ssh_.output(), POLLIN, 0}, {ssh_.errors(), POLLIN, 0}};
    const int count = ::poll(fds, ssh_.errors() >= 0 ? 2 : 1, milliseconds);
    if (count < 0 && errno != EINTR) return lost();
    if (ssh_.errors() >= 0 && fds[1].revents) ssh_.drainErrors();
    if (!fds[0].revents) return {};
    char buffer[65536];
    qsizetype taken = 0;
    while (taken < receiveBudget) {
        const auto read = ::read(ssh_.output(), buffer, sizeof buffer);
        if (read > 0) {
            framer_.append(QByteArray(buffer, qsizetype(read)));
            taken += read;
            continue;
        }
        if (read < 0 && errno == EINTR) continue;
        if (read == 0) eof_ = true;  // the replies already read still count
        else if (errno != EAGAIN && errno != EWOULDBLOCK) eof_ = true;
        break;
    }
    QString error;
    while (auto packet = framer_.next(&error)) {
        auto reply = wire::parse(*packet, &error);
        if (!reply) { protocolError_ = error; return lost(); }
        // Each outstanding request is answered exactly once; nothing else may arrive.
        if (!outstanding_.erase(reply->id)) {
            protocolError_ = QStringLiteral("The server sent a reply put.gd did not ask for");
            return lost();
        }
        if (abandoned_.erase(reply->id)) continue;  // a request given up on (a cancelled write)
        replies_[reply->id] = std::move(*reply);
    }
    if (!error.isEmpty()) { protocolError_ = error; return lost(); }
    return {};
}
Error Client::send(const QByteArray& packet) {
    qsizetype written = 0;
    QElapsedTimer waited;
    waited.start();
    while (written < packet.size()) {
        // A packet is sent whole: cutting one short would garble the stream for every later request.
        if (written == 0 && cancelled_ && cancelled_->load()) return cancelledError();
        if (eof_) return lost();
        const auto count = ::write(ssh_.input(), packet.constData() + written, size_t(packet.size() - written));
        if (count > 0) { written += count; waited.restart(); continue; }
        if (count < 0 && errno == EINTR) continue;
        if (count < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (waited.elapsed() > replyDeadlineMs) return lost();
            // While ssh can't take more, read its replies so it never waits on us.
            pollfd fds[2]{{ssh_.input(), POLLOUT, 0}, {ssh_.output(), POLLIN, 0}};
            ::poll(fds, 2, 100);
            if (fds[1].revents)
                if (auto e = pump(0)) return e;
            continue;
        }
        return lost();
    }
    return {};
}
Error Client::request(quint32 id, const QByteArray& packet) {
    // Outstanding before the first byte goes: a reply can come back before send() returns.
    outstanding_.insert(id);
    if (auto e = send(packet)) {
        if (e.code == QLatin1String("cancelled")) outstanding_.erase(id);  // nothing of it was sent
        return e;
    }
    return {};
}
Error Client::await(quint32 id, wire::Reply& reply, int deadlineMs) {
    QElapsedTimer waited;
    waited.start();
    while (true) {
        if (const auto found = replies_.find(id); found != replies_.end()) {
            reply = std::move(found->second);
            replies_.erase(found);
            return {};
        }
        if (eof_) return lost();
        if (cancelled_ && cancelled_->load()) {
            abandoned_.insert(id);
            return cancelledError();
        }
        if (waited.elapsed() > deadlineMs) { broken_ = true; return {"sftp_timeout", "The server stopped answering", {}}; }
        if (auto e = pump(100)) return e;
    }
}
Error Client::handshake(bool& noise) {
    noise = false;
    if (auto e = send(wire::init())) return e;
    QElapsedTimer waited;
    waited.start();
    while (true) {
        if (cancelled_ && cancelled_->load()) return cancelledError();
        if (waited.elapsed() > 60000) return lost();
        pollfd fds[2]{{ssh_.output(), POLLIN, 0}, {ssh_.errors(), POLLIN, 0}};
        ::poll(fds, ssh_.errors() >= 0 ? 2 : 1, 100);
        if (ssh_.errors() >= 0 && fds[1].revents) ssh_.drainErrors();
        if (!fds[0].revents) {
            if (!ssh_.running()) return lost();
            continue;
        }
        char buffer[65536];
        const auto read = ::read(ssh_.output(), buffer, sizeof buffer);
        if (read < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (read <= 0) return lost();
        framer_.append(QByteArray(buffer, qsizetype(read)));
        QString error;
        const auto packet = framer_.next(&error);
        if (!error.isEmpty()) { noise = true; protocolError_ = error; return lost(); }
        if (!packet) continue;
        const auto reply = wire::parse(*packet, &error);
        if (!reply || reply->type != wire::Version) { noise = true; protocolError_ = error.isEmpty() ? QStringLiteral("The server did not start SFTP") : error; return lost(); }
        if (reply->version != 3) { protocolError_ = QStringLiteral("The server answered SFTP version %1; put.gd speaks version 3").arg(reply->version); return lost(); }
        extensions_ = reply->extensions;
        break;
    }
    if (has("limits@openssh.com", "1")) {
        const auto id = nextId();
        if (auto e = request(id, wire::extended(id, "limits@openssh.com", {}))) return e;
        wire::Reply reply;
        if (auto e = await(id, reply)) return e;
        if (reply.type != wire::ExtendedReply) { protocolError_ = QStringLiteral("The server answered limits@openssh.com with something else"); return lost(); }
        const auto l = wire::limits(reply.data);
        if (!l) { protocolError_ = QStringLiteral("The server sent malformed limits"); return lost(); }
        // Zero means no limit of that kind; a packet limit bounds the data with the header around it.
        auto fit = [&](quint64 data) {
            auto chunk = data ? qMin(data, maximumChunk) : maximumChunk;
            if (l->packet) chunk = l->packet > overhead(256) ? qMin(chunk, l->packet - overhead(256)) : 0;
            return chunk;
        };
        writeChunk_ = fit(l->write);
        readChunk_ = fit(l->read);
        if (writeChunk_ < 512 || readChunk_ < 512) { protocolError_ = QStringLiteral("The server's SFTP limits are too small to upload with"); return lost(); }
    }
    return {};
}
bool Client::has(const char* extension, const char* version) const {
    for (const auto& [name, data] : extensions_) if (name == extension && data == version) return true;
    return false;
}
Error Client::statusOnly(const wire::Reply& reply, Status& status) {
    if (reply.type != wire::Status) { protocolError_ = QStringLiteral("The server answered with the wrong kind of reply"); return lost(); }
    status = {reply.code, reply.message};
    return {};
}
Error Client::simple(const QByteArray& packet, quint32 id, Status& status) {
    if (auto e = request(id, packet)) return e;
    wire::Reply reply;
    if (auto e = await(id, reply)) return e;
    return statusOnly(reply, status);
}
Error Client::realpath(const QByteArray& path, QByteArray& absolute, Status& status) {
    const auto id = nextId();
    if (auto e = request(id, wire::path(wire::Realpath, id, path.isEmpty() ? QByteArray(".") : path))) return e;
    wire::Reply reply;
    if (auto e = await(id, reply)) return e;
    if (reply.type == wire::Status) { status = {reply.code == wire::Ok ? quint32(wire::Failure) : reply.code, reply.message}; return {}; }
    if (reply.type != wire::Name || reply.names.size() != 1) { protocolError_ = QStringLiteral("The server answered a path request with something else"); return lost(); }
    absolute = reply.names.first().name;
    status = {};
    return {};
}
namespace {
}
Error Client::lstat(const QByteArray& path, wire::Attributes& attributes, Status& status, bool follow) {
    const auto id = nextId();
    if (auto e = request(id, wire::path(follow ? wire::Stat : wire::Lstat, id, path))) return e;
    wire::Reply reply;
    if (auto e = await(id, reply)) return e;
    if (reply.type == wire::Status) { status = {reply.code == wire::Ok ? quint32(wire::Failure) : reply.code, reply.message}; return {}; }
    if (reply.type != wire::Attrs) { protocolError_ = QStringLiteral("The server answered a file check with something else"); return lost(); }
    attributes = reply.attributes;
    status = {};
    return {};
}
Error Client::fstat(const QByteArray& handle, wire::Attributes& attributes, Status& status) {
    const auto id = nextId();
    if (auto e = request(id, wire::path(wire::Fstat, id, handle))) return e;
    wire::Reply reply;
    if (auto e = await(id, reply)) return e;
    if (reply.type == wire::Status) { status = {reply.code == wire::Ok ? quint32(wire::Failure) : reply.code, reply.message}; return {}; }
    if (reply.type != wire::Attrs) { protocolError_ = QStringLiteral("The server answered a file check with something else"); return lost(); }
    attributes = reply.attributes;
    status = {};
    return {};
}
Error Client::openExclusive(const QByteArray& path, QByteArray& handle, Status& status, int deadlineMs) {
    const auto id = nextId();
    if (auto e = request(id, wire::open(id, path, wire::OpenWrite | wire::OpenCreate | wire::OpenExclusive))) return e;
    wire::Reply reply;
    if (auto e = await(id, reply, deadlineMs)) return e;
    if (reply.type == wire::Status) { status = {reply.code == wire::Ok ? quint32(wire::Failure) : reply.code, reply.message}; return {}; }
    if (reply.type != wire::Handle) { protocolError_ = QStringLiteral("The server answered an open with something else"); return lost(); }
    handle = reply.handle;
    status = {};
    return {};
}
Error Client::openRead(const QByteArray& path, QByteArray& handle, Status& status) {
    const auto id = nextId();
    if (auto e = request(id, wire::open(id, path, wire::OpenRead))) return e;
    wire::Reply reply;
    if (auto e = await(id, reply)) return e;
    if (reply.type == wire::Status) { status = {reply.code == wire::Ok ? quint32(wire::Failure) : reply.code, reply.message}; return {}; }
    if (reply.type != wire::Handle) { protocolError_ = QStringLiteral("The server answered an open with something else"); return lost(); }
    handle = reply.handle;
    status = {};
    return {};
}
Error Client::write(const QByteArray& handle, QIODevice& source, qint64 total, QCryptographicHash& hash, const std::function<void(qint64)>& sent, Status& status) {
    constexpr int window = 64;
    std::map<quint32, qint64> inFlight;  // request id → bytes
    qint64 offset = 0, acknowledged = 0;
    QByteArray chunk;
    status = {};
    const auto give = [&] {  // requests not answered yet are given up on: their replies are dropped
        for (const auto& [id, bytes] : inFlight) abandoned_.insert(id);
    };
    QElapsedTimer progress;  // since the last acknowledgement
    progress.start();
    while (acknowledged < total || !inFlight.empty()) {
        if (cancelled_ && cancelled_->load()) { give(); return cancelledError(); }
        while (offset < total && int(inFlight.size()) < window && status.code == wire::Ok) {
            chunk.resize(qsizetype(qMin<qint64>(qint64(writeChunk_), total - offset)));
            const auto read = source.read(chunk.data(), chunk.size());
            if (read != chunk.size()) { give(); return {"artifact_changed", "The file changed or could not be read while it was uploading", {}}; }
            hash.addData(QByteArrayView(chunk.constData(), read));
            const auto id = nextId();
            if (auto e = request(id, wire::write(id, handle, quint64(offset), chunk.constData(), quint32(read)))) { give(); return e; }
            inFlight[id] = read;
            offset += read;
        }
        if (inFlight.empty()) break;
        if (auto e = pump(replies_.empty() ? 100 : 0)) return e;
        bool answered = false;
        for (auto it = inFlight.begin(); it != inFlight.end();) {
            const auto found = replies_.find(it->first);
            if (found == replies_.end()) { ++it; continue; }
            const auto reply = std::move(found->second);
            replies_.erase(found);
            if (reply.type != wire::Status) { protocolError_ = QStringLiteral("The server answered a write with something else"); return lost(); }
            if (reply.code != wire::Ok && status.code == wire::Ok) status = {reply.code, reply.message};
            // Only bytes the server confirmed count as sent.
            if (reply.code == wire::Ok) acknowledged += it->second;
            answered = true;
            it = inFlight.erase(it);
        }
        if (answered) progress.restart();
        else if (eof_) { give(); return lost(); }
        else if (progress.elapsed() > replyDeadlineMs) { give(); broken_ = true; return {"sftp_timeout", "The server stopped acknowledging the upload", {}}; }
        if (status.code != wire::Ok && inFlight.empty()) return {};
        if (sent && answered) sent(acknowledged);
    }
    return {};
}
Error Client::readHash(const QByteArray& handle, qint64 bound, QCryptographicHash& hash, qint64& size, Status& status) {
    size = 0;
    status = {};
    quint64 offset = 0;
    while (size <= bound) {
        // One read at a time, in order: a short read at the end needs no reassembly.
        const auto id = nextId();
        if (auto e = request(id, wire::read(id, handle, offset, quint32(readChunk_)))) return e;
        wire::Reply reply;
        if (auto e = await(id, reply)) return e;
        if (reply.type == wire::Status) {
            if (reply.code == wire::Eof) return {};
            status = {reply.code, reply.message};
            return {};
        }
        if (reply.type != wire::Data || reply.data.isEmpty() || quint64(reply.data.size()) > readChunk_) { protocolError_ = QStringLiteral("The server answered a read with something else"); return lost(); }
        hash.addData(reply.data);
        size += reply.data.size();
        offset += quint64(reply.data.size());
    }
    return {};
}
Error Client::fsync(const QByteArray& handle, Status& status) {
    const auto id = nextId();
    return simple(wire::extended(id, "fsync@openssh.com", wire::string(handle)), id, status);
}
Error Client::close(const QByteArray& handle, Status& status) {
    const auto id = nextId();
    return simple(wire::close(id, handle), id, status);
}
Error Client::remove(const QByteArray& path, Status& status) {
    const auto id = nextId();
    return simple(wire::path(wire::Remove, id, path), id, status);
}
Error Client::mkdir(const QByteArray& path, Status& status) {
    const auto id = nextId();
    return simple(wire::mkdir(id, path), id, status);
}
std::optional<wire::FileSystem> Client::statvfs(const QByteArray& path) {
    if (!has("statvfs@openssh.com", "2")) return std::nullopt;
    const auto id = nextId();
    if (request(id, wire::extended(id, "statvfs@openssh.com", wire::string(path)))) return std::nullopt;
    wire::Reply reply;
    if (await(id, reply) || reply.type != wire::ExtendedReply) return std::nullopt;
    return wire::fileSystem(reply.data);
}
}
