#include "SftpWire.h"
#include <QtEndian>
namespace putgd::sftp::wire {
namespace {
class Reader {
public:
    explicit Reader(const QByteArray& bytes) : bytes_(bytes) {}
    bool ok() const { return ok_; }
    bool atEnd() const { return at_ == bytes_.size(); }
    quint8 u8() {
        if (!need(1)) return 0;
        return quint8(bytes_[at_++]);
    }
    quint32 u32() {
        if (!need(4)) return 0;
        const auto value = qFromBigEndian<quint32>(bytes_.constData() + at_);
        at_ += 4;
        return value;
    }
    quint64 u64() {
        if (!need(8)) return 0;
        const auto value = qFromBigEndian<quint64>(bytes_.constData() + at_);
        at_ += 8;
        return value;
    }
    QByteArray string(quint32 bound) {
        const auto length = u32();
        if (!ok_ || length > bound || !need(length)) { ok_ = false; return {}; }
        const auto value = bytes_.mid(at_, qsizetype(length));
        at_ += qsizetype(length);
        return value;
    }
    Attributes attributes() {
        Attributes a;
        a.flags = u32();
        if (a.flags & 0x1) a.size = u64();
        if (a.flags & 0x2) { a.uid = u32(); a.gid = u32(); }
        if (a.flags & 0x4) a.permissions = u32();
        if (a.flags & 0x8) { a.atime = u32(); a.mtime = u32(); }
        if (a.flags & 0x80000000u) {
            const auto count = u32();
            if (count > quint32(maximumExtensions)) { ok_ = false; return a; }
            for (quint32 i = 0; i < count && ok_; ++i) { string(maximumPacket); string(maximumPacket); }
        }
        return a;
    }
private:
    bool need(quint64 count) {
        if (!ok_ || quint64(bytes_.size() - at_) < count) { ok_ = false; return false; }
        return true;
    }
    const QByteArray& bytes_;
    qsizetype at_ = 0;
    bool ok_ = true;
};
void putU32(QByteArray& out, quint32 value) {
    char b[4];
    qToBigEndian(value, b);
    out.append(b, 4);
}
void putU64(QByteArray& out, quint64 value) {
    char b[8];
    qToBigEndian(value, b);
    out.append(b, 8);
}
QByteArray packet(quint8 type, const QByteArray& body) {
    QByteArray out;
    out.reserve(body.size() + 5);
    putU32(out, quint32(body.size() + 1));
    out.append(char(type));
    out.append(body);
    return out;
}
QByteArray withId(quint32 id) {
    QByteArray body;
    putU32(body, id);
    return body;
}
}
std::optional<QByteArray> Framer::next(QString* error) {
    if (buffer_.size() - offset_ < 4) return std::nullopt;
    const auto length = qFromBigEndian<quint32>(buffer_.constData() + offset_);
    if (length < 1 || length > maximumPacket) {
        if (error) *error = QStringLiteral("The server sent an SFTP packet of %1 bytes").arg(length);
        buffer_.clear();
        offset_ = 0;
        return std::nullopt;
    }
    if (buffer_.size() - offset_ - 4 < qsizetype(length)) return std::nullopt;
    auto body = buffer_.mid(offset_ + 4, qsizetype(length));
    offset_ += 4 + qsizetype(length);
    // Drop what was read once it is most of the buffer, so a long upload's replies don't pile up.
    if (offset_ > 65536 && offset_ * 2 > buffer_.size()) { buffer_.remove(0, offset_); offset_ = 0; }
    return body;
}
std::optional<Reply> parse(const QByteArray& packet, QString* error) {
    Reader in(packet);
    Reply reply;
    reply.type = in.u8();
    const auto bad = [&](const char* what) -> std::optional<Reply> {
        if (error) *error = QStringLiteral("The server sent a malformed SFTP %1").arg(QLatin1String(what));
        return std::nullopt;
    };
    if (!in.ok()) return bad("packet");
    switch (reply.type) {
    case Version: {
        reply.version = in.u32();
        while (in.ok() && !in.atEnd()) {
            if (reply.extensions.size() >= maximumExtensions) return bad("version");
            auto name = in.string(256);
            auto data = in.string(4096);
            if (in.ok()) reply.extensions.append({name, data});
        }
        if (!in.ok()) return bad("version");
        return reply;
    }
    case Status: {
        reply.id = in.u32();
        reply.code = in.u32();
        if (!in.ok()) return bad("status");
        // Version 3 servers may end a status after its code; the message and language are optional.
        if (!in.atEnd()) {
            reply.message = QString::fromUtf8(in.string(4096));
            if (!in.atEnd()) in.string(256);
        }
        if (!in.ok()) return bad("status");
        return reply;
    }
    case Handle:
        reply.id = in.u32();
        reply.handle = in.string(256);
        if (!in.ok() || reply.handle.isEmpty()) return bad("handle");
        return reply;
    case Data:
    case ExtendedReply:
        reply.id = in.u32();
        if (reply.type == Data) reply.data = in.string(maximumPacket);
        else if (in.ok()) reply.data = packet.mid(5);
        if (!in.ok()) return bad("data");
        return reply;
    case Name: {
        reply.id = in.u32();
        const auto count = in.u32();
        if (!in.ok() || count > quint32(maximumNames)) return bad("name list");
        for (quint32 i = 0; i < count; ++i) {
            Entry entry;
            entry.name = in.string(4096);
            in.string(8192);  // the long name, for display by ls-like clients only
            entry.attributes = in.attributes();
            if (!in.ok()) return bad("name list");
            reply.names.append(std::move(entry));
        }
        return reply;
    }
    case Attrs:
        reply.id = in.u32();
        reply.attributes = in.attributes();
        if (!in.ok()) return bad("attributes");
        return reply;
    default:
        return bad("reply");
    }
}
QByteArray string(const QByteArray& value) {
    QByteArray out;
    putU32(out, quint32(value.size()));
    out.append(value);
    return out;
}
QByteArray init() {
    QByteArray body;
    putU32(body, 3);
    return packet(Init, body);
}
QByteArray open(quint32 id, const QByteArray& file, quint32 flags) {
    auto body = withId(id);
    body.append(string(file));
    putU32(body, flags);
    putU32(body, 0);  // no attributes: the server's umask decides the mode
    return packet(Open, body);
}
QByteArray close(quint32 id, const QByteArray& handle) { return packet(Close, withId(id) + string(handle)); }
QByteArray read(quint32 id, const QByteArray& handle, quint64 offset, quint32 length) {
    auto body = withId(id) + string(handle);
    putU64(body, offset);
    putU32(body, length);
    return packet(Read, body);
}
QByteArray write(quint32 id, const QByteArray& handle, quint64 offset, const char* data, quint32 length) {
    QByteArray body;
    body.reserve(qsizetype(length) + handle.size() + 20);
    putU32(body, id);
    body.append(string(handle));
    putU64(body, offset);
    putU32(body, length);
    body.append(data, qsizetype(length));
    return packet(Write, body);
}
QByteArray path(quint8 type, quint32 id, const QByteArray& file) { return packet(type, withId(id) + string(file)); }
QByteArray mkdir(quint32 id, const QByteArray& file) {
    auto body = withId(id) + string(file);
    putU32(body, 0);
    return packet(Mkdir, body);
}
QByteArray readdir(quint32 id, const QByteArray& handle) { return packet(Readdir, withId(id) + string(handle)); }
QByteArray extended(quint32 id, const QByteArray& name, const QByteArray& payload) { return packet(Extended, withId(id) + string(name) + payload); }
std::optional<FileSystem> fileSystem(const QByteArray& data) {
    Reader in(data);
    FileSystem fs;
    fs.blockSize = in.u64();
    fs.fragmentSize = in.u64();
    fs.blocks = in.u64();
    fs.free = in.u64();
    fs.available = in.u64();
    if (!in.ok()) return std::nullopt;
    return fs;
}
std::optional<Limits> limits(const QByteArray& data) {
    Reader in(data);
    Limits l;
    l.packet = in.u64();
    l.read = in.u64();
    l.write = in.u64();
    l.handles = in.u64();
    if (!in.ok()) return std::nullopt;
    return l;
}
}
