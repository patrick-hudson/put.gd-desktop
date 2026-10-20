#pragma once
#include "ClipboardPayload.h"
#include <QJsonObject>
#include <QDateTime>
#include <QList>
#include <atomic>
#include <memory>
namespace putgd {
struct Artifact {
 QString id,path,name,mime,category,sha256;
 qint64 bytes=0;
 bool originalFile=false;
 QString previewPath;
 bool namingPrepared=false;
 QJsonObject toJson() const;
 static Artifact fromJson(const QJsonObject&);
};
struct Acquisition { QList<Artifact> artifacts; Error error; };
class ArtifactStore {
public:
 explicit ArtifactStore(QString root);
 // maximumObjectBytes is the destination's per-file limit (zero: none). Both it and
 // free disk space are checked before any byte is copied.
 Acquisition acquire(const QString& operationId,const ClipboardPayload&,const ResourceLimits&,
                     const QString& action="clipboard",const Settings& = Settings::fromValidated(Settings::defaults()),
                     const QDateTime& actionTime = {},qint64 maximumObjectBytes=0);
 // The disk under the artifact root. The reserve is min(5% of the filesystem, 4 GiB)
 // and never under 1 GiB: GNOME warns of low space at 5% free, and 1 GiB keeps logs,
 // the session and updates working on a small disk, so imports and recordings never
 // take /home down to where the desktop fails (#213; 15% refused most of a small
 // VM's free space, and every write here is bounded and refused up front). It
 // applies whatever the byte limits say. `reserved` counts other imports in flight.
 struct DiskBudget {
  qint64 freeBytes=-1,totalBytes=-1,reserveBytes=0,reservedBytes=0;
  qint64 available() const {return freeBytes<0?-1:qMax<qint64>(0,freeBytes-reserveBytes-reservedBytes);}
  QJsonObject toJson() const;
 };
 static qint64 reserveFor(qint64 filesystemBytes);
 // A byte count as the disk messages print it (MiB, GiB or TiB).
 static QString sizeText(qint64 bytes);
 static DiskBudget diskBudget(const QString& path);
 // Atomically checks that `bytes` fit beside the reserve and every other import in
 // flight on the same filesystem, and holds them until the returned token is released.
 // Null when they don't fit; `seen` then reports the budget that refused them.
 static std::shared_ptr<void> reserveDisk(const QString& path,qint64 bytes,DiskBudget* seen=nullptr);
 // Tests may substitute the filesystem's free and total bytes (negative: real).
 static void setDiskForTesting(qint64 freeBytes,qint64 totalBytes);
 Error verify(const Artifact&) const;
 Acquisition recover(const QString& operationId) const;
 void discardIncomplete();
 Error remove(const QString& operationId);
 QString preview(const Artifact&,qint64 budget,qint64 maximumPixels);
 // A video's poster frame (#112) in the same cache, budget and naming as image
 // previews. It decodes a whole process away and takes up to the job deadline,
 // so it runs after the operation ends, never on the upload or result path.
 QString poster(const Artifact&,qint64 budget,qint64 maximumPixels,std::shared_ptr<std::atomic_bool> cancelled={},Error* failure=nullptr);
 // Tests may substitute the poster helper (empty: putgd-record beside the app).
 static void setPosterHelperForTesting(const QString& path);
 void clearPreviews();
 qint64 bytes() const;
private:
 QString cache(const QString& id,const QByteArray& png,qint64 budget);
 QString root_;
};
}
