#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QString>
#include <QtGlobal>
namespace putgd::b2 {
// Backblaze B2 native API (b2api v3) destination rules shared by the resident and
// putgd-b2-worker (#282). Qt Core only: no network types, so the resident links none.
inline constexpr qint64 maximumFileBytes = 10'000'000'000'000LL; // B2: 10 TB per file
inline constexpr qint64 minimumPartBytes = 5'000'000;              // absoluteMinimumPartSize
inline constexpr qint64 maximumPartBytes = 5'000'000'000LL;
inline constexpr qint64 maximumParts = 10000;
inline constexpr int maximumNameBytes = 1024;                      // UTF-8 bytes per file name
inline constexpr int maximumLinkDays = 7;                          // b2_get_download_authorization limit
// Files above the threshold use b2_start_large_file and parts; smaller files one
// b2_upload_file. Tests may lower both; the worker still refuses parts under 5 MB.
struct UploadPolicy {
    qint64 largeFileThresholdBytes = 64LL * 1024 * 1024;
    qint64 partBytes = 16LL * 1024 * 1024;
};
// Part size for a file, or zero for a single upload. It grows in whole MiB once
// 10,000 parts of the policy size would not hold the file.
qint64 partBytes(qint64 bytes, const UploadPolicy& = {});
qint64 partCount(qint64 bytes, qint64 partBytes);
bool validPlan(qint64 bytes, qint64 partBytes);
QJsonObject defaults();
Error validateSettings(const QJsonObject&);
// prefix + candidate, with one separator.
QString fileName(const QJsonObject& settings, const QString& candidate);
// B2 file names travel percent-encoded in headers and URL paths; '/' stays as is.
QString encodeName(const QString&);
// The link for a published file: publicUrlBase + name, or the account's download
// URL + /file/<bucket>/<name>. Authorized links append ?Authorization=<token>.
QString fileUrl(const QJsonObject& settings, const QString& downloadUrl, const QString& name, const QString& token = {});
}
