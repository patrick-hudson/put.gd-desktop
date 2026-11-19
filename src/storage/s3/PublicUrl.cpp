#include "PublicUrl.h"
#include <QUrl>
namespace putgd {
bool s3VirtualAddressing(const QJsonObject& settings) {
    const auto mode = settings["addressing"].toString();
    if (mode == "virtual") return true;
    if (mode == "path") return false;
    const auto host = QUrl(settings["endpoint"].toString()).host();
    return host.endsWith("amazonaws.com") && !settings["bucket"].toString().contains('.');
}
QString s3StoredKey(const QString& key) {
    return key.split('/', Qt::SkipEmptyParts).join('/');
}
bool s3KeyAddressable(const QString& key) {
    for (const auto& segment : key.split('/'))
        if (segment == QLatin1String(".") || segment == QLatin1String("..")) return false;
    return true;
}
QString s3ObjectKey(const QJsonObject& settings, const QString& candidate) {
    const auto prefix = s3StoredKey(settings["objectPrefix"].toString());
    return prefix.isEmpty() ? candidate : prefix + '/' + candidate;
}
QString s3LegacyPublicObjectUrl(const QJsonObject& settings, const QString& key) {
    QUrl base(settings["publicUrlBase"].isString() ? settings["publicUrlBase"].toString()
                                                    : settings["endpoint"].toString());
    QString encoded = base.toString(QUrl::FullyEncoded);
    while (encoded.endsWith('/')) encoded.chop(1);
    if (!settings["publicUrlBase"].isString()) {
        if (s3VirtualAddressing(settings)) {
            base.setHost(settings["bucket"].toString() + "." + base.host());
            encoded = base.toString(QUrl::FullyEncoded);
            while (encoded.endsWith('/')) encoded.chop(1);
        } else {
            encoded += "/" + QString::fromLatin1(QUrl::toPercentEncoding(settings["bucket"].toString()));
        }
    }
    QStringList segments;
    for (const auto& segment : key.split('/'))
        segments.append(QString::fromLatin1(QUrl::toPercentEncoding(segment)));
    return encoded + "/" + segments.join('/');
}
QString publicObjectUrl(const QJsonObject& settings, const QString& key) {
    return s3LegacyPublicObjectUrl(settings, s3StoredKey(key));
}
QString s3RegionProblem(const QString& region) {
    if (region.isEmpty()) return QStringLiteral("Enter the bucket's region, such as us-east-1. For Cloudflare R2, enter auto.");
    // Aws::Utils::IsValidDnsLabel: 1-63 ASCII letters, digits and hyphens,
    // starting and ending with a letter or digit.
    const auto alnum = [](QChar c) { return c.unicode() < 128 && c.isLetterOrNumber(); };
    bool valid = region.size() <= 63 && alnum(region.front()) && alnum(region.back());
    for (const auto c : region) valid = valid && (alnum(c) || c == '-');
    return valid ? QString() : QStringLiteral("Region takes a region code such as us-east-1: letters, digits and hyphens, without spaces. For Cloudflare R2, enter auto, not the location the Cloudflare dashboard shows.");
}
bool s3CloudflareR2Endpoint(const QString& endpoint) {
    return QUrl(endpoint).host().endsWith(QLatin1String(".r2.cloudflarestorage.com"), Qt::CaseInsensitive);
}
}
