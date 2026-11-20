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
 static QString candidate(const Artifact&,const QString& mode,bool forceRename=false,int maximumBytes=255,const RandomNameSettings& random={});
 // With fresh set to a string style, a taken random name is replaced by a new random string
 // (same extension) instead of gaining a UUID suffix.
 static QString renamedCandidate(const QString&,int maximumBytes=255,const RandomNameSettings* fresh=nullptr);
 static QList<NamedItem> batch(const QList<Artifact>&,const QString& mode,const QString& policy,int maximumBytes=255,const RandomNameSettings& random={});
};
}
