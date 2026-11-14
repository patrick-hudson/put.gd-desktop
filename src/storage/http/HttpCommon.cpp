#include "HttpCommon.h"
#include <functional>
#include "HttpTemplate.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QRegularExpression>
#include <QSet>
#include <QUrl>
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
QString singleLine(const QString& input) {
    QString out;
    out.reserve(input.size());
    for (const auto c : input) {
        if (c == '\n' || c == '\r' || c == '\t') { if (!out.endsWith(' ')) out.append(' '); }
        else if (c.unicode() >= 0x20 && !(c.unicode() >= 0x7f && c.unicode() < 0xa0)) out.append(c);
    }
    return out.trimmed();
}
QString boundedMessage(const QString& input, int maximum) {
    const auto line = singleLine(input);
    return line.size() >= maximum ? line.left(maximum - 1).trimmed() + QChar(0x2026) : line;
}

namespace {
// A name that says its value is a credential: of a header, parameter or argument, or of
// a member inside a stored body. "Authorization", "X-Api-Key", "upload_token", "pwd".
bool credentialName(const QString& name) {
    const auto lower = name.toLower();
    static const QStringList marks{"token", "secret", "passw", "passphrase", "pwd", "cookie", "credential", "bearer", "signature", "hmac", "jwt", "session", "csrf", "xsrf"};
    for (const auto& mark : marks) if (lower.contains(mark)) return true;
    // "auth" but not "author"; "key" but not "keyword" or "keyboard".
    static const QRegularExpression part("auth(?!or(?!i[sz]))|key(?!word|board)");
    // Short names count as a whole word only.
    static const QRegularExpression word("(?:^|[^a-z0-9])(?:pass|sid|sig|pin|otp)(?:$|[^a-z0-9])");
    return lower.contains(part) || lower.contains(word);
}
bool lettersAndDigits(const QString& text) {
    bool letter = false, digit = false;
    for (const auto c : text) { if (c.isDigit()) digit = true; else if (c.isLetter()) letter = true; }
    return letter && digit;
}
// What of one stored value is looked for in text that came back from a server. Not the
// value as such: an imported argument such as a folder name is stored like a token, and
// an ordinary word must not make every link that repeats it suspect. Looked for is what
// is credential-like:
//  - the whole value of a row whose name says it is a credential, and of every "other
//    secret value", from four characters; and its words of eight or more, so the token
//    of "Bearer <token>" is found alone;
//  - a key-like run anywhere in any value, by the rule the import uses for addresses:
//    sixteen or more letters and digits, or twenty or more with "-" and "_", holding a
//    letter and a digit. A key inside a stored JSON body or a cookie is found by itself;
//  - a value that the stored text itself names as a credential: "password":"…",
//    token=…, <secret>…</secret>, from four characters.
QStringList credentialTexts(const QString& key, const QString& value) {
    QStringList out;
    const auto add = [&](const QString& text) { if (!text.isEmpty() && !out.contains(text)) out.append(text); };
    static const QRegularExpression space("\\s+"), setting("^(?:true|false|null|none|undefined|[0-9.]{1,7})$", QRegularExpression::CaseInsensitiveOption);
    static const QRegularExpression scheme("^(?:bearer|basic|digest|token)\\s+", QRegularExpression::CaseInsensitiveOption);
    const auto credential = [&](const QString& text) {
        // A switch or a small number under a credential's name ("use_token": true) is neither.
        if (text.size() >= 4 && !setting.match(text).hasMatch()) add(text);
        for (const auto& word : text.split(space, Qt::SkipEmptyParts)) if (word.size() >= 8) add(word);
        // "Bearer <token>": the token by itself, however short.
        if (const auto match = scheme.match(text); match.hasMatch() && text.size() - match.capturedLength() >= 4) add(text.mid(match.capturedLength()).trimmed());
    };
    const auto slash = key.indexOf('/');
    if (key.startsWith(extraSecretsList() + '/') || (slash >= 0 && credentialName(key.mid(slash + 1)))) credential(value);
    static const QRegularExpression run("[A-Za-z0-9_-]+"), plain("[A-Za-z0-9]+");
    for (auto it = run.globalMatch(value); it.hasNext();) {
        const auto text = it.next().captured();
        if (text.size() >= 20 && lettersAndDigits(text)) add(text);
        for (auto inner = plain.globalMatch(text); inner.hasNext();)
            if (const auto part = inner.next().captured(); part.size() >= 16 && lettersAndDigits(part)) add(part);
    }
    static const QRegularExpression member("([A-Za-z_][A-Za-z0-9_.-]{0,63})[\"']?\\s*[:=]\\s*(?:(?i:bearer|basic|digest|token)\\s+)?(?:\"((?:[^\"\\\\]|\\\\.){4,})\"|'([^']{4,})'|([^\"'&;,\\s<>{}()\\[\\]]{4,}))");
    for (auto it = member.globalMatch(value); it.hasNext();) {
        const auto match = it.next();
        if (!credentialName(match.captured(1))) continue;
        auto text = match.captured(2) + match.captured(3) + match.captured(4);
        if (!match.captured(2).isEmpty()) text.replace("\\\"", "\"").replace("\\/", "/").replace("\\\\", "\\");
        credential(text);
    }
    static const QRegularExpression element("<([A-Za-z_][A-Za-z0-9_.:-]{0,63})(?:\\s[^<>]*)?>\\s*([^<>\\s](?:[^<>]{2,}[^<>\\s]))\\s*</");
    for (auto it = element.globalMatch(value); it.hasNext();) {
        const auto match = it.next();
        if (credentialName(match.captured(1))) credential(match.captured(2));
    }
    return out;
}
// A needle written in base64 inside a longer run: the characters that depend on its
// bytes alone, for each of the three places it can start, in both alphabets.
QStringList base64Forms(const QString& needle) {
    QStringList out;
    const auto bytes = needle.toUtf8();
    for (int lead = 0; lead < 3; ++lead) {
        const auto encoded = (QByteArray(lead, '\0') + bytes).toBase64(QByteArray::Base64Encoding | QByteArray::OmitTrailingEquals);
        const qsizetype skip = lead == 0 ? 0 : lead + 1, whole = (qsizetype(lead) + bytes.size()) * 8 / 6;
        const auto core = QString::fromLatin1(encoded.mid(skip, whole - skip));
        // Shorter than this it would be found by chance.
        if (core.size() < 12) continue;
        out.append(core);
        if (const auto other = QString(core).replace('+', '-').replace('/', '_'); other != core) out.append(other);
    }
    return out;
}
// One pattern for every mixture of spellings: each character as itself, as its
// percent-encoded bytes (either case), or as JSON writes it.
QString eitherCase(const QByteArray& hex) {
    QString out;
    for (const auto digit : hex) out += (digit >= 'a' && digit <= 'f') ? QString("[%1%2]").arg(QChar(digit)).arg(QChar(digit).toUpper()) : QString(QChar(digit));
    return out;
}
QRegularExpression spellings(const QString& needle) {
    QString pattern;
    for (const auto c : needle) {
        QString encoded;
        for (const auto byte : QString(c).toUtf8()) encoded += "%" + eitherCase(QByteArray(1, byte).toHex());
        QStringList forms{QRegularExpression::escape(QString(c)), encoded};
        if (c == '/') forms.append(QStringLiteral("\\\\/"));
        if (c == '"') forms.append(QStringLiteral("\\\\\""));
        if (c == '\\') forms.append(QStringLiteral("\\\\\\\\"));
        if (c == ' ') forms.append(QStringLiteral("\\+"));
        if (c.unicode() > 0x7e) forms.append("\\\\u" + eitherCase(QByteArray::number(c.unicode(), 16).rightJustified(4, '0')));
        pattern += "(?:" + forms.join('|') + ")";
    }
    return QRegularExpression(pattern);
}
QString jsonUnescaped(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == '\\' && i + 1 < text.size()) {
            const auto next = text[i + 1];
            if (next == '/' || next == '"' || next == '\\') { out += next; ++i; continue; }
            if (next == 'u' && i + 5 < text.size()) {
                bool ok = false;
                const auto code = text.mid(i + 2, 4).toUShort(&ok, 16);
                if (ok) { out += QChar(code); i += 5; continue; }
            }
        }
        out += text[i];
    }
    return out;
}
// The text without what does not show: control and format characters. A value echoed
// with one inside it reads as the value once the text is made fit to show.
QString visible(const QString& text) {
    QString out;
    out.reserve(text.size());
    for (const auto c : text) if (c.category() != QChar::Other_Control && c.category() != QChar::Other_Format) out.append(c);
    return out;
}
// The text as it reads once its escapes are undone, in the orders a server nests them.
QStringList readings(const QString& text) {
    QStringList out{text};
    const auto add = [&](const QString& reading) { if (!out.contains(reading)) out.append(reading); };
    add(visible(text));
    add(jsonUnescaped(text));
    // Percent-encoding undone, up to three layers deep, with "+" read as a space too.
    for (int layer = 0; layer < 3; ++layer)
        for (const auto& reading : QStringList(out)) {
            add(QUrl::fromPercentEncoding(reading.toUtf8()));
            add(QUrl::fromPercentEncoding(QString(reading).replace('+', ' ').toUtf8()));
        }
    return out;
}
bool wordAt(const QString& text, qsizetype start, qsizetype length) {
    const auto inside = [&](qsizetype i) { return i >= 0 && i < text.size() && text[i].isLetterOrNumber(); };
    return !inside(start - 1) && !inside(start + length);
}
// Whether the needle stands in the text: anywhere when it is long, as a whole word otherwise.
bool stands(const QString& text, const QString& needle) {
    for (auto at = text.indexOf(needle); at >= 0; at = text.indexOf(needle, at + 1))
        if (needle.size() >= 16 || wordAt(text, at, needle.size())) return true;
    return false;
}
// Longer needles are matched as written only: a pattern for them would be enormous.
constexpr qsizetype longestPattern = 256;
bool holdsBase64(const QStringList& texts, const QString& needle) {
    for (const auto& form : base64Forms(needle)) for (const auto& text : texts) if (text.contains(form)) return true;
    return false;
}
qsizetype afterAuthority(const QString& address) {
    const auto authority = address.indexOf("://");
    if (authority < 0) return 0;
    const auto path = address.indexOf(QRegularExpression("[/?#]"), authority + 3);
    return path < 0 ? address.size() : path;
}
}
QString scrubStoredValues(const QString& input, const QHash<QString, QString>& values) {
    static const QString mark = QStringLiteral("<redacted>");
    auto text = input;
    QStringList all;
    for (auto it = values.cbegin(); it != values.cend(); ++it) for (const auto& needle : credentialTexts(it.key(), it.value())) if (!all.contains(needle)) all.append(needle);
    std::sort(all.begin(), all.end(), [](const QString& a, const QString& b) { return a.size() > b.size(); });
    for (const auto& needle : all) {
        if (needle.size() <= longestPattern) text.replace(spellings(needle), mark);
        else text.replace(needle, mark);
    }
    // A spelling that could not be rewritten in place: a value encoded twice, written in
    // base64, or with a character inside it that does not show.
    const auto texts = readings(text);
    for (const auto& needle : all) {
        for (const auto& reading : texts) if (reading.contains(needle)) return QStringLiteral("<withheld: the server's text repeats a stored value>");
        if (holdsBase64(texts, needle)) return QStringLiteral("<withheld: the server's text repeats a stored value>");
    }
    return text;
}
QString storedValueIn(const QString& address, const QHash<QString, QString>& values) {
    const auto tail = address.mid(afterAuthority(address));
    if (tail.isEmpty()) return {};
    auto keys = values.keys();
    std::sort(keys.begin(), keys.end());
    const auto texts = readings(tail);
    for (const auto& key : std::as_const(keys))
        for (const auto& needle : credentialTexts(key, values.value(key))) {
            for (const auto& reading : texts) if (stands(reading, needle)) return key;
            if (holdsBase64(texts, needle)) return key;
            if (needle.size() > longestPattern) continue;
            // Mixed spellings no single reading undoes.
            for (auto it = spellings(needle).globalMatch(tail); it.hasNext();) {
                const auto match = it.next();
                if (needle.size() >= 16 || wordAt(tail, match.capturedStart(), match.capturedLength())) return key;
            }
        }
    return {};
}
QString nameStoredValues(const QString& address, const QHash<QString, QString>& values) {
    QList<QPair<QString, QString>> exact; // value → key, longest first
    auto keys = values.keys();
    std::sort(keys.begin(), keys.end());
    // Only a value that is itself looked for can be named in its own place.
    for (const auto& key : std::as_const(keys))
        if (const auto value = values.value(key); key != dataSecretKey() && credentialTexts(key, value).contains(value) && stands(address, value)) exact.append({value, key});
    std::sort(exact.begin(), exact.end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    if (exact.isEmpty()) return {};
    const auto from = afterAuthority(address);
    QString out = escapeTemplate(address.left(from)), rest = address.left(from);
    bool named = false;
    for (qsizetype i = from; i < address.size();) {
        bool matched = false;
        for (const auto& [value, key] : exact) {
            if (!QStringView(address).mid(i).startsWith(value) || (value.size() < 16 && !wordAt(address, i, value.size()))) continue;
            out += "{secret:" + escapeTemplate(key) + "}";
            i += value.size();
            matched = named = true;
            break;
        }
        if (matched) continue;
        out += escapeTemplate(QString(address[i]));
        rest += address[i];
        ++i;
    }
    // Whatever is left as text must hold none of them in any other spelling.
    if (!named || !storedValueIn(rest, values).isEmpty()) return {};
    return out;
}
QString incompleteReply(int status, const QByteArray& verb, const QList<QPair<QByteArray, QByteArray>>& headers, qint64 received) {
    // These carry no body, whatever their headers say.
    if (verb.compare("HEAD", Qt::CaseInsensitive) == 0 || (status >= 100 && status < 200) || status == 204 || status == 304) return {};
    qint64 declared = -1;
    for (const auto& [name, value] : headers) {
        // Chunked wins over a Content-Length beside it, and declares no length.
        if (name.compare("transfer-encoding", Qt::CaseInsensitive) == 0 && value.toLower().contains("chunked")) return {};
        if (name.compare("content-length", Qt::CaseInsensitive) != 0) continue;
        // "5, 5" is one length said twice. Two different ones, or no number, is no length.
        for (const auto& part : value.split(',')) {
            bool ok = false;
            const auto length = part.trimmed().toLongLong(&ok);
            if (!ok || length < 0 || (declared >= 0 && length != declared)) return {};
            declared = length;
        }
    }
    if (declared < 0 || received >= declared) return {};
    return QStringLiteral("it ended after %1 of %2 bytes").arg(received).arg(declared);
}
QUrl requestOrigin(const QJsonObject& settings) {
    const auto text = settings["requestUrl"].toString();
    const auto authority = text.indexOf("://");
    if (authority < 0) return {};
    const auto path = text.indexOf(QRegularExpression("[/?#]"), authority + 3);
    return QUrl(path < 0 ? text : text.left(path), QUrl::StrictMode);
}
namespace {
// IPv4 as the resolver reads it: one to four parts, each decimal, octal or hex.
bool ipv4Number(const QString& host, quint32& out) {
    const auto parts = host.split('.');
    if (parts.size() > 4) return false;
    quint64 address = 0;
    for (qsizetype i = 0; i < parts.size(); ++i) {
        bool ok = false;
        const auto& part = parts[i];
        const quint64 number = part.startsWith("0x") ? part.mid(2).toULongLong(&ok, 16) : part.size() > 1 && part.startsWith('0') ? part.toULongLong(&ok, 8) : part.toULongLong(&ok, 10);
        if (!ok) return false;
        const bool lastPart = i == parts.size() - 1;
        const quint64 limit = lastPart ? (quint64(1) << (8 * (5 - parts.size()))) : 256;
        if (number >= limit) return false;
        address = lastPart ? (address << (8 * (5 - parts.size()))) | number : (address << 8) | number;
    }
    out = quint32(address);
    return true;
}
// The eight groups of an IPv6 address: hexadecimal groups, one "::", and a dotted IPv4
// address in place of the last two.
bool ipv6Groups(QString host, QList<quint16>& out) {
    if (const auto zone = host.indexOf('%'); zone >= 0) host = host.left(zone);
    const auto gap = host.indexOf("::");
    if (gap >= 0 && host.indexOf("::", gap + 1) >= 0) return false;
    const auto side = [](const QString& text, bool end, QList<quint16>& groups) {
        if (text.isEmpty()) return true;
        const auto pieces = text.split(':');
        for (qsizetype i = 0; i < pieces.size(); ++i) {
            const auto& piece = pieces[i];
            if (piece.contains('.')) {
                quint32 four = 0;
                if (!end || i != pieces.size() - 1 || piece.count('.') != 3 || !ipv4Number(piece, four)) return false;
                groups.append(quint16(four >> 16));
                groups.append(quint16(four & 0xffff));
                continue;
            }
            bool ok = false;
            const auto group = piece.toUInt(&ok, 16);
            if (!ok || piece.isEmpty() || piece.size() > 4) return false;
            groups.append(quint16(group));
        }
        return true;
    };
    QList<quint16> head, tail;
    if (gap < 0) {
        if (!side(host, true, head) || head.size() != 8) return false;
        out = head;
        return true;
    }
    if (!side(host.left(gap), false, head) || !side(host.mid(gap + 2), true, tail) || head.size() + tail.size() > 7) return false;
    out = head;
    while (out.size() + tail.size() < 8) out.append(0);
    out += tail;
    return true;
}
// An address in one spelling, so two spellings of it compare equal: IPv4 as the mapped
// IPv6 address.
bool canonicalAddress(const QString& text, QList<quint16>& out) {
    auto host = text.trimmed().toLower();
    if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
    if (host.contains(':')) return ipv6Groups(host, out);
    quint32 four = 0;
    if (host.isEmpty() || !ipv4Number(host, four)) return false;
    out = {0, 0, 0, 0, 0, 0xffff, quint16(four >> 16), quint16(four & 0xffff)};
    return true;
}
}
bool loopbackAddress(const QString& name) {
    auto host = name.trimmed().toLower();
    if (host.startsWith('[') && host.endsWith(']')) host = host.mid(1, host.size() - 2);
    while (host.endsWith('.')) host.chop(1);
    if (host.isEmpty()) return false;
    if (host == "localhost" || host.endsWith(".localhost")) return true;
    if (host.contains(':')) {
        // IPv6: the loopback and unspecified addresses, and an IPv4 one mapped into it or
        // written in its last two groups, in hexadecimal as well as dotted.
        QList<quint16> groups;
        if (!ipv6Groups(host, groups)) return false;
        for (int i = 0; i < 5; ++i) if (groups[i] != 0) return false;
        if (groups[5] != 0 && groups[5] != 0xffff) return false;
        const quint32 four = (quint32(groups[6]) << 16) | groups[7];
        if (groups[5] == 0) return four <= 1 || (four >> 24) == 127;
        return four == 0 || (four >> 24) == 127;
    }
    quint32 address = 0;
    return ipv4Number(host, address) && (address == 0 || (address >> 24) == 127);
}
bool leadsToThisMachine(const QString& host, const QStringList& resolved, const QStringList& own) {
    if (loopbackAddress(host)) return true;
    QList<QList<quint16>> mine;
    for (const auto& address : own) if (QList<quint16> groups; canonicalAddress(address, groups)) mine.append(groups);
    auto addresses = resolved;
    addresses.append(host);
    for (const auto& address : addresses) {
        if (loopbackAddress(address)) return true;
        if (QList<quint16> groups; canonicalAddress(address, groups) && mine.contains(groups)) return true;
    }
    return false;
}
QString returnedAddressProblem(const QUrl& address, const QUrl& origin) {
    if (!address.userInfo().isEmpty()) return QStringLiteral("it carries a user name, which hides where it really goes");
    if (loopbackAddress(address.host()) && !loopbackAddress(origin.host())) return QStringLiteral("it points at this computer");
    return {};
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
        // The link is recorded and shared, its thumbnail recorded, an error message shown
        // and logged. Only the deletion URL is kept with its stored values still named.
        if (QLatin1String(key) != QLatin1String("deletionUrl") && !secretReferences(content).isEmpty())
            return bad(QString(key) + ": {secret:…} cannot be used here; this value is shown and recorded, so it must not contain a stored secret", key);
    }
    if (!integer(s["connectTimeoutMs"], 1000, 120000) || !integer(s["requestTimeoutMs"], 1000, 3600000))
        return bad("timeouts must be whole milliseconds within their limits", "requestTimeoutMs");
    return {};
}

