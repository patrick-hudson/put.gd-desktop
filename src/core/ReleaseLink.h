#pragma once
#include <QJsonObject>
#include <QUrl>
#include <array>
#include <optional>
namespace putgd {
// A release version: YY.MDD.N or YY.MDD.N-nightly (#373), the UTC committer
// date of the tagged commit (month times 100 plus day, no leading zeros) and
// its commit count. The pre-#373 0.Y.Z, 0.Y.Z-rc.N and 0.Y.Z-nightly.N stay
// readable: installed builds and published releases still carry them.
struct ReleaseVersion {
 QString kind; // "stable", "candidate" (pre-#373 only) or "nightly"
 // Orders every release as dpkg and rpm order its packages: by date and count,
 // a nightly before the stable release of one YY.MDD.N, and every calendar
 // version after every pre-#373 one.
 std::array<qulonglong,5> key{};
};
std::optional<ReleaseVersion> parseReleaseVersion(const QString& version);
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
