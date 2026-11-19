#pragma once
#include "Error.h"
#include <QJsonObject>
#include <QString>
namespace putgd {
// What a destination's passed test is tied to (#1692). The resident keeps one record per
// destination: passed or failed, and for which connection. The fingerprint names that
// connection: the provider, the settings that decide where requests go, who signs them and
// what the test proved, and the credential reference. A label, the destination's place in
// the list, being the capture default, timeouts and share-link preferences are left out, so
// changing them keeps the record. Both the resident and the Settings window compute it, so
// an unsaved connection edit shows as untested at once.
namespace destinationcheck {
// The settings keys of each provider that enter the fingerprint.
QStringList connectionKeys(const QString& provider);
// A lowercase hex SHA-256 of the provider, those settings and the credential reference.
// Never a credential value: only configuration, which holds none.
QString fingerprint(const QString& provider,const QJsonObject& settings,const QJsonObject& credentials);
// The same for a saved profile object {provider, settings, credentials}.
QString fingerprint(const QJsonObject& profile);
// Whether an upload error means the destination no longer works with these settings and
// credentials: refused sign-in or permission, or a missing bucket or folder. Network
// failures, timeouts, cancellation, size limits and uncertain outcomes are not.
bool rejectsDestination(const Error& error);
}
}
