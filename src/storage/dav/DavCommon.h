#pragma once
#include "core/Error.h"
#include <QJsonValue>
#include <QJsonObject>
#include <QString>
#include <QUrl>
#include <QtGlobal>
namespace putgd::dav {
// Nextcloud and plain WebDAV destinations (#1872). Two destination types share one
// worker, putgd-webdav-worker: Nextcloud signs in through Login Flow v2 (or an app
// password), uploads over WebDAV and makes a public share link through the OCS Share
// API; plain WebDAV uploads to a collection and builds the link from an optional public
// base. These rules are shared by the resident, the Settings form and the worker. Qt
// Core only: no network types, so the resident links none.
inline constexpr char nextcloudId[] = "nextcloud";
inline constexpr char webdavId[] = "webdav";
inline bool isDav(const QString& provider) { return provider == nextcloudId || provider == webdavId; }
// A file name in one path segment; Nextcloud and most WebDAV servers store 255 bytes.
inline constexpr int maximumNameBytes = 255;
inline constexpr int maximumFolderChars = 1024;
// Nextcloud chunked upload v2: files above the threshold go up in chunks (5 MB to 5 GB
// each, at most 10,000), then one MOVE assembles them. Tests may lower the threshold.
inline constexpr qint64 chunkThresholdBytes = 50LL * 1024 * 1024;
inline constexpr qint64 chunkBytes = 16LL * 1024 * 1024;
inline constexpr qint64 minimumChunkBytes = 5LL * 1024 * 1024;
inline constexpr qint64 maximumChunks = 10000;
// Request bounds: a request without file data has a whole-request deadline; one that
// carries file data only a stall limit (no bytes moving for that long), as for B2.
inline constexpr int connectTimeoutMs = 10000;
inline constexpr int requestTimeoutMs = 120000;
inline constexpr int stallTimeoutMs = 60000;
// Assembling a chunked upload (its MOVE) moves no bytes while the server works: it gets this
// deadline instead of the stall limit.
inline constexpr int moveTimeoutMs = 30 * 60 * 1000;
// Login Flow v2: the poll token is valid for 20 minutes on the server.
inline constexpr int signInTimeoutMs = 20 * 60 * 1000;

QJsonObject nextcloudDefaults();
QJsonObject webdavDefaults();
QJsonObject defaults(const QString& provider);
Error validateSettings(const QString& provider, const QJsonObject&);
// A Nextcloud address as typed or pasted (the Files app, a share page, a WebDAV URL)
// reduced to the install base: scheme, host, port and a subfolder install path, without
// /index.php, /apps/…, /s/…, /remote.php/… or a trailing slash. "cloud.example.com"
// becomes https://cloud.example.com. Empty when it is not an address.
QString nextcloudServer(const QString& typed);
// A WebDAV collection address as typed: trimmed, https:// added when no scheme is given,
// and ending in "/". Empty when it is not an address.
QString collectionAddress(const QString& typed);
// The folder below the user's files: no leading or trailing "/", no empty, "." or ".."
// segment. Empty means the files root.
QString cleanFolder(const QString&);
// One path segment or a path percent-encoded for a URL; "/" stays between segments.
QString encodePath(const QString& path);
// Where the files of this destination live, ending in "/": the Nextcloud user's folder
// (…/remote.php/dav/files/<user>/<folder>/) or the WebDAV collection.
QUrl folderUrl(const QJsonObject& settings, const QString& provider, const QString& userId);
QUrl fileUrl(const QJsonObject& settings, const QString& provider, const QString& userId, const QString& name);
// The link a plain WebDAV upload hands out: the link base (or the collection) + name.
QString webdavLink(const QJsonObject& settings, const QString& name);
// Nextcloud share links: the page, and the file itself for the test's byte compare.
QString sharePage(const QString& server, const QString& token);
QString shareDownload(const QString& server, const QString& token);
// The share token of a Nextcloud share page link, or empty.
QString shareToken(const QString& link);
// A share token is a secret held in a path: text for logs, test journals and
// diagnostics keeps "/s/…" in place of every token.
QString redactShareTokens(const QString&);
QJsonValue redactShareTokensIn(const QJsonValue&);
// Scheme, lower-case host, explicit port and encoded path: one spelling per DAV target, for
// serializing requests and binding credentials.
QString canonicalUrl(const QUrl&);
// Credentials are bound to where they were given (#1872): the server's origin (scheme, host and
// port), and for Nextcloud the account ID the server reported. The binding is
// kept beside the secret in the credential store's session-token field, as compact JSON; it is
// not secret. A request is sent only while the destination still names that place.
QString bindingBase(const QJsonObject& settings, const QString& provider);
QString binding(const QJsonObject& settings, const QString& provider, const QString& userId = {});
QJsonObject parseBinding(const QString& stored);
// Whether a Login Flow v2 page or poll endpoint may be used for this server: the server's scheme,
// host and port, a path below its base, no user information, and plain HTTP only where allowed.
bool loginPageAllowed(const QUrl& page, const QString& server, bool allowInsecure);
// Whether the two addresses have the same scheme, host and port (Login Flow v2's
// server and poll endpoint must be the server the user entered).
bool sameOrigin(const QUrl& a, const QUrl& b);
// Whether Allow plain HTTP applies to an address as typed: http:// to another computer.
bool plainHttpAddress(const QString& typed);
// "Signed in as …" text for Settings, from the saved account name.
QString signedInText(const QJsonObject& settings);
}
