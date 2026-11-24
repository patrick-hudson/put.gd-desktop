#include "StagingDirectory.h"
#ifdef Q_OS_UNIX
#include <QCryptographicHash>
#include <QJsonValue>
#include <algorithm>
#include <fcntl.h>
#include <unistd.h>
namespace putgd::staging {
namespace {
bool validDirectory(const struct stat& s) { return S_ISDIR(s.st_mode) && s.st_uid == getuid() && (s.st_mode & 0777) == 0700; }
bool sameModificationTime(const struct stat& a, const struct stat& b) {
#ifdef Q_OS_DARWIN
    return a.st_mtimespec.tv_sec == b.st_mtimespec.tv_sec && a.st_mtimespec.tv_nsec == b.st_mtimespec.tv_nsec;
#else
    return a.st_mtim.tv_sec == b.st_mtim.tv_sec && a.st_mtim.tv_nsec == b.st_mtim.tv_nsec;
#endif
}
}
bool number(const QJsonValue& value, qint64& result) {
    bool ok = false;
    if (!value.isString()) return false;
    result = value.toString().toLongLong(&ok);
    return ok && result >= 0 && QString::number(result) == value.toString();
}
bool hashText(const QString& value) { return value.size() == 64 && QByteArray::fromHex(value.toLatin1()).toHex() == value.toLatin1(); }
bool privateFile(const struct stat& s, mode_t mode) { return S_ISREG(s.st_mode) && s.st_uid == getuid() && (s.st_mode & 0777) == mode && s.st_nlink == 1; }
bool sameFile(const struct stat& a, const struct stat& b) { return a.st_dev == b.st_dev && a.st_ino == b.st_ino; }
bool sameInput(const struct stat& a, const struct stat& b) { return sameFile(a, b) && a.st_size == b.st_size && sameModificationTime(a, b); }
bool hashMatches(QFile& file, qint64 size, const QString& sha256) {
    if (!file.seek(0)) return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (total < size) {
        const auto chunk = file.read(std::min<qint64>(1024 * 1024, size - total));
        if (chunk.isEmpty()) return false;
        hash.addData(chunk);
        total += chunk.size();
    }
    return QString::fromLatin1(hash.result().toHex()) == sha256 && file.seek(0);
}
Directory::~Directory() {
    if (fd_ >= 0) ::close(fd_);
}
bool Directory::open(const QString& path) {
    if (fd_ >= 0) return false;
    path_ = path;
    fd_ = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_DIRECTORY);
    return fd_ >= 0 && !::fstat(fd_, &identity_) && validDirectory(identity_);
}
bool Directory::openFile(QFile& file, const QJsonObject& object, const char* name, mode_t mode, struct stat& identity) const {
    qint64 device = 0, inode = 0, size = 0;
    if (fd_ < 0 || object.size() != 5 || !object["path"].isString() || !object["sha256"].isString() || !number(object["device"], device) ||
        !number(object["inode"], inode) || !number(object["bytes"], size)) return false;
    const bool readOnly = mode == 0400;
    const int fd = ::openat(fd_, name, (readOnly ? O_RDONLY : O_RDWR) | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    if (::fstat(fd, &identity) || !privateFile(identity, mode) || qulonglong(identity.st_dev) != qulonglong(device) ||
        qulonglong(identity.st_ino) != qulonglong(inode) || identity.st_size != size) {
        ::close(fd);
        return false;
    }
    if (!file.open(fd, (readOnly ? QIODevice::ReadOnly : QIODevice::ReadWrite) | QIODevice::Unbuffered, QFileDevice::AutoCloseHandle)) {
        ::close(fd);
        return false;
    }
    return true;
}
bool Directory::intact() const {
    struct stat now{};
    return fd_ >= 0 && !::lstat(QFile::encodeName(path_).constData(), &now) && sameFile(identity_, now) && validDirectory(now);
}
bool Directory::entryIntact(const QFile& file, const char* name, const struct stat& identity, mode_t mode, bool unchanged) const {
    struct stat open{}, named{};
    const auto same = unchanged ? sameInput : sameFile;
    return !::fstat(file.handle(), &open) && same(identity, open) && privateFile(open, mode) &&
           !::fstatat(fd_, name, &named, AT_SYMLINK_NOFOLLOW) && same(identity, named) && privateFile(named, mode);
}
bool Directory::inputIntact(const QFile& file, const char* name, const struct stat& identity) const {
    return entryIntact(file, name, identity, 0400, true);
}
bool Directory::outputIntact(const QFile& file, const char* name, const struct stat& identity) const {
    return entryIntact(file, name, identity, 0600, false);
}
}
#endif
