#pragma once
#include "SftpWire.h"
#include "core/Error.h"
#include <QCryptographicHash>
#include <QIODevice>
#include <atomic>
#include <functional>
#include <map>
namespace putgd::sftp {
class SshProcess;
// put.gd's SFTP v3 client over one ssh child's stdin and stdout (#1871). Writes are
// pipelined: up to 64 requests in flight, each acknowledged before its bytes count as sent.
class Client {
public:
    Client(SshProcess& ssh, const std::atomic_bool* cancelled) : ssh_(ssh), cancelled_(cancelled) {}
    // INIT and VERSION. `noise` is set when the stream began with something that is not
    // SFTP: text a shell printed at startup.
    Error handshake(bool& noise);
    bool has(const char* extension) const;
    void setCancellation(const std::atomic_bool* cancelled) { cancelled_ = cancelled; }
    struct Status { quint32 code = 0; QString message; };
    // An SFTP request that got a status other than OK: the code in `status`, no Error.
    Error realpath(const QByteArray& path, QByteArray& absolute, Status& status);
    Error lstat(const QByteArray& path, wire::Attributes& attributes, Status& status, bool follow = false);
    Error openExclusive(const QByteArray& path, QByteArray& handle, Status& status);
    Error openRead(const QByteArray& path, QByteArray& handle, Status& status);
    // Sends `total` bytes from `source`, hashing what it sends; `sent` is called with
    // the bytes the server has acknowledged.
    Error write(const QByteArray& handle, QIODevice& source, qint64 total, QCryptographicHash& hash, const std::function<void(qint64)>& sent, Status& status);
    // Reads up to `bound` bytes and hashes them; `size` is how many there were.
    Error readHash(const QByteArray& handle, qint64 bound, QCryptographicHash& hash, qint64& size, Status& status);
    Error fsync(const QByteArray& handle, Status& status);
    Error close(const QByteArray& handle, Status& status);
    Error remove(const QByteArray& path, Status& status);
    Error mkdir(const QByteArray& path, Status& status);
    std::optional<wire::FileSystem> statvfs(const QByteArray& path);
    // The connection is gone, or the server broke the protocol: nothing more can be asked.
    bool broken() const { return broken_; }
private:
    quint32 nextId() { return ++id_; }
    Error send(const QByteArray& packet);
    // Reads what has arrived, waiting up to `milliseconds`; replies go into replies_.
    Error pump(int milliseconds);
    Error await(quint32 id, wire::Reply& reply);
    Error simple(const QByteArray& packet, quint32 id, Status& status);
    Error lost();
    SshProcess& ssh_;
    const std::atomic_bool* cancelled_;
    wire::Framer framer_;
    std::map<quint32, wire::Reply> replies_;
    QList<std::pair<QByteArray, QByteArray>> extensions_;
    quint32 id_ = 0;
    quint64 writeChunk_ = 32768;
    bool broken_ = false;
    QString protocolError_;
};
}
