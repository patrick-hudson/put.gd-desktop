#pragma once
#include "core/Error.h"
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
// Text from a server shown to the user: one line, bounded, control characters removed.
QString boundedMessage(const QString&, int maximum = 300);

// A .sxcu file read into destination settings. Every header, parameter and argument
// value and the Data body are secrets: they are returned in `secrets` (credential-store
// key → value) and never appear in `settings`, except a value that is exactly
// {filename} or {input}. A secret repeated inside the request URL or a response value,
// as some services write their deletion URL, becomes {secret:key} there. Nothing here
// is written anywhere.
struct SxcuImport {
    Error error;
    QString name;                    // suggested destination name
    QJsonObject settings;
    QMap<QString, QString> secrets;
    QStringList notes;               // what was adapted or left out, for the user
};
SxcuImport importSxcu(const QByteArray& json);
}
