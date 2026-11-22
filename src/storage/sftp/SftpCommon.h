#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <optional>
namespace putgd::sftp {
// SFTP destination (#1871): the user's own OpenSSH client carries put.gd's SFTP v3
// client to their server. These rules are shared by the resident, the Settings form and
// putgd-sftp-worker. Qt Core only: no network or process code, so the resident links none.
inline const QString providerId = QStringLiteral("sftp");
// The setting keys, in the order Settings shows them.
const QStringList& settingKeys();
QJsonObject defaults();
// What the Server field says, read the way ssh reads a destination: host, host:port,
// [v6]:port, user@host[:port], a Host alias from ~/.ssh/config, or a pasted
// sftp://user@host:port/path or user@host:/path. A password in the text is refused.
struct Address {
    QString host, user, folder;
    int port = 0;  // 0: as ssh config says (22 unless it says otherwise)
    bool hasUser = false, hasFolder = false;
};
// The address, or the reason it isn't one (`error` names what to change).
std::optional<Address> parseServer(const QString& text, QString* error = nullptr);
// host, host:port or [v6]:port: how the Server field stores an address.
QString serverText(const QString& host, int port);
// What is wrong with one field (server, user, folder, publicUrlBase, hostKeyFingerprint)
// of these settings, or an empty string. An empty Server is a destination still being
// set up: `forUse` asks whether it can be used now, and then empty is a problem too.
QString problem(const QJsonObject& settings, const QString& field, bool forUse = false);
// Every field as problem() sees it. Field errors carry details.field.
Error validate(const QJsonObject& settings, bool forUse = false);
// The folder as SFTP takes it: "~" and a leading "~/" mean the login folder, which is
// where a relative path starts; empty is the login folder itself.
QString remoteFolder(const QString& folder);
// The link for a stored file: Link address + the percent-encoded name, or empty
// when the destination has no Link address.
QString linkFor(const QJsonObject& settings, const QString& name);
// "host:/absolute/path", as the upload notice and History's Copy path give it.
QString location(const QJsonObject& settings, const QString& absolutePath);
// The credential-store names of the two optional secrets.
inline QString passwordKey() { return QStringLiteral("password"); }
inline QString passphraseKey() { return QStringLiteral("passphrase"); }
// setup.storeCredentials for an SFTP destination: names (both, either may be empty) and
// the typed values' rules. Leaving both empty means the agent and key files.
Error namedCredentials(const QJsonObject& profile, const QMap<QString, QString>& values, QStringList* names);
// Host keys (#1871): "ED25519 SHA256:…", from one known_hosts line ("host type base64").
QString keyType(const QString& algorithm);
QString fingerprintOfLine(const QString& line);
// The host token of a known_hosts line (its first field), or empty.
QString hostOfLine(const QString& line);
// "ED25519 SHA256:<43 base64 characters>".
bool fingerprintValid(const QString& fingerprint);
// The worker program's name, and its askpass mode's environment variables.
inline constexpr char workerName[] = "putgd-sftp-worker";
inline constexpr char askpassSocketVariable[] = "PUTGD_SFTP_ASKPASS_SOCKET";
inline constexpr char askpassTokenVariable[] = "PUTGD_SFTP_ASKPASS_TOKEN";
// Files read back for a deletion's identity check up to this size (32 MB).
inline constexpr qint64 readBackBytes = 32LL * 1024 * 1024;
}
