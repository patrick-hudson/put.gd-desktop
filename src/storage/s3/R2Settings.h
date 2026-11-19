#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QString>
#include <optional>
// Cloudflare R2 as a destination of its own (#1678, #1679, #1691). Its settings name the
// account and jurisdiction; the S3 driver underneath gets the endpoint built from them,
// the region auto and path addressing. No AWS headers: Settings and the resident share it.
namespace putgd::r2 {
inline const QString providerId = QStringLiteral("cloudflare-r2");
// The setting keys, in the order Settings shows them.
const QStringList& settingKeys();
QJsonObject defaults();
// 32 hexadecimal digits, as the R2 overview's Account details show it.
bool accountIdValid(const QString& accountId);
// "default", "eu" or "fedramp".
bool jurisdictionValid(const QString& jurisdiction);
// R2's bucket rule: 3-63 lowercase letters, digits and hyphens, starting and ending with a
// letter or digit.
bool bucketValid(const QString& bucket);
// https://ACCOUNT.r2.cloudflarestorage.com, with .eu or .fedramp before .r2 for a jurisdiction.
QString endpoint(const QString& accountId, const QString& jurisdiction);
// The settings the S3 driver runs with: endpoint, region auto, path addressing, ACL omit.
QJsonObject s3Settings(const QJsonObject& settings);
// The dashboard's "S3 API" address, https://ACCOUNT[.eu|.fedramp].r2.cloudflarestorage.com
// with an optional /BUCKET, read from the raw text. Read only when every part is valid: a
// bucket is one path segment by R2's rule, and nothing else follows; percent-encoding is refused.
struct S3ApiAddress { QString accountId, jurisdiction, bucket; };
std::optional<S3ApiAddress> parseS3ApiAddress(const QString& text);
// Move to Cloudflare R2: the R2 settings for an S3 destination whose endpoint is an R2
// account endpoint, or nothing when it is not one put.gd can name this way.
std::optional<QJsonObject> fromS3Settings(const QJsonObject& s3);
// What is wrong with these settings, or an empty error. The S3 rules then apply to
// s3Settings() of them. Field errors carry details.field (accountId, jurisdiction, bucket).
Error validate(const QJsonObject& settings);
// What Settings says at one field (accountId, jurisdiction, bucket, publicUrlBase) about these
// settings, or an empty string: the same rules validate() applies.
QString problem(const QJsonObject& settings, const QString& field);
}
