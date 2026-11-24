#pragma once
#include <QByteArray>
#include <QFile>
#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <functional>
#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif
namespace putgd::helperfiles {
// What the resident shares with a putgd-ui helper about the private files it
// staged for one session (#1445), for the editor and for image jobs.
#ifdef Q_OS_UNIX
// {path, device, inode, bytes, sha256}, the numbers as decimal strings.
QJsonObject descriptor(const QString& path, const struct stat&, const QString& sha256 = {});
#endif
// The SHA-256 of the first size bytes from the start of file, or empty when they
// cannot all be read or stop() asks to stop (checked before each MiB).
QString digest(QFile& file, qint64 size, const std::function<bool()>& stop = {});
// Reads the helper's standard error as it arrives and hands each chunk to sink
// when there is one. More than 1 MiB in all sets flooded and stops reading.
void captureStandardError(QProcess& process, bool& flooded, std::function<void(const QByteArray&)> sink = {});
}
