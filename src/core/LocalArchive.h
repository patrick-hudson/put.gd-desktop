#pragma once
#include "ArtifactStore.h"
namespace putgd {
struct LocalCopy { QString status="disabled",path; Error error; QJsonObject toJson() const; static LocalCopy fromJson(const QJsonObject&); };
class LocalArchive {
public:
 static QString folder(const ArchiveSettings&,const QString& category);
 static LocalCopy save(const Artifact&,const ArchiveSettings&);
};
}
