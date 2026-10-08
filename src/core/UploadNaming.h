#pragma once
#include "ArtifactStore.h"
namespace putgd {
struct NamedItem { QString name,status="pending"; };
class UploadNaming {
public:
 static QString candidate(const Artifact&,const QString& mode,bool forceRename=false,int maximumBytes=255);
 static QList<NamedItem> batch(const QList<Artifact>&,const QString& mode,const QString& policy,int maximumBytes=255);
};
}
