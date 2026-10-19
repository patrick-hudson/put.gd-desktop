#pragma once
#include <QJsonObject>
#include <QUrl>
namespace putgd {
// Maps a compiled build identity (buildManifestJson) to its public GitHub page
// without any IO: stable, candidate and nightly versions name the release tag
// tools/releases cuts; a clean development build names its source commit;
// dirty, unknown or malformed identities have no page.
struct ReleaseLink {
 QString version;
 QString kind; // "stable", "candidate", "nightly", "commit" or "none"
 QUrl url;
 QJsonObject toJson() const;
};
ReleaseLink releaseLink(const QJsonObject& build);
}
