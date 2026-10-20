#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QUrl>
#include <optional>
namespace putgd {
// Update checks (#293). The resident runs `putgd-cli --check-update CHANNEL` at
// most once a day; that child fetches the static feed and compares versions
// here, so the resident itself never loads a TLS backend. Nothing in this file
// performs IO.
namespace update {
// Which published package this build is: ubuntu24.04, ubuntu26.04, debian13,
// fedora44, appimage or flatpak; empty for a build no package job made.
QString packageTarget();
// False when the build was configured with PUTGD_UPDATE_CHECK=OFF.
bool checkBuilt();

struct Build {
 QString version;
 QString kind;    // stable, candidate, nightly or none (see ReleaseLink)
 QString channel; // the feed the build follows: stable (also candidates), nightly, or empty
 QString target;
 bool supported=false;
 QString reason;  // why checks are off: disabled_at_build, flatpak, no_package, development_build
 QJsonObject toJson() const;
};
// target and built default to this build's compiled values.
Build build(const QJsonObject& manifest);
Build build(const QJsonObject& manifest,const QString& target,bool built);

// Release versions: X.Y.Z, X.Y.Z-rc.N or X.Y.Z-nightly.N. For one base,
// nightly < rc < stable. Returns nullopt when either is not a release version.
std::optional<int> compareVersions(const QString& left,const QString& right);

// https://app.put.gd/update/v1/<channel>.json. PUTGD_UPDATE_FEED_URL replaces
// the https://app.put.gd/update/v1/ prefix, for tests and the lab: an https URL,
// or http only on a loopback address.
QUrl feedUrl(const QString& channel,Error&);

// Validates feed contract v1 and compares it with the build. The result is
// {channel, current, latest, tag, publishedAt, notesUrl, downloadUrl,
// updateAvailable}; latest and the URLs are null when the channel has no
// release. Links the feed names must stay on the project's GitHub releases or
// app.put.gd, since phase 1 feeds are not signed.
QJsonObject evaluate(const QByteArray& feed,const Build&,const QString& channel,Error&);
}
}
