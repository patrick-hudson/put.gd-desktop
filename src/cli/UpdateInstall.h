#pragma once
#include "app/AppImageInstall.h"
#include "core/Update.h"
#include <QHash>
#include <QJsonObject>
#include <functional>
namespace putgd {
// putgd-cli --install-update CHANNEL [--method auto|zsync|download] [--state DIR]
// installs the newest AppImage on CHANNEL over the running one (#293 phase 3).
// The resident starts it only after a click in Settings or update.install with
// confirm. It prints {"event":"progress",...} lines, then one result line in
// the --check-update format.
//
// zsync path (#1766): put.gd's own client (Zsync.h) reads the release's .zsync
// on the mirror, copies the blocks the running image already has into a
// private staging folder and fetches the rest from the mirror's image with
// Range requests; the result must match the .zsync's SHA-1. Fallback, after
// any zsync failure: the whole image from the mirror (then the GitHub
// release). Both go through put.gd's own TLS. Either
// way the file must match the release's SHA256SUMS line before it is renamed
// onto the image's path; the old image is kept as a hidden backup. That
// SHA256SUMS must carry the release signer's minisign signature
// (pkg.put.gd/releases/<tag>/SHA256SUMS.minisig) by the channel's key, with the
// trusted comment "put.gd <tag> <channel>".
//
// --switch (#344): CHANNEL is not the build's own. The release is then checked
// with CHANNEL's key and trusted comment, and the new image must embed CHANNEL's
// permalink instead of the running image's; an older version also needs
// --allow-older. Without --switch another channel's image is refused.
struct InstallOptions {
 QString channel,method="auto",stateRoot,image;
 bool switchChannel=false,allowOlder=false;
 // The minisign key SHA256SUMS must be signed with, by channel; a channel
 // without one uses the release key compiled in (tests pass their own).
 QHash<QString,QByteArray> publicKeys;
 // How long the zsync update may take, from the .zsync's request through the
 // seed scan and the last range, before it falls back. The resident kills the
 // whole child after an hour (UpdateService::installTimeoutMs); 15 minutes
 // leaves 45 for the whole file, which a 64 KB/s link still manages. Tests
 // pass a shorter one.
 int zsyncTimeoutMs=15*60*1000;
 // Who owns the image and whether its folder can be written; empty asks the
 // system (tests pass their own).
 appimage::Access access;
};
bool isInstallMode(int argc,char** argv);
int runInstallMode(int argc,char** argv);
// The install itself; needs a running QCoreApplication. Exposed for tests.
QJsonObject installUpdate(const update::Build&,const InstallOptions&,const std::function<void(const QJsonObject&)>& progress,Error&);
}
