#pragma once
#include <QJsonObject>
#include <QString>
namespace putgd {
// The URL of an object, written for the key S3 stores (s3StoredKey).
QString publicObjectUrl(const QJsonObject& settings, const QString& key);
// The URL as receipts recorded it before #1582, with the key exactly as given.
QString s3LegacyPublicObjectUrl(const QJsonObject& settings, const QString& key);
bool s3VirtualAddressing(const QJsonObject& settings);
// The endpoint as an upload's identity names it (#1699): scheme and host lowercased and a
// trailing slash dropped, so a Cloudflare R2 destination and an S3 destination on the same R2
// endpoint serialize and match the same objects.
QString s3IdentityEndpoint(const QString& endpoint);
// Whether the endpoint's host is AWS: amazonaws.com or amazonaws.com.cn, or a host under one, with
// one trailing dot ignored. The one check for Amazon S3 versus another S3-compatible service (#1699).
bool s3AmazonEndpoint(const QString& endpoint);
// Whether S3 requests to this endpoint may follow an HTTP redirect: only to an AWS host. A
// signed request to Cloudflare R2 or another S3-compatible service never goes to another host.
bool s3FollowsRedirects(const QJsonObject& settings);
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
// The same for a Region at this endpoint: also aws-global anywhere but AWS (s3AmazonEndpoint).
QString s3RegionProblem(const QString& region, const QString& endpoint);
// Cloudflare R2's S3 API, also under a jurisdiction such as ACCOUNT.eu.r2.cloudflarestorage.com.
// R2 has no regions: requests to it are signed with auto whatever Region holds (#1678).
bool s3CloudflareR2Endpoint(const QString& endpoint);
// What to tell someone whose Endpoint has a path, or an empty string. Requests and
// links add the bucket after that path, so a bucket typed there is named twice (#1676).
// `quoteTyped` names the part to remove as typed, for the screen only: the message without it
// carries no part of the path, so refused saves and errors never log one.
QString s3EndpointProblem(const QString& endpoint, bool quoteTyped = false);
}
