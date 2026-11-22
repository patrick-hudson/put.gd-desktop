#pragma once
#include "SftpWire.h"
#include "core/Error.h"
#include <QCryptographicHash>
#include <QIODevice>
#include <atomic>
#include <functional>
#include <map>
#include <set>
namespace putgd::sftp {
class SshProcess;
// put.gd's SFTP v3 client over one ssh child's stdin and stdout (#1871). Writes are
// pipelined: up to 64 requests in flight, each acknowledged before its bytes count as sent.
// Every reply must answer a request still outstanding, exactly once; anything else is a
// protocol error and ends the connection.
class Client {
public:
    Client(SshProcess& ssh, const std::atomic_bool* cancelled) : ssh_(ssh), cancelled_(cancelled) {}
    // INIT and VERSION (exactly 3), then limits@openssh.com when offered. `noise` is set
    // when the stream began with something that is not SFTP: text a shell printed at startup.
    Error handshake(bool& noise);
    // An extension the server offered, at the version put.gd implements.
    bool has(const char* extension, const char* version) const;
    void setCancellation(const std::atomic_bool* cancelled) { cancelled_ = cancelled; }
    struct Status { quint32 code = 0; QString message; };
    // An SFTP request that got a status other than OK: the code in `status`, no Error.
    Error realpath(const QByteArray& path, QByteArray& absolute, Status& status);
    Error lstat(const QByteArray& path, wire::Attributes& attributes, Status& status, bool follow = false);
    Error fstat(const QByteArray& handle, wire::Attributes& attributes, Status& status, int deadlineMs = 120000);
    // `deadlineMs` bounds the wait for the reply; a reply that doesn't come leaves the outcome unknown.
    Error openExclusive(const QByteArray& path, QByteArray& handle, Status& status, int deadlineMs = 120000);
    Error openRead(const QByteArray& path, QByteArray& handle, Status& status);
    // Sends `total` bytes from `source`, hashing what it sends; `sent` is called with the
    // bytes the server has acknowledged. No acknowledgement for two minutes ends it.
    Error write(const QByteArray& handle, QIODevice& source, qint64 total, QCryptographicHash& hash, const std::function<void(qint64)>& sent, Status& status);
    // Reads up to `bound` bytes and hashes them; `size` is how many there were.
    Error readHash(const QByteArray& handle, qint64 bound, QCryptographicHash& hash, qint64& size, Status& status);
    Error fsync(const QByteArray& handle, Status& status);
    Error close(const QByteArray& handle, Status& status, int deadlineMs = 120000);
    Error remove(const QByteArray& path, Status& status, int deadlineMs = 120000);
    Error mkdir(const QByteArray& path, Status& status);
    std::optional<wire::FileSystem> statvfs(const QByteArray& path);
    // The connection is gone, or the server broke the protocol: nothing more can be asked.
    bool broken() const { return broken_; }
private:
    quint32 nextId() { return ++id_; }
    // Sends one request and records its id as outstanding.
    // `deadlineMs` bounds the whole send: a packet ssh can't take in time leaves the stream unusable.
    Error request(quint32 id, const QByteArray& packet, int deadlineMs = 120000);
    Error send(const QByteArray& packet, int deadlineMs = 120000);
    // Reads what has arrived (at most a fixed budget), waiting up to `milliseconds`; replies to
    // outstanding requests go into replies_.
    Error pump(int milliseconds);
    Error await(quint32 id, wire::Reply& reply, int deadlineMs = 120000);
    Error simple(const QByteArray& packet, quint32 id, Status& status, int deadlineMs = 120000);
    Error statusOnly(const wire::Reply&, Status&);
    Error lost();
    SshProcess& ssh_;
    const std::atomic_bool* cancelled_;
    wire::Framer framer_;
    std::map<quint32, wire::Reply> replies_;
    std::set<quint32> outstanding_, abandoned_;
    QList<std::pair<QByteArray, QByteArray>> extensions_;
    quint32 id_ = 0;
    quint64 writeChunk_ = 32768, readChunk_ = 32768;
    bool broken_ = false, eof_ = false;
    QString protocolError_;
};
}
