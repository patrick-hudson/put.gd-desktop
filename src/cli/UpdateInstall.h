#pragma once
#include "core/Update.h"
#include <QJsonObject>
#include <functional>
namespace putgd {
// putgd-cli --install-update CHANNEL [--method auto|zsync|download] [--state DIR]
// installs the newest AppImage on CHANNEL over the running one (#293 phase 3).
// The resident starts it only after a click in Settings or update.install with
// confirm. It prints {"event":"progress",...} lines, then one result line in
// the --check-update format.
//
// zsync path: appimageupdatetool (or zsync2) on the host, run with the host's
// environment and CA bundle on a hard link of the image in a private staging
// folder, pinned to the release's mirrored .zsync. Fallback: the whole image
// from the GitHub release (then the mirror) through put.gd's own TLS. Either
// way the file must match the release's SHA256SUMS line before it is renamed
// onto the image's path; the old image is kept as a hidden backup. That
// SHA256SUMS must carry the release signer's minisign signature
// (pkg.put.gd/releases/<tag>/SHA256SUMS.minisig) by the channel's key, with the
// trusted comment "put.gd <tag> <channel>".
struct InstallOptions {
 QString channel,method="auto",stateRoot,image,caBundle;
 // The minisign key SHA256SUMS must be signed with; empty: the release key
 // compiled in for the build's channel (tests pass their own).
 QByteArray publicKey;
};
bool isInstallMode(int argc,char** argv);
int runInstallMode(int argc,char** argv);
// The install itself; needs a running QCoreApplication. Exposed for tests.
QJsonObject installUpdate(const update::Build&,const InstallOptions&,const std::function<void(const QJsonObject&)>& progress,Error&);
}
