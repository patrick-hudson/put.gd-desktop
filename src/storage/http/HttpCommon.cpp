#include "HttpCommon.h"
#include "HttpTemplate.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrlQuery>
#include <algorithm>
namespace putgd::http {
namespace {
Error bad(const QString& what, const QString& field = {}) {
    return {"http_settings_invalid", "Invalid custom uploader settings: " + what, field.isEmpty() ? QJsonObject{} : QJsonObject{{"field", field}}};
}
bool integer(const QJsonValue& value, double low, double high) {
    const auto number = value.toDouble(-1);
    return value.isDouble() && number >= low && number <= high && number == double(qint64(number));
}
bool controlFree(const QString& text) {
    for (const auto c : text) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
    return true;
}
// RFC 9110 token characters.
bool headerName(const QString& name) {
    static const QRegularExpression token("^[A-Za-z0-9!#$%&'*+.^_`|~-]{1,256}$");
    return token.match(name).hasMatch();
}
// Headers the transport writes itself; a configured value would corrupt the request.
bool managedHeader(const QString& name) {
    static const QStringList managed{"host", "content-length", "transfer-encoding", "connection", "upgrade", "expect", "te", "trailer", "keep-alive", "proxy-connection"};
    return managed.contains(name.toLower());
}
QString singular(const QString& list) {
    return list == "headers" ? QStringLiteral("header") : list == "parameters" ? QStringLiteral("parameter") : list == "secrets" ? QStringLiteral("secret value") : QStringLiteral("argument");
}
// The request URL with its placeholders filled by a sample, as the transport rule sees it.
QUrl sampleUrl(const QString& text) {
    RequestValues sample;
    sample.fileName = "file.png";
    sample.input = "text";
    sample.hasInput = true;
    sample.encode = RequestValues::Encode::Url;
    sample.sampleSecrets = true;
    return QUrl(renderRequest(text, sample), QUrl::StrictMode);
}
Error urlProblem(const QString& text, bool allowInsecure) {
    if (text.isEmpty()) return {};
    if (text.size() > 2048 || !controlFree(text)) return bad("the request URL is too long or has control characters", "requestUrl");
    if (const auto problem = templateProblem(text, TemplateUse::Request); !problem.isEmpty()) return bad("request URL: " + problem, "requestUrl");
    // Scheme and host are fixed text: a placeholder may only fill the path.
    const auto authority = text.indexOf("://");
    const auto path = authority < 0 ? -1 : text.indexOf('/', authority + 3);
    const auto brace = text.indexOf('{');
    if (authority < 0 || (brace >= 0 && (path < 0 || brace < path))) return bad("the request URL must start with https:// and a host name", "requestUrl");
    const auto url = sampleUrl(text);
    if (!url.isValid() || url.host().isEmpty() || !url.userInfo().isEmpty() || url.hasFragment() || !QStringList{"http", "https"}.contains(url.scheme()))
        return bad("the request URL must be an http(s) address without a user name, password or fragment", "requestUrl");
    if (url.hasQuery()) return bad("put query values under Parameters, not in the request URL", "requestUrl");
    if (!transportAllowed(url, allowInsecure))
        return {"http_insecure_url", "This address uses plain HTTP, which sends the file and its credentials unencrypted. Use https://, or turn on Allow plain HTTP for this destination.",
                {{"field", "requestUrl"}}};
    return {};
}
QString text(const QJsonValue& value) {
    if (value.isString()) return value.toString();
    if (value.isBool()) return value.toBool() ? QStringLiteral("true") : QStringLiteral("false");
    if (value.isDouble()) return QString::number(value.toDouble(), 'g', 17);
    return {};
}
int compareVersions(const QString& a, const QString& b) {
    const auto left = a.split('.'), right = b.split('.');
    for (int i = 0; i < qMax(left.size(), right.size()); ++i) {
        const int x = left.value(i).toInt(), y = right.value(i).toInt();
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}
Error sxcuError(const QString& code, const QString& message) { return {code, message, {}}; }
}

const QStringList& rowLists() {
    static const QStringList lists{"headers", "parameters", "arguments"};
    return lists;
}
QJsonObject defaults() {
    return {{"accepts", QJsonArray{"image", "text", "file"}}, {"allowInsecureHttp", false}, {"arguments", QJsonArray{}}, {"body", "multipart"},
            {"connectTimeoutMs", 10000}, {"data", ""}, {"dataSecret", false}, {"deletionUrl", ""}, {"errorMessage", ""}, {"fileFormName", "file"},
            {"headers", QJsonArray{}}, {"parameters", QJsonArray{}}, {"requestMethod", "POST"}, {"requestTimeoutMs", 600000}, {"requestUrl", ""},
            {"responseUrl", ""}, {"secrets", QJsonArray{}}, {"thumbnailUrl", ""}};
}
QString secretKey(const QString& list, const QString& name) { return list + "/" + name; }
QStringList secretKeys(const QJsonObject& settings) {
    QStringList keys;
    for (const auto& list : rowLists())
        for (const auto& value : settings[list].toArray())
            if (value.toObject()["secret"].toBool()) keys.append(secretKey(list, value.toObject()["name"].toString()));
    if (settings["dataSecret"].toBool()) keys.append(dataSecretKey());
    for (const auto& value : settings[extraSecretsList()].toArray()) keys.append(secretKey(extraSecretsList(), value.toObject()["name"].toString()));
    return keys;
}
QString secretLabel(const QString& key) {
    if (key == dataSecretKey()) return QStringLiteral("request body");
    const auto slash = key.indexOf('/');
    return singular(key.left(slash)) + " " + key.mid(slash + 1);
}
QString artifactKind(const QString& mime, const QString& category) {
    if (mime.startsWith("image/") || category == "images") return QStringLiteral("image");
    if (mime.startsWith("text/") || category == "text") return QStringLiteral("text");
    return QStringLiteral("file");
}
bool acceptsKind(const QJsonObject& settings, const QString& kind) {
    const auto accepts = settings["accepts"].toArray();
    return accepts.contains("file") || accepts.contains(kind);
}
bool loopbackHost(const QString& host) { return QStringList{"127.0.0.1", "::1", "localhost"}.contains(host.toLower()); }
bool transportAllowed(const QUrl& url, bool allowInsecureHttp) {
    if (url.scheme() == "https") return true;
    return url.scheme() == "http" && (allowInsecureHttp || loopbackHost(url.host()));
}
bool sameHost(const QUrl& from, const QUrl& to) {
    if (from.host().compare(to.host(), Qt::CaseInsensitive) != 0 || to.host().isEmpty()) return false;
    if (from.scheme() == to.scheme()) return from.port(from.scheme() == "https" ? 443 : 80) == to.port(to.scheme() == "https" ? 443 : 80);
    // http → https on the default ports is the same server asking for encryption.
    return from.scheme() == "http" && to.scheme() == "https" && from.port(80) == 80 && to.port(443) == 443;
}
QString boundedMessage(const QString& input, int maximum) {
    QString out;
    for (const auto c : input) {
        if (c == '\n' || c == '\r' || c == '\t') { if (!out.endsWith(' ')) out.append(' '); }
        else if (c.unicode() >= 0x20 && c.unicode() != 0x7f) out.append(c);
        if (out.size() >= maximum) { out = out.left(maximum - 1) + QChar(0x2026); break; }
    }
    return out.trimmed();
}

Error validateSettings(const QJsonObject& s) {
    if (QStringList(s.keys()) != QStringList(defaults().keys())) return bad("missing or unsupported fields");
    if (!s["allowInsecureHttp"].isBool() || !s["dataSecret"].isBool()) return bad("switches must be true or false");
    const auto method = s["requestMethod"].toString();
    if (!QStringList{"GET", "POST", "PUT", "PATCH"}.contains(method)) return bad("the method is GET, POST, PUT or PATCH", "requestMethod");
    const auto body = s["body"].toString();
    if (!QStringList{"none", "multipart", "form", "json", "xml", "binary"}.contains(body)) return bad("the body is none, multipart, form, json, xml or binary", "body");
    if (method == "GET" && body != "none") return bad("a GET request has no body; choose POST, PUT or PATCH, or the body None", "body");
    if (!s["requestUrl"].isString()) return bad("the request URL must be text", "requestUrl");
    if (auto error = urlProblem(s["requestUrl"].toString(), s["allowInsecureHttp"].toBool())) return error;
    const auto accepts = s["accepts"].toArray();
    QSet<QString> kinds;
    for (const auto& value : accepts) if (QStringList{"image", "text", "file"}.contains(value.toString())) kinds.insert(value.toString());
    if (!s["accepts"].isArray() || accepts.isEmpty() || kinds.size() != accepts.size()) return bad("choose at least one of image, text and file", "accepts");
    const auto form = s["fileFormName"].toString();
    if (!s["fileFormName"].isString() || form.size() > maximumNameChars || !controlFree(form)) return bad("the file form name is too long or has control characters", "fileFormName");
    for (const auto& list : rowLists()) {
        if (!s[list].isArray() || s[list].toArray().size() > maximumRows) return bad(QString("at most %1 %2").arg(maximumRows).arg(list), list);
        QSet<QString> names;
        for (const auto& value : s[list].toArray()) {
            const auto row = value.toObject();
            const auto name = row["name"].toString();
            const bool secret = row["secret"].toBool();
            if (!value.isObject() || !row["name"].isString() || !row["secret"].isBool() || QStringList(row.keys()) != (secret ? QStringList{"name", "secret"} : QStringList{"name", "secret", "value"}))
                // A secret row has a name only: its value is a credential, kept in the credential store.
                return bad("each " + singular(list) + " has a name, a secret flag and, unless secret, a value", list);
            if (name.isEmpty() || name.size() > maximumNameChars || !controlFree(name)) return bad("a " + singular(list) + " name is empty, too long or has control characters", list);
            if (list == "headers" && (!headerName(name) || managedHeader(name))) return bad("the header name " + name.left(64) + " cannot be set", list);
            if (list == "headers" && name.compare("content-type", Qt::CaseInsensitive) == 0 && body != "binary")
                return bad("the Content-Type header follows the body type; it can be set for a binary body only", list);
            const auto unique = list == "headers" ? name.toLower() : name;
            if (names.contains(unique)) return bad("the " + singular(list) + " " + name.left(64) + " appears twice", list);
            names.insert(unique);
            if (secret) continue;
            const auto content = row["value"].toString();
            if (!row["value"].isString() || content.size() > maximumValueChars || (list == "headers" && !controlFree(content)))
                return bad("the value of " + singular(list) + " " + name.left(64) + " is too long or has control characters", list);
            if (const auto problem = templateProblem(content, TemplateUse::Request); !problem.isEmpty()) return bad(singular(list) + " " + name.left(64) + ": " + problem, list);
        }
    }
    {
        // Named values with no row of their own: a name each, never a value.
        const auto extra = s[extraSecretsList()].toArray();
        if (!s[extraSecretsList()].isArray() || extra.size() > maximumRows) return bad(QString("at most %1 other secret values").arg(maximumRows), "secrets");
        static const QRegularExpression plainName("^[A-Za-z0-9._-]{1,64}$");
        QSet<QString> names;
        for (const auto& value : extra) {
            const auto row = value.toObject();
            const auto name = row["name"].toString();
            if (!value.isObject() || QStringList(row.keys()) != QStringList{"name", "secret"} || row["secret"] != true || !plainName.match(name).hasMatch() || names.contains(name))
                return bad("each other secret value has a unique name of letters, digits, dots, hyphens or underscores, and no value in settings", "secrets");
            names.insert(name);
        }
    }
    {
        // {secret:key} may only name a value this destination stores.
        const auto known = secretKeys(s);
        QStringList texts{s["requestUrl"].toString(), s["responseUrl"].toString(), s["thumbnailUrl"].toString(), s["deletionUrl"].toString(), s["errorMessage"].toString()};
        for (const auto& list : rowLists()) for (const auto& value : s[list].toArray()) texts.append(value.toObject()["value"].toString());
        for (const auto& text : texts)
            for (const auto& key : secretReferences(text))
                if (!known.contains(key)) return bad("{secret:" + boundedMessage(key, 64) + "} names a value this destination does not store; add it as a secret row", "secrets");
    }
    if (!s["data"].isString() || s["data"].toString().size() > maximumDataChars) return bad("the request body text is too long", "data");
    if (s["dataSecret"].toBool() && !s["data"].toString().isEmpty()) return bad("a secret request body is kept in the credential store, not in settings", "data");
    for (const auto* key : {"responseUrl", "thumbnailUrl", "deletionUrl", "errorMessage"}) {
        const auto content = s[key].toString();
        if (!s[key].isString() || content.size() > maximumTemplateChars || !controlFree(content)) return bad("a response value is too long or has control characters", key);
        if (const auto problem = templateProblem(content, TemplateUse::Response); !problem.isEmpty()) return bad(QString(key) + ": " + problem, key);
    }
    if (!integer(s["connectTimeoutMs"], 1000, 120000) || !integer(s["requestTimeoutMs"], 1000, 3600000))
        return bad("timeouts must be whole milliseconds within their limits", "requestTimeoutMs");
    return {};
}

namespace {
SxcuImport readSxcu(const QByteArray& json);
}
SxcuImport importSxcu(const QByteArray& json) {
    auto out = readSxcu(json);
    // A refused file hands nothing on, the values read before the refusal included.
    if (out.error) { out.settings = {}; out.secrets.clear(); out.notes.clear(); out.name.clear(); }
    return out;
}
namespace {
SxcuImport readSxcu(const QByteArray& json) {
    SxcuImport out;
    if (json.size() > maximumSxcuBytes) { out.error = sxcuError("sxcu_invalid", "This file is too large to be a ShareX custom uploader (.sxcu)."); return out; }
    QJsonParseError parse;
    const auto document = QJsonDocument::fromJson(json, &parse);
    if (parse.error != QJsonParseError::NoError || !document.isObject()) {
        out.error = sxcuError("sxcu_invalid", "This is not a ShareX custom uploader (.sxcu): it must be one JSON object.");
        return out;
    }
    // ShareX reads property names without regard to case.
    QHash<QString, QJsonValue> fields;
    const auto object = document.object();
    for (auto it = object.begin(); it != object.end(); ++it) fields.insert(it.key().toLower(), it.value());
    const auto field = [&](const char* name) { return fields.value(QString::fromLatin1(name).toLower()); };
    const auto fieldText = [&](const char* name) { return text(field(name)); };

    const auto version = fieldText("Version").trimmed();
    const bool legacy = version.isEmpty() || compareVersions(version, "12.3.1") <= 0;

    // DestinationType is a flag list: "ImageUploader, TextUploader, FileUploader".
    QStringList types;
    const auto destination = field("DestinationType");
    if (destination.isDouble()) {
        const int flags = destination.toInt();
        const QStringList order{"imageuploader", "textuploader", "fileuploader", "urlshortener", "urlsharingservice"};
        for (int bit = 0; bit < order.size(); ++bit) if (flags & (1 << bit)) types.append(order[bit]);
    } else for (const auto& part : destination.toString().split(',', Qt::SkipEmptyParts)) types.append(part.trimmed().toLower());
    types.removeAll("none");
    QJsonArray accepts;
    for (const auto& [type, kind] : QList<QPair<QString, QString>>{{"imageuploader", "image"}, {"textuploader", "text"}, {"fileuploader", "file"}})
        if (types.contains(type)) accepts.append(kind);
    const bool shortener = types.contains("urlshortener"), sharing = types.contains("urlsharingservice");
    if (accepts.isEmpty() && (shortener || sharing)) {
        out.error = sxcuError("sxcu_unsupported_type", QString("This file describes a %1, not an uploader. put.gd imports image, text and file uploaders only.")
                                                          .arg(shortener && sharing ? "URL shortener and URL sharing service" : shortener ? "URL shortener" : "URL sharing service"));
        return out;
    }
    for (const auto& type : types)
        if (!QStringList{"imageuploader", "textuploader", "fileuploader", "urlshortener", "urlsharingservice"}.contains(type)) {
            out.error = sxcuError("sxcu_invalid", "This file names a destination type put.gd does not know.");
            return out;
        }
    if (accepts.isEmpty()) {
        accepts = QJsonArray{"image", "text", "file"};
        out.notes.append("The file names no destination type; it was imported for images, text and files.");
    } else if (shortener || sharing) out.notes.append("The file is also a URL shortener or sharing service; put.gd uses its uploader part only.");

    auto method = fieldText("RequestMethod").trimmed().toUpper();
    if (method.isEmpty()) method = fieldText("RequestType").trimmed().toUpper(); // before 12.4
    if (method.isEmpty()) method = "POST";
    if (!QStringList{"GET", "POST", "PUT", "PATCH"}.contains(method)) {
        out.error = sxcuError("sxcu_unsupported", QString("This uploader sends a %1 request, which put.gd does not support. Use GET, POST, PUT or PATCH.").arg(boundedMessage(method, 16)));
        return out;
    }

    QString body;
    const auto bodyField = field("Body");
    static const QStringList bodies{"none", "multipart", "form", "json", "xml", "binary"};
    if (bodyField.isDouble() && bodyField.toInt() >= 0 && bodyField.toInt() < bodies.size()) body = bodies[bodyField.toInt()];
    else {
        const auto name = bodyField.toString().trimmed().toLower();
        static const QHash<QString, QString> names{{"none", "none"}, {"multipartformdata", "multipart"}, {"formurlencoded", "form"}, {"json", "json"}, {"xml", "xml"}, {"binary", "binary"}};
        if (name.isEmpty()) body = legacy && method == "POST" ? "multipart" : "none"; // before 12.4 a POST was always a form
        else if (names.contains(name)) body = names.value(name);
        else { out.error = sxcuError("sxcu_invalid", "This file names a request body put.gd does not know."); return out; }
    }
    if (method == "GET" && body != "none") {
        out.notes.append("The file pairs a GET request with a body; put.gd sends the GET without one.");
        body = "none";
    }

    QStringList regexList;
    for (const auto& value : field("RegexList").toArray()) regexList.append(value.toString());

    auto requestUrl = fieldText("RequestURL").trimmed();
    if (requestUrl.isEmpty()) { out.error = sxcuError("sxcu_invalid", "This file has no RequestURL, the address uploads are sent to."); return out; }
    // Template text of every field, to tell the two placeholder generations apart.
    QStringList templates{requestUrl, fieldText("URL"), fieldText("ThumbnailURL"), fieldText("DeletionURL"), fieldText("ErrorMessage")};
    for (const auto* list : {"Parameters", "Headers", "Arguments"}) for (const auto& value : field(list).toObject()) templates.append(text(value));
    bool old = !version.isEmpty() && compareVersions(version, "13.7.1") <= 0;
    if (version.isEmpty()) for (const auto& value : templates) if (usesOldSyntax(value)) old = true;
    const auto migrate = [&](const QString& value) { return old ? migrateOldSyntax(value, regexList) : value; };
    if (old) out.notes.append("The file uses the older $…$ placeholders; they were converted.");

    requestUrl = migrate(requestUrl);
    if (!requestUrl.contains("://")) { requestUrl = "https://" + requestUrl; out.notes.append("The request URL had no scheme; https:// was added."); }
    // A query in the URL becomes parameters, as ShareX does on load.
    QList<QPair<QString, QString>> queryParameters;
    if (const auto mark = requestUrl.indexOf('?'); mark >= 0) {
        const auto query = requestUrl.mid(mark + 1);
        requestUrl = requestUrl.left(mark);
        for (const auto& [key, value] : QUrlQuery(query).queryItems(QUrl::FullyDecoded)) if (!key.isEmpty()) queryParameters.append({key, value});
    }
    if (const auto hash = requestUrl.indexOf('#'); hash >= 0) requestUrl = requestUrl.left(hash);

    auto settings = defaults();
    settings["accepts"] = accepts;
    settings["requestMethod"] = method;
    settings["requestUrl"] = requestUrl;
    settings["body"] = body;
    settings["fileFormName"] = body == "multipart" ? fieldText("FileFormName") : QString();
    qsizetype secretBytes = 0;
    static const QRegularExpression namePattern("%(?:y|yy|mo|mon|mon2|d|h|mi|s|ms|pm|w|w2|wy|unix|i|ia|ib|ix|rn|ra|rna|rx|guid|radjective|ranimal|remoji|rf|width|height|t|pn|un|uln|cn|n)\\b");
    bool patterns = false;
    for (const auto& [sxcuName, list] : QList<QPair<const char*, QString>>{{"Parameters", "parameters"}, {"Headers", "headers"}, {"Arguments", "arguments"}}) {
        if (!field(sxcuName).isUndefined() && !field(sxcuName).isNull() && !field(sxcuName).isObject()) {
            out.error = sxcuError("sxcu_invalid", QString("%1 must be an object of names and values.").arg(sxcuName));
            return out;
        }
        QList<QPair<QString, QString>> entries;
        if (list == "parameters") entries = queryParameters;
        const auto source = field(sxcuName).toObject();
        for (auto it = source.begin(); it != source.end(); ++it) {
            bool replaced = false;
            for (auto& entry : entries) if (entry.first == it.key()) { entry.second = text(it.value()); replaced = true; }
            if (!replaced) entries.append({it.key(), text(it.value())});
        }
        // Arguments are form fields: other bodies have none.
        if (list == "arguments" && body != "multipart" && body != "form") {
            if (!entries.isEmpty()) out.notes.append("The file's Arguments are not sent with this request body.");
            entries.clear();
        }
        if (entries.size() > maximumRows) { out.error = sxcuError("sxcu_unsupported", QString("This file has more than %1 %2.").arg(maximumRows).arg(list)); return out; }
        QJsonArray rows;
        QSet<QString> seen;
        for (const auto& [name, raw] : entries) {
            const auto label = singular(list) + " " + boundedMessage(name, 64);
            if (name.isEmpty() || name.size() > maximumNameChars || !controlFree(name)) { out.error = sxcuError("sxcu_invalid", "A " + singular(list) + " has an empty or unusable name."); return out; }
            if (list == "headers") {
                if (managedHeader(name) || (name.compare("content-type", Qt::CaseInsensitive) == 0 && body != "binary")) {
                    out.notes.append("The header " + boundedMessage(name, 64) + " is set by put.gd for this request and was left out.");
                    continue;
                }
                if (!headerName(name)) { out.error = sxcuError("sxcu_invalid", "The " + label + " is not a valid header name."); return out; }
            }
            const auto unique = list == "headers" ? name.toLower() : name;
            if (seen.contains(unique)) { out.error = sxcuError("sxcu_invalid", "The " + label + " appears twice."); return out; }
            seen.insert(unique);
            const auto value = migrate(raw);
            if (value.size() > maximumValueChars || (list == "headers" && !controlFree(value))) { out.error = sxcuError("sxcu_unsupported", "The value of " + label + " is too long or has control characters."); return out; }
            if (const auto problem = templateProblem(value, TemplateUse::Request); !problem.isEmpty()) { out.error = sxcuError("sxcu_unsupported", "The " + label + " cannot be used: " + problem); return out; }
            if (namePattern.match(value).hasMatch()) patterns = true;
            const auto plain = value.trimmed().toLower();
            // A value that is only the file name or the text placeholder holds no credential.
            if (value.isEmpty() || plain == "{filename}" || plain == "{input}") rows.append(QJsonObject{{"name", name}, {"secret", false}, {"value", value}});
            else {
                rows.append(QJsonObject{{"name", name}, {"secret", true}});
                out.secrets.insert(secretKey(list, name), value);
                secretBytes += value.toUtf8().size() + name.toUtf8().size();
            }
        }
        settings[list] = rows;
    }
    if (patterns) out.notes.append("A value uses a ShareX name pattern such as %y or %ra; put.gd sends it as written.");

    if (body == "json" || body == "xml") {
        auto data = fieldText("Data");
        if (old) data.replace("$input$", "{input}", Qt::CaseInsensitive).replace("$filename$", "{filename}", Qt::CaseInsensitive);
        if (data.size() > maximumDataChars) { out.error = sxcuError("sxcu_unsupported", "The file's request body text is too long."); return out; }
        if (!data.isEmpty()) {
            settings["dataSecret"] = true;
            out.secrets.insert(dataSecretKey(), data);
            secretBytes += data.toUtf8().size();
        }
    }
    if (secretBytes > maximumSecretBytes) { out.error = sxcuError("sxcu_unsupported", "The file's header, parameter and argument values are too large to store."); return out; }

    // A stored value written out again elsewhere in the file is named there, not repeated:
    // XBackBone, for one, ends its deletion URL with the upload token.
    QList<QPair<QString, QString>> known; // value → key, longest first
    for (auto it = out.secrets.cbegin(); it != out.secrets.cend(); ++it) if (it.key() != dataSecretKey() && it.value().size() >= 6 && !it.value().contains('{')) known.append({it.value(), it.key()});
    std::sort(known.begin(), known.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    QJsonArray extra;
    const auto named = [&](QString value) {
        for (const auto& [secret, key] : known) {
            QString name;
            for (const auto c : key) { if (QStringLiteral("{}|\\").contains(c)) name.append('\\'); name.append(c); }
            value.replace(secret, "{secret:" + name + "}");
        }
        return value;
    };
    // A long opaque run of letters and digits left in an address is treated as a key:
    // it moves to a secret value of its own.
    const auto opaque = [&](QString value, const QString& name, qsizetype from) {
        static const QRegularExpression run("[A-Za-z0-9_-]{20,}"), digit("[0-9]"), letter("[A-Za-z]");
        for (int guard = 0; guard < maximumRows; ++guard) {
            QRegularExpressionMatch found;
            for (auto it = run.globalMatch(value, from); it.hasNext();) {
                const auto match = it.next();
                const auto text = match.captured();
                // Inside a placeholder it is a path or pattern, not a key.
                const auto before = value.left(match.capturedStart());
                if (before.count('{') > before.count('}') || !text.contains(digit) || !text.contains(letter)) continue;
                found = match;
                break;
            }
            if (!found.hasMatch()) break;
            const auto row = name + (extra.isEmpty() ? QString() : "-" + QString::number(extra.size() + 1));
            extra.append(QJsonObject{{"name", row}, {"secret", true}});
            out.secrets.insert(secretKey(extraSecretsList(), row), found.captured());
            value = value.left(found.capturedStart()) + "{secret:" + secretKey(extraSecretsList(), row) + "}" + value.mid(found.capturedEnd());
        }
        return value;
    };
    {
        auto address = named(requestUrl);
        const auto authority = address.indexOf("://");
        const auto path = authority < 0 ? -1 : address.indexOf('/', authority + 3);
        if (path > 0) address = opaque(address, "url-key", path);
        if (address != requestUrl) { settings["requestUrl"] = address; requestUrl = address; }
    }
    auto responseUrl = named(migrate(fieldText("URL")));
    // Before 12.4 a ResponseType said where the link was when URL was empty.
    if (responseUrl.isEmpty()) {
        const auto type = fieldText("ResponseType").trimmed().toLower();
        if (type == "redirectionurl") responseUrl = "{responseurl}";
        else if (type == "locationheader") responseUrl = "{header:Location}";
    }
    settings["responseUrl"] = responseUrl;
    settings["thumbnailUrl"] = named(migrate(fieldText("ThumbnailURL")));
    {
        auto deletion = named(migrate(fieldText("DeletionURL")));
        // After the host: the host name itself is no key.
        const auto authority = deletion.indexOf("://");
        deletion = opaque(deletion, "deletion-key", authority < 0 ? 0 : qMax<qsizetype>(0, deletion.indexOf('/', authority + 3)));
        settings["deletionUrl"] = deletion;
    }
    settings["errorMessage"] = migrate(fieldText("ErrorMessage"));
    settings[extraSecretsList()] = extra;
    for (const auto& [key, sxcuName] : QList<QPair<const char*, const char*>>{{"responseUrl", "URL"}, {"thumbnailUrl", "ThumbnailURL"}, {"deletionUrl", "DeletionURL"}, {"errorMessage", "ErrorMessage"}}) {
        const auto value = settings[key].toString();
        if (value.size() > maximumTemplateChars || !controlFree(value)) { out.error = sxcuError("sxcu_unsupported", QString("%1 is too long or has control characters.").arg(sxcuName)); return out; }
        if (const auto problem = templateProblem(value, TemplateUse::Response); !problem.isEmpty()) { out.error = sxcuError("sxcu_unsupported", QString("%1 cannot be used: %2").arg(sxcuName, problem)); return out; }
    }

    // Every rule but the transport one: a plain-HTTP address imports, and the destination
    // then says what to change.
    auto relaxed = settings;
    relaxed["allowInsecureHttp"] = true;
    if (auto error = validateSettings(relaxed)) { out.error = sxcuError("sxcu_unsupported", error.message.replace("Invalid custom uploader settings: ", "This file cannot be used: ")); return out; }
    const auto url = sampleUrl(requestUrl);
    if (!transportAllowed(url, false)) out.notes.append("The address uses plain HTTP, which sends files and credentials unencrypted. Turn on Allow plain HTTP to use it.");
    out.name = boundedMessage(fieldText("Name"), 96);
    if (out.name.isEmpty()) out.name = url.host();
    out.settings = settings;
    return out;
}
}
}
