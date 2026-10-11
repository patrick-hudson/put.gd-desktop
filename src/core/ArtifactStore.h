#pragma once
#include "ClipboardPayload.h"
#include <QJsonObject>
#include <QDateTime>
#include <QList>
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
 Acquisition acquire(const QString& operationId,const ClipboardPayload&,const ResourceLimits&,
                     const QString& action="clipboard",const Settings& = Settings::fromValidated(Settings::defaults()),
                     const QDateTime& actionTime = {});
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
