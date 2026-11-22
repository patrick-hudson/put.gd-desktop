#pragma once
#include <QByteArray>
#include <QList>
#include <QString>
#include <optional>
// SFTP version 3 (draft-ietf-secsh-filexfer-02), the version OpenSSH's sftp-server speaks,
// as putgd-sftp-worker sends and reads it (#1871). Every length a server sends is checked
// against what is left of its packet and against a fixed bound before it is used.
namespace putgd::sftp::wire {
enum Type : quint8 {
    Init = 1, Version = 2, Open = 3, Close = 4, Read = 5, Write = 6, Lstat = 7, Fstat = 8, Opendir = 11, Readdir = 12,
    Remove = 13, Mkdir = 14, Realpath = 16, Stat = 17, Status = 101, Handle = 102, Data = 103, Name = 104, Attrs = 105,
    Extended = 200, ExtendedReply = 201
};
enum OpenFlag : quint32 { OpenRead = 1, OpenWrite = 2, OpenCreate = 8, OpenTruncate = 0x10, OpenExclusive = 0x20 };
enum StatusCode : quint32 { Ok = 0, Eof = 1, NoSuchFile = 2, PermissionDenied = 3, Failure = 4, BadMessage = 5, NoConnection = 6, ConnectionLost = 7, Unsupported = 8 };
// The largest packet put.gd accepts: OpenSSH's own limit is 256 KB.
inline constexpr quint32 maximumPacket = 1024 * 1024;
inline constexpr int maximumNames = 1024;
inline constexpr int maximumExtensions = 64;
struct Attributes {
    quint32 flags = 0;
    quint64 size = 0;
    quint32 uid = 0, gid = 0, permissions = 0, atime = 0, mtime = 0;
    bool hasSize() const { return flags & 0x1; }
    bool hasOwner() const { return flags & 0x2; }
    bool hasPermissions() const { return flags & 0x4; }
    bool hasTimes() const { return flags & 0x8; }
    bool regularFile() const { return hasPermissions() && (permissions & 0170000) == 0100000; }
    bool directory() const { return hasPermissions() && (permissions & 0170000) == 0040000; }
};
struct Entry { QByteArray name; Attributes attributes; };
struct Reply {
    quint8 type = 0;
    quint32 id = 0;
    quint32 version = 0;                              // Version
    QList<std::pair<QByteArray, QByteArray>> extensions; // Version
    quint32 code = 0;                                 // Status
    QString message;                                  // Status
    QByteArray handle, data;                          // Handle, Data, ExtendedReply (data)
    QList<Entry> names;                               // Name
    Attributes attributes;                            // Attrs
};
// Collects bytes from the transport and hands out whole packets (type byte onward).
class Framer {
public:
    void append(const QByteArray& bytes) { buffer_.append(bytes); }
    // The next whole packet, or nothing yet. A length beyond maximumPacket or under one
    // byte is a protocol error: `error` is set and nothing more is read.
    std::optional<QByteArray> next(QString* error);
    qsizetype buffered() const { return buffer_.size(); }
private:
    QByteArray buffer_;
    qsizetype offset_ = 0;
};
// One packet's body as Framer returns it.
std::optional<Reply> parse(const QByteArray& packet, QString* error);
// Requests. Paths and names travel as the bytes the server stores.
QByteArray init();
QByteArray open(quint32 id, const QByteArray& path, quint32 flags);
QByteArray close(quint32 id, const QByteArray& handle);
QByteArray read(quint32 id, const QByteArray& handle, quint64 offset, quint32 length);
QByteArray write(quint32 id, const QByteArray& handle, quint64 offset, const char* data, quint32 length);
QByteArray path(quint8 type, quint32 id, const QByteArray& path);  // Lstat, Stat, Remove, Realpath, Opendir
QByteArray mkdir(quint32 id, const QByteArray& path);
QByteArray readdir(quint32 id, const QByteArray& handle);
QByteArray extended(quint32 id, const QByteArray& name, const QByteArray& payload);
QByteArray string(const QByteArray&);
// statvfs@openssh.com and limits@openssh.com replies (an ExtendedReply's data).
struct FileSystem { quint64 blockSize = 0, fragmentSize = 0, blocks = 0, free = 0, available = 0; };
std::optional<FileSystem> fileSystem(const QByteArray& data);
struct Limits { quint64 packet = 0, read = 0, write = 0, handles = 0; };
std::optional<Limits> limits(const QByteArray& data);
}
