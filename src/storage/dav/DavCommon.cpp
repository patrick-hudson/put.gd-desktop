#include "DavCommon.h"
#include "storage/http/HttpCommon.h"
#include <QRegularExpression>
#include <QStringList>
namespace putgd::dav {
QJsonObject nextcloudDefaults() {
    return {{"server", ""}, {"folder", "put.gd"}, {"allowInsecureHttp", false}, {"accountName", ""}};
}
QJsonObject webdavDefaults() {
    return {{"address", ""}, {"linkBase", QJsonValue::Null}, {"allowInsecureHttp", false}};
}
QJsonObject defaults(const QString& provider) { return provider == nextcloudId ? nextcloudDefaults() : webdavDefaults(); }
namespace {
QUrl parsed(const QString& typed) {
    auto text = typed.trimmed();
    if (text.isEmpty()) return {};
    if (!text.contains("://")) text = "https://" + text;
    QUrl url(text, QUrl::TolerantMode);
    if (!url.isValid() || url.host().isEmpty() || !QStringList{"http", "https"}.contains(url.scheme().toLower())) return {};
    url.setScheme(url.scheme().toLower());
    url.setHost(url.host().toLower());
    url.setUserInfo({});
    url.setQuery(QString());
    url.setFragment({});
    return url;
}
bool controlFree(const QString& text) {
    for (const auto c : text) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
    return true;
}
// An address a destination keeps: http(s), a host, no user, query or fragment, and plain
// HTTP only where the custom uploader's rule allows it (loopback, or the switch on).
QString addressProblem(const QJsonValue& value, bool allowInsecure, bool required) {
    if (value.isNull() && !required) return {};
    if (!value.isString()) return QStringLiteral("enter an address");
    const auto text = value.toString();
    if (text.isEmpty()) return required ? QStringLiteral("enter the server address") : QString();
    const QUrl url(text, QUrl::StrictMode);
    if (text.size() > 2048 || !url.isValid() || url.host().isEmpty() || !QStringList{"http", "https"}.contains(url.scheme()) || !url.userInfo().isEmpty() ||
        url.hasQuery() || url.hasFragment() || !controlFree(text))
        return QStringLiteral("the address must be an https:// URL without a user name, query or fragment");
    if (!http::transportAllowed(url, allowInsecure))
        return QStringLiteral("plain http:// addresses need Allow plain HTTP for this destination");
    return {};
}
}
QString nextcloudServer(const QString& typed) {
    auto url = parsed(typed);
    if (!url.isValid()) return {};
    // Everything from the first segment Nextcloud serves below its install base.
    auto path = url.path();
    static const QRegularExpression tail("/(index\\.php|remote\\.php|public\\.php|status\\.php|ocs|apps|s|login|f|settings)(/.*)?$|/+$");
    for (int guard = 0; guard < 8; ++guard) {
        const auto match = tail.match(path);
        if (!match.hasMatch()) break;
        path = path.left(match.capturedStart());
    }
    url.setPath(path);
    auto text = url.toString(QUrl::FullyEncoded);
    while (text.endsWith('/')) text.chop(1);
    return text;
}
QString collectionAddress(const QString& typed) {
    auto url = parsed(typed);
    if (!url.isValid()) return {};
    auto text = url.toString(QUrl::FullyEncoded);
    if (!text.endsWith('/')) text += '/';
    return text;
}
QString cleanFolder(const QString& folder) {
    QStringList kept;
    for (const auto& segment : folder.split('/', Qt::SkipEmptyParts)) {
        const auto trimmed = segment.trimmed();
        if (trimmed.isEmpty() || trimmed == "." || trimmed == "..") continue;
        kept.append(trimmed);
    }
    return kept.join('/');
}
QString encodePath(const QString& path) { return QString::fromLatin1(QUrl::toPercentEncoding(path, "/")); }
Error validateSettings(const QString& provider, const QJsonObject& s) {
    const auto label = provider == nextcloudId ? QStringLiteral("Nextcloud") : QStringLiteral("WebDAV");
    const auto bad = [&](const QString& what) { return Error{"webdav_settings_invalid", "Invalid " + label + " settings: " + what, {}}; };
    if (QStringList(s.keys()) != QStringList(defaults(provider).keys())) return bad("missing or unsupported fields");
    if (!s["allowInsecureHttp"].isBool()) return bad("Allow plain HTTP is true or false");
    const bool insecure = s["allowInsecureHttp"].toBool();
    if (provider == nextcloudId) {
        // Empty until the user enters one: a new destination saves before it is set up.
        if (const auto problem = addressProblem(s["server"], insecure, false); !problem.isEmpty()) return bad(problem);
        const auto server = s["server"].toString();
        if (!server.isEmpty() && (server.endsWith('/') || nextcloudServer(server) != server))
            return bad("the server address is the Nextcloud install's base address, such as https://cloud.example.com");
        const auto folder = s["folder"].toString();
        if (!s["folder"].isString() || folder.size() > maximumFolderChars || cleanFolder(folder) != folder || !controlFree(folder) || folder.contains('\\'))
            return bad("the folder is a path in your files such as put.gd or Screenshots/put.gd, without a leading or trailing /");
        if (!s["accountName"].isString() || s["accountName"].toString().size() > 256 || !controlFree(s["accountName"].toString()))
            return bad("the account name is text of up to 256 characters");
        return {};
    }
    if (const auto problem = addressProblem(s["address"], insecure, false); !problem.isEmpty()) return bad(problem);
    if (!s["address"].toString().isEmpty() && !s["address"].toString().endsWith('/')) return bad("the folder address ends with /");
    if (const auto problem = addressProblem(s["linkBase"], insecure, false); !problem.isEmpty()) return bad("link address: " + problem);
    if (s["linkBase"].isString() && s["linkBase"].toString().isEmpty()) return bad("leave the link address out (null) instead of empty");
    return {};
}
QUrl folderUrl(const QJsonObject& settings, const QString& provider, const QString& userId) {
    if (provider == webdavId) return QUrl(settings["address"].toString(), QUrl::StrictMode);
    auto base = settings["server"].toString() + "/remote.php/dav/files/" + QString::fromLatin1(QUrl::toPercentEncoding(userId)) + "/";
    const auto folder = settings["folder"].toString();
    if (!folder.isEmpty()) base += encodePath(folder) + "/";
    return QUrl(base, QUrl::StrictMode);
}
QUrl fileUrl(const QJsonObject& settings, const QString& provider, const QString& userId, const QString& name) {
    return QUrl(folderUrl(settings, provider, userId).toString(QUrl::FullyEncoded) + QString::fromLatin1(QUrl::toPercentEncoding(name)), QUrl::StrictMode);
}
QString webdavLink(const QJsonObject& settings, const QString& name) {
    auto base = settings["linkBase"].isString() ? settings["linkBase"].toString() : settings["address"].toString();
    if (!base.endsWith('/')) base += '/';
    return base + QString::fromLatin1(QUrl::toPercentEncoding(name));
}
QString sharePage(const QString& server, const QString& token) { return server + "/s/" + token; }
QString shareDownload(const QString& server, const QString& token) { return sharePage(server, token) + "/download"; }
QString shareToken(const QString& link) {
    static const QRegularExpression token("/s/([A-Za-z0-9]+)/?$");
    const auto match = token.match(QUrl(link).path());
    return match.hasMatch() ? match.captured(1) : QString();
}
QString redactShareTokens(const QString& text) {
    static const QRegularExpression token("/s/[A-Za-z0-9]{6,}");
    auto out = text;
    return out.replace(token, QStringLiteral("/s/…"));
}
bool sameOrigin(const QUrl& a, const QUrl& b) {
    const auto port = [](const QUrl& url) { return url.port(url.scheme() == "https" ? 443 : 80); };
    return a.scheme() == b.scheme() && a.host().compare(b.host(), Qt::CaseInsensitive) == 0 && port(a) == port(b);
}
bool plainHttpAddress(const QString& typed) {
    const QUrl url(typed.trimmed());
    return url.scheme() == "http" && !http::loopbackHost(url.host());
}
QString signedInText(const QJsonObject& settings) {
    const auto name = settings["accountName"].toString();
    return name.isEmpty() ? QStringLiteral("Signed out") : QStringLiteral("Signed in as ") + name;
}
}
