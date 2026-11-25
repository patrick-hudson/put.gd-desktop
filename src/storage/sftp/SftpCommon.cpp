#include "SftpCommon.h"
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QUrl>
#include <sys/stat.h>
#include <unistd.h>
namespace putgd::sftp {
namespace {
QRegularExpression whole(const char* pattern) { return QRegularExpression(QRegularExpression::anchoredPattern(QString::fromLatin1(pattern))); }
// A host name or ~/.ssh/config alias: letters, digits, dots, hyphens and underscores, not
// starting with a hyphen (ssh would read an option) or a dot.
const QRegularExpression& namePattern() { static const auto value = whole("[A-Za-z0-9_][A-Za-z0-9_.-]{0,252}"); return value; }
// An IPv6 literal, with an optional zone (fe80::1%eth0).
const QRegularExpression& v6Pattern() { static const auto value = whole("[0-9A-Fa-f:.]{2,45}(%[A-Za-z0-9_.-]{1,32})?"); return value; }
const QRegularExpression& userPattern() { static const auto value = whole("[A-Za-z0-9_][A-Za-z0-9_.$-]{0,127}"); return value; }
bool controlFree(const QString& text) {
    for (const auto c : text) if (c.unicode() < 0x20 || c.unicode() == 0x7f) return false;
    return true;
}
bool hostValid(const QString& host) {
    if (host.contains(':')) return v6Pattern().match(host).hasMatch() && host.count(':') >= 2;
    return namePattern().match(host).hasMatch();
}
// host, host:port, [v6], [v6]:port or a bare v6 literal, into `address`.
bool hostPort(const QString& text, Address& address, QString* error, bool allowPath) {
    const auto fail = [error](const QString& why) { if (error) *error = why; return false; };
    QString rest;
    if (text.startsWith('[')) {
        const auto close = text.indexOf(']');
        if (close < 0) return fail(QStringLiteral("An IPv6 address in brackets needs its closing ]."));
        address.host = text.mid(1, close - 1);
        if (!address.host.contains(':')) return fail(QStringLiteral("Brackets are only for an IPv6 address."));
        rest = text.mid(close + 1);
        if (!rest.isEmpty() && !rest.startsWith(':')) return fail(QStringLiteral("Put the port after the ] as :port."));
        rest = rest.mid(1);
    } else if (text.count(':') >= 2 && !text.contains('/')) {
        address.host = text;  // a bare IPv6 literal: no port can follow it
    } else {
        const auto colon = text.indexOf(':');
        address.host = colon < 0 ? text : text.left(colon);
        if (colon >= 0) {
            rest = text.mid(colon + 1);
            if (rest.isEmpty()) return fail(QStringLiteral("Put a port number after the colon, or remove it."));
        }
    }
    if (!rest.isEmpty()) {
        // Digits after the colon are always a port, however many: never an SCP-style path.
        static const auto digits = whole("[0-9]+");
        if (digits.match(rest).hasMatch()) {
            address.port = rest.size() > 5 ? 0 : rest.toInt();
            if (address.port < 1 || address.port > 65535) return fail(QStringLiteral("Port must be a number from 1 to 65535."));
        } else if (allowPath) {
            // user@host:path, as scp and sftp take it: the path follows the colon.
            address.folder = rest;
            address.hasFolder = true;
        } else return fail(QStringLiteral("Port must be a number from 1 to 65535."));
    }
    if (address.host.isEmpty()) return fail(QStringLiteral("Enter the server's host name or address."));
    if (!hostValid(address.host))
        return fail(address.host.startsWith('-') ? QStringLiteral("A server name can't start with a hyphen.")
                                                 : QStringLiteral("Enter a host name, an IP address or a Host alias from ~/.ssh/config, such as files.example.com."));
    return true;
}
bool userValid(const QString& user, QString* error) {
    if (user.contains(':')) { if (error) *error = QStringLiteral("Don't include a password in Server. Enter it in Password instead."); return false; }
    if (!userPattern().match(user).hasMatch()) { if (error) *error = QStringLiteral("User may only contain letters, digits, dots, hyphens, $, or underscores."); return false; }
    return true;
}
QString urlProblem(const QJsonValue& value) {
    const auto text = value.toString();
    const QUrl url(text, QUrl::StrictMode);
    if (!value.isString() || text.isEmpty() || !controlFree(text) || !url.isValid() || url.scheme() != QLatin1String("https") || url.host().isEmpty() ||
        !url.userInfo().isEmpty() || url.hasQuery() || url.hasFragment())
        return QStringLiteral("Enter the HTTPS address the folder is served at, such as https://files.example.com/shots/, without a query or fragment.");
    return {};
}
}
const QStringList& settingKeys() {
    static const QStringList keys{"server", "user", "folder", "publicUrlBase", "hostKeyFingerprint", "keyFile"};
    return keys;
}
QJsonObject defaults() {
    return {{"server", ""}, {"user", ""}, {"folder", ""}, {"publicUrlBase", QJsonValue::Null}, {"hostKeyFingerprint", QJsonValue::Null}};
}
std::optional<Address> parseServer(const QString& raw, QString* error) {
    const auto fail = [error](const QString& why) -> std::optional<Address> { if (error) *error = why; return std::nullopt; };
    const auto text = raw.trimmed();
    if (text.isEmpty()) return fail(QStringLiteral("Enter a Server hostname, host:port, or Host alias from ~/.ssh/config."));
    if (!controlFree(text) || text.contains(' ') || text.contains('\t')) return fail(QStringLiteral("Server can't contain spaces or control characters."));
    if (text.size() > 2048) return fail(QStringLiteral("The server address is too long."));
    Address address;
    QString rest = text;
    bool url = false;
    for (const auto* scheme : {"sftp://", "ssh://"})
        if (text.startsWith(QLatin1String(scheme), Qt::CaseInsensitive)) { rest = text.mid(int(qstrlen(scheme))); url = true; }
    QString path;
    if (url) {
        const auto slash = rest.indexOf('/');
        if (slash >= 0) { path = rest.mid(slash); rest = rest.left(slash); }
        if (rest.contains('?') || rest.contains('#') || path.contains('?') || path.contains('#')) return fail(QStringLiteral("Enter the address without a query or fragment."));
    }
    const auto at = rest.lastIndexOf('@');
    if (at >= 0) {
        address.user = rest.left(at);
        if (url) address.user = QUrl::fromPercentEncoding(address.user.toUtf8());
        rest = rest.mid(at + 1);
        if (!userValid(address.user, error)) return std::nullopt;
        address.hasUser = true;
    }
    if (!hostPort(rest, address, error, !url)) return std::nullopt;
    if (url && path == QLatin1String("/")) {
        // sftp://host/ names the server's root folder.
        address.folder = QStringLiteral("/");
        address.hasFolder = true;
    } else if (url && !path.isEmpty()) {
        // sftp://host/~/shots is the login folder's shots; sftp://host/srv/shots is absolute.
        auto decoded = QUrl::fromPercentEncoding(path.toUtf8());
        if (decoded.startsWith(QLatin1String("/~/")) || decoded == QLatin1String("/~")) decoded = decoded.mid(1);
        if (!controlFree(decoded)) return fail(QStringLiteral("The path has a control character in it."));
        address.folder = decoded;
        address.hasFolder = true;
    }
    return address;
}
QString withoutPassword(const QString& raw, bool* had) {
    if (had) *had = false;
    const auto text = raw.trimmed();
    qsizetype start = 0, end = text.size();
    const auto scheme = text.indexOf(QLatin1String("://"));
    if (scheme >= 0) {
        start = scheme + 3;
        const auto slash = text.indexOf('/', start);
        if (slash >= 0) end = slash;
    }
    const auto at = text.lastIndexOf('@', end - 1);
    if (at < start) return text;
    // The user part's password separator: a ':' or a percent-encoded one ("%3A", either case).
    auto colon = text.indexOf(':', start);
    const auto encoded = text.indexOf(QLatin1String("%3a"), start, Qt::CaseInsensitive);
    if (encoded >= 0 && encoded < at && (colon < 0 || encoded < colon)) colon = encoded;
    if (colon < 0 || colon > at) return text;
    if (had) *had = true;
    return text.left(colon) + text.mid(at);
}
QString withoutUserInfo(const QString& link, bool* had) {
    if (had) *had = false;
    const auto text = link.trimmed();
    const auto scheme = text.indexOf(QLatin1String("://"));
    if (scheme < 0) return text;
    const auto start = scheme + 3;
    auto end = text.size();
    for (const auto c : {'/', '?', '#'}) if (const auto i = text.indexOf(QLatin1Char(c), start); i >= 0 && i < end) end = i;
    const auto at = text.lastIndexOf('@', end - 1);
    if (at < start) return text;
    if (had) *had = true;
    return text.left(start) + text.mid(at + 1);
}
QString serverText(const QString& host, int port) {
    const auto shown = host.contains(':') ? "[" + host + "]" : host;
    return port > 0 ? shown + ":" + QString::number(port) : shown;
}
QString problem(const QJsonObject& settings, const QString& field, bool forUse) {
    const auto value = settings[field];
    if (field == QLatin1String("server")) {
        if (!value.isString()) return QStringLiteral("Enter a Server hostname, host:port, or Host alias from ~/.ssh/config.");
        if (value.toString().isEmpty())
            return forUse ? QStringLiteral("Enter a Server hostname, host:port, or Host alias from ~/.ssh/config to finish setting up this destination.") : QString();
        QString why;
        const auto address = parseServer(value.toString(), &why);
        if (!address) return why;
        if (address->hasUser || address->hasFolder || value.toString() != value.toString().trimmed())
            return QStringLiteral("Server holds the host and port only. Put the user name in User and the path in Folder on server.");
    } else if (field == QLatin1String("user")) {
        if (!value.isString()) return QStringLiteral("Enter the user name on the server, or leave it empty to use the one ssh would.");
        QString why;
        if (!value.toString().isEmpty() && !userValid(value.toString(), &why)) return why;
    } else if (field == QLatin1String("folder")) {
        const auto folder = value.toString();
        if (!value.isString() || !controlFree(folder)) return QStringLiteral("Folder on server can't contain control characters.");
        if (folder.toUtf8().size() > 4096) return QStringLiteral("Folder on server must be at most 4 KB.");
        if (folder.startsWith('~') && folder != QLatin1String("~") && !folder.startsWith(QLatin1String("~/")))
            return QStringLiteral("Write another user's folder as its full path, such as /home/alice/shots.");
    } else if (field == QLatin1String("publicUrlBase")) {
        if (!value.isNull()) return urlProblem(value);
    } else if (field == QLatin1String("keyFile")) {
        // Optional (#1892): absent or null is no chosen key.
        if (value.isUndefined() || value.isNull()) return {};
        const auto path = value.toString();
        if (!value.isString() || path.isEmpty() || !QFileInfo(path).isAbsolute() || !controlFree(path))
            return QStringLiteral("Select a private key file (such as /home/alice/keys/id_ed25519), or leave SSH key empty.");
        if (path.toUtf8().size() > 4096) return QStringLiteral("The SSH key path must be at most 4 KB.");
        // ssh expands % tokens and ${VARIABLES} in a key path, so it would open another file.
        if (path.contains('%') || path.contains(QLatin1String("${")))
            return QStringLiteral("ssh reads % and ${ in a key path as its own placeholders. Rename the key or its folder, or name the key in ~/.ssh/config.");
    } else if (field == QLatin1String("hostKeyFingerprint")) {
        if (!value.isNull() && !(value.isString() && fingerprintValid(value.toString())))
            return QStringLiteral("A trusted host key is recorded as its type and SHA256 fingerprint, such as ED25519 SHA256:….");
    }
    return {};
}
Error validate(const QJsonObject& settings, bool forUse) {
    for (auto it = settings.begin(); it != settings.end(); ++it)
        if (!settingKeys().contains(it.key())) return {"sftp_settings_invalid", "Invalid SFTP settings: " + it.key() + " is not one of them", {}};
    for (const auto& key : settingKeys())
        if (!settings.contains(key) && key != QLatin1String("keyFile")) return {"sftp_settings_invalid", "Invalid SFTP settings: " + key + " is missing", {}};
    for (const auto& field : settingKeys())
        if (const auto message = problem(settings, field, forUse); !message.isEmpty()) return {"sftp_settings_invalid", message, {{"field", field}}};
    return {};
}
QString remoteFolder(const QString& folder) {
    auto path = folder;
    if (path == QLatin1String("~")) return {};
    if (path.startsWith(QLatin1String("~/"))) path = path.mid(2);
    while (path.size() > 1 && path.endsWith('/')) path.chop(1);
    return path;
}
QString linkFor(const QJsonObject& settings, const QString& name) {
    if (!settings["publicUrlBase"].isString()) return {};
    auto base = settings["publicUrlBase"].toString();
    if (!base.endsWith('/')) base += '/';
    return base + QString::fromLatin1(QUrl::toPercentEncoding(name));
}
QString location(const QJsonObject& settings, const QString& absolutePath) {
    const auto address = parseServer(settings["server"].toString());
    const auto host = address ? serverText(address->host, 0) : settings["server"].toString();
    return host + ":" + absolutePath;
}
QString keyFile(const QJsonObject& settings) { return settings.value("keyFile").toString(); }
Error keyFileProblem(const QString& path, QString* privateKey) {
    if (privateKey) privateKey->clear();
    // "a folder/" has no file name: the message then says which field it is.
    const auto name = QFileInfo(path).fileName().isEmpty() ? QStringLiteral("chosen in SSH key") : QFileInfo(path).fileName();
    const auto encoded = QFile::encodeName(path);
    struct stat info {};
    if (::stat(encoded.constData(), &info) != 0)
        return {"ssh_key_missing", "The SSH key file " + name + " doesn't exist. Select the private key file again, or leave SSH key empty.", {{"field", "keyFile"}}};
    if (!S_ISREG(info.st_mode) || ::access(encoded.constData(), R_OK) != 0)
        return {"ssh_key_unreadable", "Can't read the SSH key file " + name + ". Select a readable private key file.", {{"field", "keyFile"}}};
    QFile file(path);
    const auto head = file.open(QIODevice::ReadOnly) ? file.read(64) : QByteArray();
    // A public key: a .pub file, or one line such as "ssh-ed25519 AAAA…" whatever its name.
    static const QRegularExpression publicLine(QStringLiteral("^(ssh|ecdsa|sk)-[A-Za-z0-9@.-]+[ \\t]+AAAA"));
    if (path.endsWith(QLatin1String(".pub")) || publicLine.match(QString::fromLatin1(head)).hasMatch()) {
        if (path.endsWith(QLatin1String(".pub"))) {
            const auto candidate = path.chopped(4);
            if (privateKey && QFileInfo(candidate).isFile()) *privateKey = candidate;
            if (QFileInfo(candidate).isFile())
                return {"ssh_key_public", name + " is a public key. Select " + QFileInfo(candidate).fileName() + ", the private key next to it.", {{"field", "keyFile"}}};
        }
        return {"ssh_key_public", name + " is a public key. Select the private key file instead (usually the same name without .pub).", {{"field", "keyFile"}}};
    }
    if (head.startsWith("PuTTY-User-Key-File"))
        return {"ssh_key_putty", name + " is a PuTTY key, which OpenSSH can't read. Convert it to OpenSSH format with PuTTYgen (Conversions → Export OpenSSH key, or puttygen -O private-openssh), then select the converted file.", {{"field", "keyFile"}}};
    // ssh refuses a private key of yours that anyone else may read (sshkey_perm_ok).
    if (info.st_uid == ::getuid() && (info.st_mode & 077) != 0)
        return {"ssh_key_permissions", "ssh refuses " + name + " because other users can read it. Make it private (chmod 600 on the file), then test again.",
                {{"field", "keyFile"}}};
    return {};
}
Error namedCredentials(const QJsonObject& profile, const QMap<QString, QString>& values, QStringList* names) {
    if (profile["provider"] != providerId) return {"setup_unsupported", "This destination does not store named values", {}};
    const QStringList keys{passwordKey(), passphraseKey()};
    if (names) *names = keys;
    for (auto it = values.cbegin(); it != values.cend(); ++it) {
        if (!keys.contains(it.key())) return {"credential_invalid", "An SFTP destination stores a password and a key passphrase, nothing else", {}};
        const auto label = it.key() == passwordKey() ? QStringLiteral("password") : QStringLiteral("key passphrase");
        if (it.value().size() > 4096) return {"credential_invalid", "The " + label + " is too long", {}};
        // ssh reads the answer up to a line break, so one inside it would cut it short.
        if (it.value().contains('\n') || it.value().contains('\r') || it.value().contains(QChar(0))) return {"credential_invalid", "The " + label + " has a line break in it", {}};
    }
    // Only the values given change; the others are kept. An empty one clears that secret.
    return {};
}
QString keyType(const QString& algorithm) {
    if (algorithm == QLatin1String("ssh-ed25519")) return QStringLiteral("ED25519");
    if (algorithm == QLatin1String("sk-ssh-ed25519@openssh.com")) return QStringLiteral("ED25519-SK");
    if (algorithm == QLatin1String("ssh-rsa")) return QStringLiteral("RSA");
    if (algorithm == QLatin1String("ssh-dss")) return QStringLiteral("DSA");
    if (algorithm.startsWith(QLatin1String("ecdsa-sha2-"))) return QStringLiteral("ECDSA");
    if (algorithm.startsWith(QLatin1String("sk-ecdsa-sha2-"))) return QStringLiteral("ECDSA-SK");
    auto upper = algorithm.toUpper();
    upper.replace(QRegularExpression(QStringLiteral("[^A-Z0-9-]")), QStringLiteral("-"));
    return upper.left(32);
}
namespace {
QStringList lineFields(const QString& line) {
    auto fields = line.trimmed().split(QRegularExpression(QStringLiteral("[ \\t]+")), Qt::SkipEmptyParts);
    if (!fields.isEmpty() && fields.first().startsWith('@')) fields.removeFirst();
    return fields;
}
}
QString hostOfLine(const QString& line) {
    const auto fields = lineFields(line);
    return fields.size() >= 3 ? fields[0] : QString();
}
QString fingerprintOfLine(const QString& line) {
    const auto fields = lineFields(line);
    if (fields.size() < 3) return {};
    const auto blob = QByteArray::fromBase64(fields[2].toLatin1(), QByteArray::AbortOnBase64DecodingErrors);
    if (blob.isEmpty()) return {};
    auto digest = QCryptographicHash::hash(blob, QCryptographicHash::Sha256).toBase64(QByteArray::OmitTrailingEquals);
    return keyType(fields[1]) + " SHA256:" + QString::fromLatin1(digest);
}
PromptMatch matchPrompt(const QString& askpassKind, const QByteArray& prompt, const QString& user, const QStringList& hosts, const QStringList& identityFiles) {
    if (askpassKind == QLatin1String("none")) {
        const auto lower = QString::fromUtf8(prompt).toLower();
        return {lower.contains(QLatin1String("touch")) || lower.contains(QLatin1String("presence")) ? PromptKind::Touch : PromptKind::Notice, {}};
    }
    if (!askpassKind.isEmpty()) return {};  // "confirm" and anything newer: no answer
    // Whole strings only, the trailing ": " included: no trimming, truncation or prefixes.
    // sshconnect2.c: "Enter passphrase for key '%.100s': " with the tilde-expanded path, cut to
    // 100 bytes. Compared as bytes, as ssh prints them.
    QStringList matched;
    for (const auto& file : identityFiles)
        if (!file.isEmpty() && prompt == "Enter passphrase for key '" + file.toUtf8().left(100) + "': ") matched.append(file);
    if (matched.size() == 1) return {PromptKind::Passphrase, matched.first()};
    if (matched.size() > 1) return {PromptKind::Ambiguous, {}};
    // "%s@%s's password: "
    if (!user.isEmpty())
        for (const auto& host : hosts)
            if (!host.isEmpty() && prompt == (user + "@" + host + "'s password: ").toUtf8()) return {PromptKind::Password, {}};
    return {};
}
QString hostToken(const QString& host, int port, const QString& alias) {
    if (!alias.isEmpty()) return alias;
    return port > 0 && port != 22 ? "[" + host + "]:" + QString::number(port) : host;
}
bool literalHostToken(const QString& token) {
    static const auto pattern = whole("[^*?,!|@ \\t]+");
    return !token.isEmpty() && pattern.match(token).hasMatch();
}
bool fingerprintValid(const QString& fingerprint) {
    static const auto pattern = whole("[A-Z0-9-]{1,32} SHA256:[A-Za-z0-9+/]{43}");
    return pattern.match(fingerprint).hasMatch();
}
}
