#include "UploadNaming.h"
#include <QFileInfo>
#include <QHash>
#include <QUuid>
namespace putgd {
QString UploadNaming::candidate(const Artifact& a,const QString& mode,bool rename,int max) {
 auto name=QFileInfo(a.name).fileName(); name.replace(QChar(0),QChar('_')); const auto suffix=QFileInfo(name).suffix();
 QString ext=suffix.isEmpty()?QString{}:"."+suffix;
 while(ext.toUtf8().size()>qMax(1,max/3)) ext.chop(1);
 const auto unique=QUuid::createUuid().toString(QUuid::WithoutBraces);
 QString base=ext.isEmpty()?name:name.left(name.size()-suffix.size()-1);
 if(mode!="original"||!a.originalFile) base=unique;
 else if(rename) base+="-"+unique;
 while((base+ext).toUtf8().size()>max && !base.isEmpty()) base.chop(1);
 return base+ext;
}
QList<NamedItem> UploadNaming::batch(const QList<Artifact>& artifacts,const QString& mode,const QString& policy,int max) {
 QList<NamedItem> r; QHash<QString,QList<int>> groups;
 for(const auto& a:artifacts) {auto name=candidate(a,mode,false,max); const int i=r.size(); r.append({name,"pending"});
  if(groups.contains(name)) {
   if(policy=="overwrite") {r[i].status="conflict"; for(int prior:groups[name]) r[prior].status="conflict";}
   else if(policy=="skip") r[i].status="skipped";
   else {do {name=candidate(a,mode,true,max);} while(groups.contains(name)); r[i].name=name;}
  }
  groups[name].append(i);
 }
 return r;
}
}
