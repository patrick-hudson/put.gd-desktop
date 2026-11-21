#pragma once
#include <QByteArray>
#include <QDeadlineTimer>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <functional>
#include <vector>
class QFile;
class QNetworkAccessManager;
namespace putgd::zsync {
// put.gd's own zsync client (#1766), for the --install-update child. It reads
// a release's .zsync (zsync 0.6.2's control file, as zsyncmake writes it),
// copies the blocks of the new image it finds in the running image, at any
// offset, and fetches the rest from the image's URL with HTTP Range
// requests. The caller checks the assembled file's SHA-1, then the release's
// own checks; any failure here is a reason to download the whole file.
struct Control {
 QString filename,sha1;
 qint64 length=0,blocks=0;
 int blocksize=0,seqMatches=0,rsumBytes=0,checksumBytes=0;
 // Per block, as stored: the trailing rsumBytes of the big-endian rolling
 // checksum (a then b), then the leading checksumBytes of the block's MD4.
 QByteArray records;
};
// Parses a control file; on failure reason is set (bad_header,
// unsupported_hash_lengths, unsupported_header, bad_records). A recompressed
// target (Recompress, Z-*) is unsupported_header.
bool parse(const QByteArray& data,Control&,QString& reason);
// One run of missing bytes, [start, end).
struct Range {qint64 start=0,end=0;};
struct Assembly {
 // Which target blocks the seed supplied.
 std::vector<char> known;
 qint64 reusedBytes=0;
};
// Scans seed for the target's blocks and writes each one found into output
// (already sized to control.length) at its place. progress gets the seed bytes
// read and the seed's size. False with reason on a read or write failure
// (seed_unreadable, write_failed), or timeout once deadline has passed.
bool reuse(const Control&,QFile& seed,QFile& output,Assembly&,const QDeadlineTimer& deadline,const std::function<void(qint64,qint64)>& progress,QString& reason);
// The blocks the seed didn't have, merged into runs, the last clipped to the length.
std::vector<Range> missing(const Control&,const Assembly&);
struct Fetch {
 qint64 downloadedBytes=0;
 int requests=0;
};
// Fetches ranges from url into output with Range requests of at most
// maximumRanges runs each, a few at a time, then checks each fetched block
// against its MD4. False with reason (range_refused when the server answers
// with the whole file, range_failed, range_short, range_invalid,
// range_too_large, range_network, stalled, range_checksum, write_failed, or
// timeout once deadline has passed).
bool fetch(QNetworkAccessManager&,const QUrl& url,const Control&,const std::vector<Range>&,QFile& output,Fetch&,
           const QDeadlineTimer& deadline,const std::function<void(qint64)>& progress,QString& reason,QJsonObject& details);
// The output's SHA-1 as lowercase hex, empty when it can't be read.
QString sha1(const QString& path);
}
