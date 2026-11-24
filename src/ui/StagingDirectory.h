#pragma once
#include <QtGlobal>
#ifdef Q_OS_UNIX
#include <QFile>
#include <QJsonObject>
#include <QString>
#include <sys/stat.h>
namespace putgd::staging {
// The checks putgd-ui runs on a private staging directory the resident made for
// one helper session (#1445): the image job's pgi-XXXXXX and the editor's
// pge-XXXXXX. Each caller keeps its own file names and roles.
//
// A staged file is described by {path, sha256, device, inode, bytes}, the numbers
// as canonical decimal strings.
bool number(const QJsonValue&, qint64&);
bool hashText(const QString&);
// A regular file of this user with exactly this mode and one name.
bool privateFile(const struct stat&, mode_t mode);
bool sameFile(const struct stat&, const struct stat&);
// The same file with the same size and modification time: the input as staged.
bool sameInput(const struct stat&, const struct stat&);
// Reads size bytes from the start of file and compares their SHA-256; leaves it at the start.
bool hashMatches(QFile& file, qint64 size, const QString& sha256);

class Directory final {
public:
    Directory() = default;
    Directory(const Directory&) = delete;
    Directory& operator=(const Directory&) = delete;
    ~Directory();
    // Pins the directory by descriptor: owned by this user, mode 0700, not a link.
    bool open(const QString& path);
    // Opens name in the pinned directory without following a link. A 0400 file
    // opens read-only, anything else read-write; its identity must match the
    // descriptor and the mode, and identity receives what it was opened as.
    bool openFile(QFile&, const QJsonObject& descriptor, const char* name, mode_t mode, struct stat& identity) const;
    // The path still names the pinned directory.
    bool intact() const;
    // The open input and its name are still the staged input, unchanged.
    bool inputIntact(const QFile&, const char* name, const struct stat& identity) const;
    // The open output and its name are still the staged output; it may have been written.
    bool outputIntact(const QFile&, const char* name, const struct stat& identity) const;
    int descriptor() const { return fd_; }
    const QString& path() const { return path_; }
private:
    bool entryIntact(const QFile&, const char* name, const struct stat& identity, mode_t mode, bool unchanged) const;
    int fd_ = -1;
    struct stat identity_{};
    QString path_;
};
}
#endif
