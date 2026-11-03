#pragma once
#include <QFile>
#include <QFont>
#include <QStandardPaths>
#include <QStringList>
#include <QtGlobal>
namespace putgd {
// kdeglobals in KConfig's order, most specific first.
inline QStringList kdeGlobalsPaths() {
 QStringList paths;
 for(const auto& directory:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))paths.append(directory+QStringLiteral("/kdeglobals"));
 return paths;
}
// The general font Plasma's platform theme would apply, without loading it:
// the first [General] font= entry, else Plasma's own default.
inline QFont kdeDesktopFont(const QStringList& paths) {
 for(const auto& path:paths) {
  QFile file(path);
  if(!file.open(QIODevice::ReadOnly|QIODevice::Text))continue;
  bool general=false;
  while(!file.atEnd()) {
   const auto line=file.readLine(4096).trimmed();
   if(line.startsWith('[')){general=line=="[General]";continue;}
   if(!general||!line.startsWith("font"))continue;
   const auto separator=line.indexOf('=');
   // KConfig marks an immutable or expanded entry as font[$i]=.
   if(separator<0||(line.left(separator).trimmed()!="font"&&!line.startsWith("font[")))continue;
   QFont font;
   if(font.fromString(QString::fromUtf8(line.mid(separator+1).trimmed())))return font;
  }
 }
 return QFont(QStringLiteral("Noto Sans"),10);
}
}
