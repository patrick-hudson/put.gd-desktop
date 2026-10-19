#pragma once
#include <QJsonObject>
#include <QUrl>
namespace putgd {
// Maps a compiled build identity (buildManifestJson) to its GitHub release
// without any IO. Only stable, candidate and nightly versions have one: the
// build system gives those versions only to clean builds of the exact tag
// tools/releases cuts. Development, local, dirty and unknown builds have none.
struct ReleaseLink {
 QString version;
 QString kind; // "stable", "candidate", "nightly" or "none"
 QUrl url;
 QJsonObject toJson() const;
};
ReleaseLink releaseLink(const QJsonObject& build);
}
