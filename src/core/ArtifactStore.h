#pragma once
#include "ClipboardPayload.h"
#include <QJsonObject>
#include <QDateTime>
#include <QList>
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
 // The disk under the artifact root. The reserve follows journald's SystemKeepFree
 // rule, min(15% of the filesystem, 4 GiB) and never under 1 GiB, so imports and
 // recordings never take /home down to where the desktop and logging fail. It
 // applies whatever the byte limits say. `reserved` counts other imports in flight.
 struct DiskBudget {
  qint64 freeBytes=-1,totalBytes=-1,reserveBytes=0,reservedBytes=0;
  qint64 available() const {return freeBytes<0?-1:qMax<qint64>(0,freeBytes-reserveBytes-reservedBytes);}
  QJsonObject toJson() const;
 };
 static qint64 reserveFor(qint64 filesystemBytes);
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
 void clearPreviews();
 qint64 bytes() const;
private: QString root_;
};
}
