#include "R2Settings.h"
#include <QJsonArray>
#include <QRegularExpression>
#include "PublicUrl.h"
#include <QUrl>
namespace putgd::r2 {
namespace {
// Whole-string matches (\A...\z): "^...$" would also accept a trailing newline, and a saved
// account or bucket ending in one would build another host or key.
QRegularExpression whole(const char* pattern) {return QRegularExpression(QRegularExpression::anchoredPattern(QString::fromLatin1(pattern)));}
const QRegularExpression& accountPattern() {static const QRegularExpression value = whole("[0-9a-f]{32}");return value;}
const QRegularExpression& bucketPattern() {static const QRegularExpression value = whole("[a-z0-9][a-z0-9-]{1,61}[a-z0-9]");return value;}
// The account endpoint host: ACCOUNT, an optional jurisdiction, then r2.cloudflarestorage.com.
const QRegularExpression& hostPattern() {static const QRegularExpression value = whole("([0-9a-f]{32})(?:\\.(eu|fedramp))?\\.r2\\.cloudflarestorage\\.com");return value;}
Error fieldError(const QString& field, const QString& message) {return {"s3_settings_invalid", message, {{"field", field}}};}
std::optional<std::pair<QString, QString>> accountHost(const QUrl& url) {
    if (!url.isValid() || url.scheme() != QLatin1String("https") || !url.userInfo().isEmpty() || url.port() != -1 || url.hasQuery() || url.hasFragment()) return {};
    const auto match = hostPattern().match(url.host().toLower());
    if (!match.hasMatch()) return {};
    return std::pair{match.captured(1), match.captured(2).isEmpty() ? QStringLiteral("default") : match.captured(2)};
}
}
const QStringList& settingKeys() {
    static const QStringList keys{"accountId", "jurisdiction", "bucket", "objectPrefix", "linkMode", "linkValidityDays", "publicUrlBase",
                                  "connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"};
    return keys;
}
QJsonObject defaults() {
    return {{"accountId", ""}, {"jurisdiction", "default"}, {"bucket", ""}, {"objectPrefix", ""}, {"linkMode", "presigned"}, {"linkValidityDays", 7},
            {"publicUrlBase", QJsonValue::Null}, {"connectTimeoutMs", 5000}, {"requestTimeoutMs", 60000}, {"lowSpeedBytesPerSecond", 1}, {"lowSpeedTimeSeconds", 30}};
}
bool accountIdValid(const QString& accountId) {return accountPattern().match(accountId).hasMatch();}
bool jurisdictionValid(const QString& jurisdiction) {return jurisdiction == QLatin1String("default") || jurisdiction == QLatin1String("eu") || jurisdiction == QLatin1String("fedramp");}
bool bucketValid(const QString& bucket) {return bucketPattern().match(bucket).hasMatch();}
QString endpoint(const QString& accountId, const QString& jurisdiction) {
    return "https://" + accountId + (jurisdiction == QLatin1String("default") ? QString() : "." + jurisdiction) + ".r2.cloudflarestorage.com";
}
QJsonObject s3Settings(const QJsonObject& settings) {
    QJsonObject s3{{"bucket", settings["bucket"]}, {"region", "auto"}, {"endpoint", endpoint(settings["accountId"].toString(), settings["jurisdiction"].toString())},
                   {"objectPrefix", settings["objectPrefix"]}, {"storageClass", "STANDARD"}, {"aclMode", "omit"}, {"publicUrlBase", settings["publicUrlBase"]},
                   {"addressing", "path"}, {"linkMode", settings["linkMode"]}, {"linkValidityDays", settings["linkValidityDays"]}};
    for (const auto& key : {"connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"}) s3[key] = settings[key];
    return s3;
}
std::optional<S3ApiAddress> parseS3ApiAddress(const QString& text) {
    // The raw text, never a decoded URL: Qt would turn %2D into "-" and the like, so anything
    // percent-encoded is refused rather than read as something it does not say.
    static const QRegularExpression address(QRegularExpression::anchoredPattern(QStringLiteral(
        "https://([0-9A-Fa-f]{32})(?:\\.(eu|fedramp))?\\.r2\\.cloudflarestorage\\.com(?:/([^/?#]*))?/?")), QRegularExpression::CaseInsensitiveOption);
    const auto trimmed = text.trimmed();
    if (trimmed.contains('%')) return {};
    const auto match = address.match(trimmed);
    if (!match.hasMatch()) return {};
    const auto bucket = match.captured(3);
    if (!bucket.isEmpty() && !bucketValid(bucket)) return {};
    return S3ApiAddress{match.captured(1).toLower(), match.captured(2).isEmpty() ? QStringLiteral("default") : match.captured(2).toLower(), bucket};
}
std::optional<QJsonObject> fromS3Settings(const QJsonObject& s3) {
    const QUrl url(s3["endpoint"].toString(), QUrl::StrictMode);
    const auto host = accountHost(url);
    const auto path = url.path();
    if (!host || !(path.isEmpty() || path == QLatin1String("/")) || !bucketValid(s3["bucket"].toString()) || s3["storageClass"].toString() != QLatin1String("STANDARD")) return {};
    QJsonObject r2{{"accountId", host->first}, {"jurisdiction", host->second}, {"bucket", s3["bucket"]}, {"objectPrefix", s3["objectPrefix"]},
                   {"linkMode", s3["linkMode"].toString("public")}, {"linkValidityDays", s3["linkValidityDays"].toInt(7)},
                   {"publicUrlBase", s3["publicUrlBase"].isString() ? s3["publicUrlBase"] : QJsonValue(QJsonValue::Null)}};
    for (const auto& key : {"connectTimeoutMs", "requestTimeoutMs", "lowSpeedBytesPerSecond", "lowSpeedTimeSeconds"}) r2[key] = s3[key];
    return r2;
}
QString problem(const QJsonObject& settings, const QString& field) {
    const auto value = settings[field];
    if (field == QLatin1String("accountId")) {
        if (value.toString().isEmpty()) return QStringLiteral("Enter the Account ID to finish setting up this destination. It is in R2 overview → Account details.");
        if (!value.isString() || !accountIdValid(value.toString()))
            return QStringLiteral("Enter the Account ID: 32 characters, digits and letters a to f, from R2 overview → Account details.");
    } else if (field == QLatin1String("jurisdiction")) {
        if (!value.isString() || !jurisdictionValid(value.toString())) return QStringLiteral("Choose the bucket's jurisdiction: Default, EU or FedRAMP.");
    } else if (field == QLatin1String("bucket")) {
        if (value.toString().isEmpty()) return QStringLiteral("Enter a bucket name to finish setting up this destination.");
        if (!value.isString() || !bucketValid(value.toString()))
            return QStringLiteral("R2 bucket names have 3–63 lowercase letters, digits and hyphens, and start and end with a letter or digit.");
    } else if (field == QLatin1String("publicUrlBase") && !value.isNull()) {
        // The bucket's public address: its r2.dev address or a custom domain, HTTPS, with no
        // credentials, query or fragment. The S3 API address never serves public reads.
        const auto text = value.toString();
        const QUrl url(text, QUrl::StrictMode);
        if (!value.isString() || text.isEmpty() || text.contains('\n') || !url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty() ||
            !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
            return QStringLiteral("Enter the bucket's public address, such as https://pub-….r2.dev or your custom domain, without a query or fragment.");
        if (s3CloudflareR2Endpoint(text))
            return QStringLiteral("That is the bucket's S3 API address, which doesn't serve public links. Enter its r2.dev address or a custom domain.");
    }
    return {};
}
Error validate(const QJsonObject& settings) {
    for (auto it = settings.begin(); it != settings.end(); ++it)
        if (!settingKeys().contains(it.key())) return {"s3_settings_invalid", "Invalid Cloudflare R2 settings: " + it.key() + " is not one of them", {}};
    for (const auto& key : settingKeys())
        if (!settings.contains(key)) return {"s3_settings_invalid", "Invalid Cloudflare R2 settings: " + key + " is missing", {}};
    for (const auto& field : {QStringLiteral("accountId"), QStringLiteral("jurisdiction"), QStringLiteral("bucket"), QStringLiteral("publicUrlBase")})
        if (const auto message = problem(settings, field); !message.isEmpty()) return fieldError(field, message);
    const auto link = settings["linkMode"].toString();
    const auto days = settings["linkValidityDays"];
    if ((link != QLatin1String("public") && link != QLatin1String("presigned")) || !days.isDouble() || days.toDouble() != days.toInt() || days.toInt() < 1 || days.toInt() > 7)
        return {"s3_settings_invalid", "Invalid Cloudflare R2 settings: links are public or expire after 1–7 days", {}};
    return {};
}
}
