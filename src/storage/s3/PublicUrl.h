#pragma once
#include <QJsonObject>
#include <QString>
namespace putgd {
// The URL of an object, written for the key S3 stores (s3StoredKey).
QString publicObjectUrl(const QJsonObject& settings, const QString& key);
// The URL as receipts recorded it before #1582, with the key exactly as given.
QString s3LegacyPublicObjectUrl(const QJsonObject& settings, const QString& key);
bool s3VirtualAddressing(const QJsonObject& settings);
// S3 keys are literal, but the request path that names one is not: the AWS SDK
// drops empty path segments, so a request for "a//b/p.txt" or "/a/p.txt" stores
// and finds "a/b/p.txt" or "a/p.txt" (#1582). s3StoredKey is that key.
QString s3StoredKey(const QString& key);
// libcurl also removes "." and ".." segments after the SDK has signed the path,
// so a key with one cannot be named by any request put.gd sends.
bool s3KeyAddressable(const QString& key);
// The key an upload of `candidate` uses: the stored form of the folder in bucket,
// a separator, then the candidate.
QString s3ObjectKey(const QJsonObject& settings, const QString& candidate);
// What to tell someone whose Region the AWS SDK refuses, or an empty string. The SDK
// takes only a DNS label; anything else it drops, then fails every request before
// sending it (#1675).
QString s3RegionProblem(const QString& region);
// Cloudflare R2's S3 API, also under a jurisdiction such as ACCOUNT.eu.r2.cloudflarestorage.com.
// R2 has no regions: requests to it are signed with auto whatever Region holds (#1678).
bool s3CloudflareR2Endpoint(const QString& endpoint);
// What to tell someone whose Endpoint has a path, or an empty string. Requests and
// links add the bucket after that path, so a bucket typed there is named twice (#1676).
// `quoteTyped` names the part to remove as typed, for the screen only: the message without it
// carries no part of the path, so refused saves and errors never log one.
QString s3EndpointProblem(const QString& endpoint, bool quoteTyped = false);
}
