#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QString>
#include <QStringList>
#include <functional>
namespace putgd::appimage {
// The in-app AppImage update (#293 phase 3): the file work, without network.
// putgd-cli --install-update fetches and calls these; the resident only reads
// the record and settles the backup at its next start.
//
// The new image is assembled in a private staging folder beside the running
// one (.putgd-update-XXXXXX, so the #307 login script's putgd-*.AppImage scan
// never sees it), verified against the release's SHA256SUMS line, and renamed
// onto the same path. The old image stays as a hidden hard link (or copy),
// .putgd-update-backup-<version>.AppImage, until the new version has started
// once. Nothing else in the folder is touched.

inline constexpr char stagingPrefix[]=".putgd-update-";
inline constexpr char backupPrefix[]=".putgd-update-backup-";
// No AppImage of ours comes near this; a larger download is not ours.
inline constexpr qint64 maximumImageBytes=512LL*1024*1024;

struct Image {
 QString path,directory;
 qint64 size=0;
 QString updateInformation;
};
// The running image at path ($APPIMAGE): a regular file the user owns, with
// the AppImage type 2 magic, in a writable folder with room for another
// requiredBytes. Refusals: update_install_unavailable, update_image_not_writable,
// update_disk_full.
Image inspect(const QString& path,qint64 requiredBytes,Error&);
// ELF and the AppImage type 2 magic ("AI\x02" at offset 8).
bool isAppImage(const QString& path);
// Lowercase hex SHA-256 of the file; empty when it cannot be read.
QString sha256(const QString& path);
// The hash SHA256SUMS lists for name ("<hex>  <name>" or "<hex> *<name>"), or
// empty when it lists none or lists it twice.
QString sumsLine(const QByteArray& sums,const QString& name);
// The candidate is the release's file: the SHA256SUMS hash, an AppImage, and
// the same update information as the running image. update_verification_failed
// says which check failed; nothing has changed on disk.
Error verify(const QString& candidate,const QString& expectedSha256,const QString& updateInformation);
// A hard link to source at destination, or a copy where links are refused.
bool linkOrCopy(const QString& source,const QString& destination,bool* copied=nullptr);

// Installs candidate (already verified) at image.path: same mode as the old
// file, synced, old image kept as the backup, rename() onto the path, folder
// synced. Returns the backup's record {path, device, inode, size, mtime,
// copied}; on failure the old image is still at its path.
QJsonObject swap(const QString& candidate,const Image& image,const QString& previousVersion,Error&);

// <stateRoot>/update-install.json: {schemaVersion, staging, installed, backup}.
QString statePath(const QString& stateRoot);
QJsonObject readState(const QString& stateRoot);
bool writeState(const QString& stateRoot,const QJsonObject& state);
// Resident start: removes a staging folder an interrupted install left, and
// once runningVersion runs from the installed path, the recorded backup if it
// is still the file put.gd made. Returns the state after that.
QJsonObject settle(const QString& stateRoot,const QString& runningImage,const QString& runningVersion);
// Removes a staging folder put.gd made: a directory, not a link, named
// .putgd-update-XXXXXX, holding regular files only.
bool removeStaging(const QString& path);

// appimageupdatetool, else zsync2, on the host's PATH.
struct ZsyncTool {QString kind,path;explicit operator bool() const {return !path.isEmpty();}};
ZsyncTool findZsyncTool(const QProcessEnvironment& host);
// The tool's arguments to build output (in the staging folder) from seed with
// the .zsync at zsyncUrl. appimageupdatetool names its output after the
// .zsync's Filename in seed's folder; zsync2 writes output.
QStringList zsyncArguments(const ZsyncTool&,const QString& seed,const QString& output,const QString& zsyncUrl);
// The host's environment for the tool (#326) with SSL_CERT_FILE set to the
// host's CA bundle the AppImage found (#329); zsync2 otherwise walks past the
// end of its CA path list on Fedora 44 and crashes (#297).
QProcessEnvironment zsyncEnvironment(const QProcessEnvironment& host,const QString& caBundle);
}
