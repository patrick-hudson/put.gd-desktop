#include "PublicUrl.h"
#include <QRegularExpression>
#include <QUrl>
namespace putgd {
bool s3VirtualAddressing(const QJsonObject& settings) {
    const auto mode = settings["addressing"].toString();
    if (mode == "virtual") return true;
    if (mode == "path") return false;
    return s3AmazonEndpoint(settings["endpoint"].toString()) && !settings["bucket"].toString().contains('.');
}
QString s3IdentityEndpoint(const QString& endpoint) {
    QUrl url(endpoint);
    if (!url.isValid() || url.host().isEmpty()) return endpoint;
    url.setScheme(url.scheme().toLower());
    url.setHost(url.host().toLower());
    auto text = url.toString(QUrl::FullyEncoded);
    while (text.endsWith('/')) text.chop(1);
    return text;
}
bool s3AmazonEndpoint(const QString& endpoint) {
    auto host = QUrl(endpoint).host().toLower();
    if (host.endsWith('.')) host.chop(1);  // a fully qualified "host." names the same host
    for (const auto* domain : {"amazonaws.com", "amazonaws.com.cn"})
        if (host == QLatin1String(domain) || host.endsWith('.' + QLatin1String(domain))) return true;
    return false;
}
bool s3FollowsRedirects(const QJsonObject& settings) { return s3AmazonEndpoint(settings["endpoint"].toString()); }
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
    if (!valid) return QStringLiteral("Region takes a region code such as us-east-1: letters, digits and hyphens, without spaces. For Cloudflare R2, enter auto, not the location the Cloudflare dashboard shows.");
    // The SDK reads "fips-" and "-fips" as a request for AWS FIPS endpoints, which it refuses
    // next to the endpoint put.gd always sets.
    // Case-sensitive, as the SDK checks it: "FIPS-lab" is an ordinary label to it.
    if (region.startsWith(QLatin1String("fips-")) || region.endsWith(QLatin1String("-fips")))
        return QStringLiteral("Region takes the plain region code, such as us-east-1, without fips. For FIPS, enter the FIPS endpoint in Endpoint, such as https://s3-fips.us-east-1.amazonaws.com.");
    return {};
}
QString s3RegionProblem(const QString& region, const QString& endpoint) {
    if (auto problem = s3RegionProblem(region); !problem.isEmpty()) return problem;
    // The SDK reads aws-global as AWS's global endpoint and retries a refused request in the region
    // the answer names; no other service is AWS, so it never gets that Region (#1699).
    if (region == QLatin1String("aws-global") && !s3AmazonEndpoint(endpoint))
        return QStringLiteral("aws-global is only for Amazon S3. Enter the region this service gives for the bucket, such as us-east-1.");
    return {};
}
bool s3CloudflareR2Endpoint(const QString& endpoint) {
    auto host = QUrl(endpoint).host();
    if (host.endsWith('.')) host.chop(1);  // a fully qualified "host." names the same host
    return host.endsWith(QLatin1String(".r2.cloudflarestorage.com"), Qt::CaseInsensitive);
}
// The path as sent, still encoded: "%2F" is a character of a name, not a separator.
QString s3EndpointProblem(const QString& endpoint, bool quoteTyped) {
    const QUrl url(endpoint);
    // Without a scheme and host this isn't an address yet ("s3.example.com", or "h" mid-typing):
    // the address check speaks for it, not this one.
    if (url.scheme().isEmpty() || url.host().isEmpty()) return {};
    const auto path = url.path(QUrl::FullyEncoded);
    if (path.isEmpty() || path == QLatin1String("/")) return {};
    if (!quoteTyped)
        return QStringLiteral("Endpoint takes only the service address. Remove the path after the host from Endpoint and put the bucket name in Bucket; a folder goes in Folder in bucket.");
    // The text after the host, exactly as the field shows it.
    const auto authority = endpoint.indexOf(QLatin1String("://"));
    const auto start = authority < 0 ? -1 : endpoint.indexOf('/', authority + 3);
    auto typed = start < 0 ? path : endpoint.mid(start);
    // Only the path: a pasted presigned URL's query or a fragment isn't repeated on screen.
    if (const auto end = typed.indexOf(QRegularExpression(QStringLiteral("[?#]"))); end >= 0) typed.truncate(end);
    return QStringLiteral("Endpoint takes only the service address. Remove \"%1\" from the end of Endpoint and put the bucket name in Bucket; a folder goes in Folder in bucket.").arg(typed);
}
}
