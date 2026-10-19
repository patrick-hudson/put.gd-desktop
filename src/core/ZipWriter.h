#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QString>
namespace putgd {
// A small in-memory .zip writer for diagnostics packages (#74 §3): deflate
// through qCompress (Qt's own zlib), stored when that does not shrink a file.
// No ZIP64: entries and the archive stay far below 4 GiB.
class ZipWriter {
public:
 explicit ZipWriter(QDateTime time=QDateTime::currentDateTimeUtc()):time_(std::move(time)) {}
 void add(const QString& name,const QByteArray& data);
 // The complete archive; add nothing after calling it.
 QByteArray finish();
 static quint32 crc32(const QByteArray&);
private:
 QDateTime time_;
 QByteArray body_,directory_;
 quint16 entries_=0;
};
}
