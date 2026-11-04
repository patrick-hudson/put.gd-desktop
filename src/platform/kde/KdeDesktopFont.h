#pragma once
#include <QFile>
#include <QFont>
#include <QStandardPaths>
#include <QStringList>
#include <QtGlobal>
#include <optional>
namespace putgd {
// kdeglobals in KConfig's order, most specific first.
inline QStringList kdeGlobalsPaths() {
 QStringList paths;
 for(const auto& directory:QStandardPaths::standardLocations(QStandardPaths::GenericConfigLocation))paths.append(directory+QStringLiteral("/kdeglobals"));
 return paths;
}
// The general font Plasma's platform theme would apply, without loading it: the
// first [General] font= entry, else Plasma's own default. A font[$i] entry is
// immutable, so it wins from any file, and the least specific one wins among them.
inline QFont kdeDesktopFont(const QStringList& paths) {
 std::optional<QFont> first,immutable;
 for(const auto& path:paths) {
  QFile file(path);
  if(!file.open(QIODevice::ReadOnly|QIODevice::Text))continue;
  bool general=false;
  while(!file.atEnd()) {
   const auto line=file.readLine(4096).trimmed();
   if(line.startsWith('[')){general=line=="[General]";continue;}
   if(!general||!line.startsWith("font"))continue;
   const auto separator=line.indexOf('=');
   if(separator<0)continue;
   const auto key=line.left(separator).trimmed();
   // KConfig marks an entry's flags as font[$i]=, font[$ie]= and so on.
   if(key!="font"&&!key.startsWith("font["))continue;
   QFont font;
   if(!font.fromString(QString::fromUtf8(line.mid(separator+1).trimmed())))continue;
   if(key.startsWith("font[$")&&key.contains('i'))immutable=font;
   else if(!first)first=font;
  }
 }
 if(immutable)return *immutable;
 return first?*first:QFont(QStringLiteral("Noto Sans"),10);
}
}
