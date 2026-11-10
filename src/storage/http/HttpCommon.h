#pragma once
#include "core/Error.h"
#include <QHash>
#include <QJsonObject>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QUrl>
namespace putgd::http {
// Custom HTTP uploader destination (#1361): one request the user describes, or imports
// from a ShareX .sxcu file, and placeholders that read the reply. These rules are shared
// by the resident, the Settings form and putgd-http-worker. Qt Core only: no network
// types, so the resident links none.
inline constexpr char providerId[] = "custom-http";
inline constexpr int maximumRows = 32;            // headers, parameters or arguments
inline constexpr int maximumNameChars = 256;
inline constexpr int maximumValueChars = 8192;
inline constexpr int maximumDataChars = 65536;
inline constexpr int maximumTemplateChars = 4096;
inline constexpr qsizetype maximumSxcuBytes = 256 * 1024;
inline constexpr qsizetype maximumSecretBytes = 96 * 1024; // every stored value of one destination
inline constexpr qint64 maximumInputBytes = 16LL * 1024 * 1024; // text sent through {input}

// The three lists whose values are secret unless a row says otherwise.
const QStringList& rowLists();
// "secrets": values with no row of their own, named so {secret:secrets/name} can use them
// in the request URL or the deletion URL. Always secret.
inline QString extraSecretsList() { return QStringLiteral("secrets"); }
QJsonObject defaults();
// Pure: reads no file, unlocks nothing and contacts nothing. A row marked secret must
// carry no value; its value lives in the credential store under secretKey().
Error validateSettings(const QJsonObject&);
QString secretKey(const QString& list, const QString& name);
inline QString dataSecretKey() { return QStringLiteral("data"); }
// The credential-store keys this destination's request needs.
QStringList secretKeys(const QJsonObject& settings);
// A short name for a stored value, as Settings and messages show it ("header Authorization").
QString secretLabel(const QString& key);
// What a file is to an uploader: image, text or file (anything else, recordings included).
QString artifactKind(const QString& mime, const QString& category);
// Whether this uploader takes that kind: "file" takes everything.
bool acceptsKind(const QJsonObject& settings, const QString& kind);
bool loopbackHost(const QString& host);
// HTTPS, or HTTP to a literal loopback host, or any HTTP when the destination allows it.
bool transportAllowed(const QUrl&, bool allowInsecureHttp);
// A redirect stays on the same host when the host name matches and HTTPS is not given up.
bool sameHost(const QUrl& from, const QUrl& to);
// Text from a server as one line: line breaks and tabs become a space, control
// characters go. What is shown is cleaned of stored values after this, never before: a
// value echoed around a control character reads as the value once that character is gone.
QString singleLine(const QString&);
// Text from a server shown to the user: one line, bounded, control characters removed.
QString boundedMessage(const QString&, int maximum = 300);

// A server may repeat what it was sent. The helpers below find a destination's
// credentials in text that came back, in the spellings a server writes them: as sent,
// percent-encoded in any mixture, JSON-escaped ("/" as "\/", as PHP does), in base64,
// and around characters that do not show.
//
// Not every stored value is a credential. An imported argument such as a folder name is
// stored like a token, and a link that repeats that word is an ordinary link. Looked
// for is what is credential-like: the value (from four characters, and its words of
// eight or more) of a row whose name says so ("Authorization", "api_key", "token",
// "password", "cookie" and the like) and of every other secret value; a key-like run
// inside any value (sixteen or more letters and digits, or twenty or more with "-" and
// "_", holding a letter and a digit), so a key in a stored JSON body or a cookie is
// found by itself; and a value the stored text itself names as a credential
// ("password":"…", token=…). Ordinary words and short settings are not looked for.
//
// Server text made fit to keep: every spelling of every credential replaced. When one
// survives in a spelling this cannot rewrite, the whole text is withheld.
QString scrubStoredValues(const QString& text, const QHash<QString, QString>& values);
// The credential-store key of a credential that an address holds after its host, or
// empty. The host is where the address points and is no secret. A credential under
// sixteen characters must stand as a whole word.
QString storedValueIn(const QString& address, const QHash<QString, QString>& values);
// The address as a template that names each credential standing in it exactly as
// stored, "{secret:key}", with everything else escaped. Empty when none stands there
// exactly, or when one would remain in another spelling or in part.
QString nameStoredValues(const QString& address, const QHash<QString, QString>& values);
// Scheme, host and port of a destination's request URL: where its values may go.
QUrl requestOrigin(const QJsonObject& settings);
// This machine, in any spelling an address can use: localhost, 127.0.0.0/8 (decimal,
// octal, hex or as one number), ::1, the unspecified address, and an IPv4 one of these
// inside an IPv6 address ("::ffff:7f00:1").
bool loopbackAddress(const QString& host);
// Where a name leads is the resolver's to say, not its spelling's: "ip6-localhost", the
// machine's own host name and a public name that answers 127.0.0.1 all lead here.
// `resolved` is what the host resolves to (empty for a literal address) and `own` the
// addresses of this machine's interfaces.
bool leadsToThisMachine(const QString& host, const QStringList& resolved, const QStringList& own);
// An address the reply named, judged against the request's own. Empty when usable.
// It may not carry user information ("https://shown@real/" goes to "real"), and it may
// point at this machine only when the request itself does.
QString returnedAddressProblem(const QUrl& address, const QUrl& origin);

// A .sxcu file read into destination settings. Every header, parameter and argument
// value and the Data body are secrets: they are returned in `secrets` (credential-store
// key → value) and never appear in `settings`, except a value that is exactly
// {filename} or {input}. A credential (see above: not every stored value is one) that
// the file repeats as literal text after the host of the request URL or the deletion
// URL, as some services end their deletion URL with the token, becomes {secret:key}
// there. An ordinary word that a row also holds stays as written. A long run of letters
// and digits left in those two addresses becomes a secret value of its own, with a
// note. A link that repeats a credential is refused; a thumbnail URL or error message
// that does is left out with a note.
// Nothing here is written anywhere.
// The note an import carries when its address is plain HTTP to another machine.
QString plainHttpNote();
struct SxcuImport {
    Error error;
    QString name;                    // suggested destination name
    QJsonObject settings;
    QMap<QString, QString> secrets;
    QStringList notes;               // what was adapted or left out, for the user
};
SxcuImport importSxcu(const QByteArray& json);
}