namespace {
SxcuImport readSxcu(const QByteArray& json);
}
QString plainHttpNote() {
    return QStringLiteral("The address uses plain HTTP, which sends files and credentials unencrypted. Turn on Allow plain HTTP to use it.");
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
        const int flags = destination.toInt(-1);
        if (flags < 0 || flags > 31) { out.error = sxcuError("sxcu_invalid", "This file names a destination type put.gd does not know."); return out; }
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
        for (const auto& [name, raw] : std::as_const(entries)) {
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

    // A credential written out again elsewhere in the file is named there, not repeated:
    // XBackBone, for one, ends its deletion URL with the upload token. Only literal text
    // after the host counts: inside a placeholder the same letters are a path or a
    // pattern, and a host name is no key. What counts as a credential is what is looked
    // for in a server's replies (credentialTexts): a folder name that an argument also
    // holds stays an ordinary word in every address. `whole` are the values that can be
    // named in their own place; `known` adds the key-like parts of longer values.
    QList<QPair<QString, QString>> known, whole; // text → key, longest first
    static const QRegularExpression syntax("[{}|\\\\]");
    for (auto it = out.secrets.cbegin(); it != out.secrets.cend(); ++it)
        for (const auto& text : credentialTexts(it.key(), it.value())) {
            if (text.contains(syntax)) continue;
            known.append({text, it.key()});
            if (text == it.value() && it.key() != dataSecretKey()) whole.append({text, it.key()});
        }
    for (auto* list : {&known, &whole}) std::sort(list->begin(), list->end(), [](const auto& a, const auto& b) { return a.first.size() > b.first.size(); });
    // The host ends at the path, or at "?" or "#" when the address has none.
    const auto afterHost = [](const QString& value) { return afterAuthority(value); };
    // Walks the literal text of a template from `from` on. `met` is told of each text of
    // `list` found there (a short one only as a whole word) and returns what stands in
    // its place.
    const auto literal = [&](const QString& value, qsizetype from, const QList<QPair<QString, QString>>& list, const std::function<QString(const QString& key, const QString& secret)>& met) {
        QString result;
        int depth = 0;
        for (qsizetype i = 0; i < value.size();) {
            const auto c = value[i];
            if (c == '\\' && i + 1 < value.size()) { result += value.mid(i, 2); i += 2; continue; }
            if (c == '{') ++depth;
            else if (c == '}' && depth > 0) --depth;
            else if (depth == 0 && i >= from) {
                bool replaced = false;
                for (const auto& [secret, key] : list)
                    if (QStringView(value).mid(i).startsWith(secret) && (secret.size() >= 16 || wordAt(value, i, secret.size()))) { result += met(key, secret); i += secret.size(); replaced = true; break; }
                if (replaced) continue;
            }
            result += c;
            ++i;
        }
        return result;
    };
    const auto named = [&](const QString& value) {
        return literal(value, afterHost(value), whole, [](const QString& key, const QString&) {
            QString name;
            for (const auto c : key) { if (QStringLiteral("{}|\\").contains(c)) name.append('\\'); name.append(c); }
            return "{secret:" + name + "}";
        });
    };
    // The stored values a text repeats. A link is shared and recorded, and an error
    // message is shown and logged: neither may carry one, named or written out.
    const auto repeated = [&](const QString& value) {
        QStringList labels;
        literal(value, afterHost(value), known, [&](const QString& key, const QString& secret) { if (!labels.contains(secretLabel(key))) labels.append(secretLabel(key)); return secret; });
        return labels;
    };
    QJsonArray extra;
    bool tooManyKeys = false;
    QStringList moved; // the names of the stored values made from key-like runs
    // An opaque run of letters and digits left in an address is treated as a key and
    // moves to a secret value of its own: sixteen characters or more of letters and
    // digits alone (a hex key), or twenty or more with "-" and "_" among them.
    const auto opaque = [&](QString value, const QString& name, qsizetype from) {
        static const QRegularExpression run("(?<![A-Za-z0-9_-])(?:[A-Za-z0-9_-]{20,}|[A-Za-z0-9]{16,})(?![A-Za-z0-9_-])"), digit("[0-9]"), letter("[A-Za-z]");
        const auto next = [&] {
            for (auto it = run.globalMatch(value, from); it.hasNext();) {
                const auto match = it.next();
                const auto text = match.captured();
                // Inside a placeholder it is a path or pattern, not a key.
                const auto before = value.left(match.capturedStart());
                if (before.count('{') > before.count('}') || !text.contains(digit) || !text.contains(letter)) continue;
                return match;
            }
            return QRegularExpressionMatch();
        };
        for (auto found = next(); found.hasMatch(); found = next()) {
            // More than can be stored: nothing is left behind in the address.
            if (extra.size() >= maximumRows) { tooManyKeys = true; break; }
            const auto row = name + (extra.isEmpty() ? QString() : "-" + QString::number(extra.size() + 1));
            extra.append(QJsonObject{{"name", row}, {"secret", true}});
            moved.append(row);
            out.secrets.insert(secretKey(extraSecretsList(), row), found.captured());
            value = value.left(found.capturedStart()) + "{secret:" + secretKey(extraSecretsList(), row) + "}" + value.mid(found.capturedEnd());
        }
        return value;
    };
    // A part of a credential written out in an address, such as the token of a header
    // "Bearer <token>", cannot be named by that row: it becomes a stored value of its own.
    const auto parts = [&](const QString& value, const QString& name) {
        return literal(value, afterHost(value), known, [&](const QString&, const QString& secret) -> QString {
            if (extra.size() >= maximumRows) { tooManyKeys = true; return secret; }
            const auto row = name + (extra.isEmpty() ? QString() : "-" + QString::number(extra.size() + 1));
            extra.append(QJsonObject{{"name", row}, {"secret", true}});
            moved.append(row);
            out.secrets.insert(secretKey(extraSecretsList(), row), secret);
            return "{secret:" + secretKey(extraSecretsList(), row) + "}";
        });
    };
    {
        auto address = parts(named(requestUrl), "url-key");
        address = opaque(address, "url-key", afterHost(address));
        if (address != requestUrl) { settings["requestUrl"] = address; requestUrl = address; }
    }
    auto responseUrl = migrate(fieldText("URL"));
    if (const auto labels = repeated(responseUrl); !labels.isEmpty()) {
        out.error = sxcuError("sxcu_unsupported", QString("URL repeats the value of %1. put.gd records and shares the link, so it cannot contain a credential. Remove that value from the file's URL. "
                                                          "If it is no credential (an album or a user ID, say), set this destination up by hand and leave Secret off for it.").arg(labels.join(", ")));
        return out;
    }
    // Before 12.4 a ResponseType said where the link was when URL was empty.
    if (responseUrl.isEmpty()) {
        const auto type = fieldText("ResponseType").trimmed().toLower();
        if (type == "redirectionurl") responseUrl = "{responseurl}";
        else if (type == "locationheader") responseUrl = "{header:Location}";
    }
    settings["responseUrl"] = responseUrl;
    for (const auto& [key, sxcuName] : QList<QPair<const char*, const char*>>{{"thumbnailUrl", "ThumbnailURL"}, {"errorMessage", "ErrorMessage"}}) {
        auto value = migrate(fieldText(sxcuName));
        if (const auto labels = repeated(value); !labels.isEmpty()) {
            out.notes.append(QString("%1 repeated the value of %2 and was left out: it would be shown and recorded.").arg(sxcuName, labels.join(", ")));
            value.clear();
        }
        settings[key] = value;
    }
    {
        auto deletion = parts(named(migrate(fieldText("DeletionURL"))), "deletion-key");
        // After the host: the host name itself is no key.
        deletion = opaque(deletion, "deletion-key", afterHost(deletion));
        settings["deletionUrl"] = deletion;
    }
    if (tooManyKeys) { out.error = sxcuError("sxcu_unsupported", "The file's addresses hold more key-like values than a destination can store."); return out; }
    // Said because no rule can tell a key from an album or a user ID of the same shape,
    // and a link that repeats a stored key is not recorded.
    if (!moved.isEmpty())
        out.notes.append(QString("Part of an address was taken for a key and is now stored as %1. If it is an album or a user ID that the server's links repeat, "
                                 "write it back into the address and remove it from Other secret values.").arg(moved.join(", ")));
    // No rule finds every key. A shorter part that mixes letters and digits stays as
    // written, and the note says where it is, never what it is.
    const auto keptParts = [&](const QString& address) {
        static const QRegularExpression keyLike("^(?=.*[0-9])(?=.*[A-Za-z])[A-Za-z0-9_-]{6,}$"), separator("[/?&=#]");
        QString plain;
        int depth = 0;
        for (const auto c : address.mid(afterHost(address))) {
            if (c == '{') ++depth;
            plain += depth > 0 ? QChar('{') : c;
            if (c == '}' && depth > 0) --depth;
        }
        QStringList positions;
        int position = 0;
        for (const auto& part : plain.split(separator, Qt::SkipEmptyParts)) {
            ++position;
            if (keyLike.match(part).hasMatch()) positions.append(QString::number(position));
        }
        return positions;
    };
    for (const auto& [key, what] : QList<QPair<const char*, QString>>{{"requestUrl", "request URL"}, {"deletionUrl", "deletion URL"}})
        if (const auto positions = keptParts(settings[key].toString()); !positions.isEmpty())
            out.notes.append(QString("The %1 keeps part %2 after its host as written. If that is a key, move it to Other secret values and write {secret:secrets/name} in its place.").arg(what, positions.join(" and ")));
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
    if (!transportAllowed(url, false)) out.notes.append(plainHttpNote());
    out.name = boundedMessage(fieldText("Name"), 96);
    if (out.name.isEmpty()) out.name = url.host();
    out.settings = settings;
    return out;
}
}
}
