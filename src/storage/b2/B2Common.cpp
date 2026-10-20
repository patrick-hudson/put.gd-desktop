#include "B2Common.h"
#include <QRegularExpression>
#include <QStringList>
#include <QUrl>
namespace putgd::b2 {
qint64 partBytes(qint64 bytes, const UploadPolicy& policy) {
    if (bytes <= policy.largeFileThresholdBytes || bytes > maximumFileBytes) return 0;
    constexpr qint64 mib = 1024 * 1024;
    const qint64 spread = ((bytes + maximumParts - 1) / maximumParts + mib - 1) / mib * mib;
    return qMax(qMax(policy.partBytes, minimumPartBytes), spread);
}
qint64 partCount(qint64 bytes, qint64 part) { return part > 0 ? qMax<qint64>(1, (bytes + part - 1) / part) : 0; }
bool validPlan(qint64 bytes, qint64 part) {
    return bytes > 0 && bytes <= maximumFileBytes && part >= minimumPartBytes && part <= maximumPartBytes && partCount(bytes, part) <= maximumParts;
}
QJsonObject defaults() {
    return {{"apiUrl", "https://api.backblazeb2.com"}, {"bucket", ""}, {"objectPrefix", ""}, {"linkMode", "public"},
            {"linkValidityDays", maximumLinkDays}, {"publicUrlBase", QJsonValue::Null}, {"connectTimeoutMs", 10000},
            {"requestTimeoutMs", 600000}};
}
namespace {
// HTTPS without user information, query or fragment; plain HTTP only for a literal
// loopback host, which the offline fixture uses.
bool serviceUrl(const QJsonValue& value) {
    if (!value.isString()) return false;
    const QUrl url(value.toString(), QUrl::StrictMode);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment()) return false;
    if (url.scheme() == "https") return true;
    return url.scheme() == "http" && QStringList{"127.0.0.1", "::1", "localhost"}.contains(url.host());
}
bool integer(const QJsonValue& value, double low, double high) {
    const auto number = value.toDouble(-1);
    return value.isDouble() && number >= low && number <= high && number == double(qint64(number));
}
}
Error validateSettings(const QJsonObject& s) {
    const auto bad = [](const QString& what) { return Error{"b2_settings_invalid", "Invalid Backblaze B2 settings: " + what, {}}; };
    if (QStringList(s.keys()) != QStringList(defaults().keys())) return bad("missing or unsupported fields");
    static const QRegularExpression bucket("^[A-Za-z0-9-]{6,63}$");
    const auto name = s["bucket"].toString();
    if (!s["bucket"].isString() || !bucket.match(name).hasMatch() || name.startsWith("b2-", Qt::CaseInsensitive))
        return bad("enter the bucket name (6 to 63 letters, digits or hyphens)");
    const auto prefix = s["objectPrefix"].toString();
    static const QRegularExpression control("[\\x00-\\x1f\\x7f\\\\]");
    if (!s["objectPrefix"].isString() || prefix.toUtf8().size() > 900 || prefix.startsWith('/') || prefix.contains(control) || prefix.contains("//"))
        return bad("the folder in the bucket is too long or has unsupported characters");
    if (!serviceUrl(s["apiUrl"])) return bad("the API address must be an HTTPS URL");
    const auto mode = s["linkMode"].toString();
    if (!QStringList{"public", "authorized"}.contains(mode)) return bad("links are public or authorized");
    if (!integer(s["linkValidityDays"], 1, maximumLinkDays)) return bad("authorized links last 1 to 7 days");
    if (!s["publicUrlBase"].isNull() && (!serviceUrl(s["publicUrlBase"]) || mode != "public"))
        return bad("a link domain must be an HTTPS URL and applies to public buckets only");
    if (!integer(s["connectTimeoutMs"], 1000, 120000) || !integer(s["requestTimeoutMs"], 1000, 3600000))
        return bad("timeouts must be whole milliseconds within their limits");
    return {};
}
QString fileName(const QJsonObject& settings, const QString& candidate) {
    auto prefix = settings["objectPrefix"].toString();
    if (!prefix.isEmpty() && !prefix.endsWith('/')) prefix += '/';
    return prefix + candidate;
}
QString encodeName(const QString& name) { return QString::fromLatin1(QUrl::toPercentEncoding(name, "/")); }
QString fileUrl(const QJsonObject& settings, const QString& downloadUrl, const QString& name, const QString& token) {
    auto base = settings["publicUrlBase"].isString() ? settings["publicUrlBase"].toString()
                                                     : downloadUrl + "/file/" + QString::fromLatin1(QUrl::toPercentEncoding(settings["bucket"].toString()));
    while (base.endsWith('/')) base.chop(1);
    auto url = base + "/" + encodeName(name);
    if (!token.isEmpty()) url += "?Authorization=" + QString::fromLatin1(QUrl::toPercentEncoding(token));
    return url;
}
}
