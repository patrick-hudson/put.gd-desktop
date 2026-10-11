#pragma once
#include "ArtifactStore.h"
#include <QDateTime>
namespace putgd {
struct NamedItem { QString name,status="pending"; };
struct FilenameResult { QString name; Error error; };
class UploadNaming {
public:
 static Error validateTemplate(const QString&);
 static FilenameResult renderTemplate(const QString&,const Artifact&,const QDateTime&,const QString& unique={});
 static FilenameResult acquiredName(const Artifact&,const Settings&,const QString& action,const QDateTime&);
 static QString candidate(const Artifact&,const QString& mode,bool forceRename=false,int maximumBytes=255);
 static QString renamedCandidate(const QString&,int maximumBytes=255);
 static QList<NamedItem> batch(const QList<Artifact>&,const QString& mode,const QString& policy,int maximumBytes=255);
};
}
