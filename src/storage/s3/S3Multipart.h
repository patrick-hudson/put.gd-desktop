#pragma once
#include <QJsonObject>
#include <QtGlobal>
namespace putgd {
// S3 multipart rules: parts of 5 MiB to 5 GiB (only the last may be smaller),
// at most 10,000 parts, and objects of at most 5 TiB. Qt-only; no AWS types.
inline constexpr qint64 s3MinimumPartBytes = 5LL * 1024 * 1024;
inline constexpr qint64 s3MaximumPartBytes = 5LL * 1024 * 1024 * 1024;
inline constexpr qint64 s3MaximumParts = 10000;
inline constexpr qint64 s3MaximumObjectBytes = 5LL * 1024 * 1024 * 1024 * 1024;
// Files above the threshold upload in parts streamed from the retained artifact;
// smaller files keep one PutObject. Production uses these defaults. Tests may lower
// both, but the worker still refuses parts below S3's 5 MiB minimum.
struct S3UploadPolicy {
    qint64 multipartThresholdBytes = 64LL * 1024 * 1024;
    qint64 minimumPartBytes = 16LL * 1024 * 1024;
};
// Part size for a file, or zero for a single PutObject. The size grows in whole
// MiB once 10,000 minimum-size parts would not hold the file.
inline qint64 s3PartBytes(qint64 bytes, const S3UploadPolicy& policy = {}) {
    if (bytes <= policy.multipartThresholdBytes || bytes > s3MaximumObjectBytes) return 0;
    constexpr qint64 mib = 1024 * 1024;
    const qint64 spread = ((bytes + s3MaximumParts - 1) / s3MaximumParts + mib - 1) / mib * mib;
    return qMax(qMax(policy.minimumPartBytes, s3MinimumPartBytes), spread);
}
inline qint64 s3PartCount(qint64 bytes, qint64 partBytes) {
    return partBytes > 0 ? qMax<qint64>(1, (bytes + partBytes - 1) / partBytes) : 0;
}
inline bool s3ValidPlan(qint64 bytes, qint64 partBytes) {
    return bytes > 0 && bytes <= s3MaximumObjectBytes && partBytes >= s3MinimumPartBytes &&
           partBytes <= s3MaximumPartBytes && s3PartCount(bytes, partBytes) <= s3MaximumParts;
}
}
