#include "HelperFiles.h"
#include <QCryptographicHash>
#include <algorithm>
#include <memory>
namespace putgd::helperfiles {
#ifdef Q_OS_UNIX
QJsonObject descriptor(const QString& path, const struct stat& s, const QString& sha256) {
    return {{"path", path}, {"device", QString::number(qulonglong(s.st_dev))}, {"inode", QString::number(qulonglong(s.st_ino))},
            {"bytes", QString::number(s.st_size)}, {"sha256", sha256}};
}
#endif
QString digest(QFile& file, qint64 size, const std::function<bool()>& stop) {
    if (!file.seek(0)) return {};
    QCryptographicHash hash(QCryptographicHash::Sha256);
    qint64 total = 0;
    while (total < size) {
        if (stop && stop()) return {};
        const auto chunk = file.read(std::min<qint64>(1024 * 1024, size - total));
        if (chunk.isEmpty()) return {};
        hash.addData(chunk);
        total += chunk.size();
    }
    return QString::fromLatin1(hash.result().toHex());
}
void captureStandardError(QProcess& process, bool& flooded, std::function<void(const QByteArray&)> sink) {
    auto bytes = std::make_shared<qint64>(0);
    QObject::connect(&process, &QProcess::readyReadStandardError, &process, [&process, &flooded, sink = std::move(sink), bytes] {
        process.setReadChannel(QProcess::StandardError);
        while (process.bytesAvailable() > 0) {
            const auto chunk = process.read(16384);
            *bytes += chunk.size();
            if (sink) sink(chunk);
            if (*bytes > 1024 * 1024) {
                flooded = true;
                break;
            }
        }
        process.setReadChannel(QProcess::StandardOutput);
    });
}
}
