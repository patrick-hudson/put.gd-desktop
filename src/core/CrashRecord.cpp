#include "CrashRecord.h"
#include <QDateTime>
#include <QJsonArray>
#include <QList>
#include <QRegularExpression>
namespace putgd::crash {
namespace {
constexpr char prefix[]="putgd-crash\t";
constexpr int maximumFrames=128,maximumModules=64;
QString address(const QByteArray& value) {
 static const QRegularExpression hex("^0x[0-9a-f]{1,16}$");
 const auto text=QString::fromLatin1(value);return hex.match(text).hasMatch()?text:QString();
}
}
bool isRecordLine(const QByteArray& line) {return line.startsWith(prefix);}
QJsonObject parseRecord(const QByteArray& text) {
 QJsonObject record;QJsonArray frames,modules;bool begun=false,complete=false;
 for(auto line:text.split('\n')) {
  if(line.endsWith('\r'))line.chop(1);
  if(!isRecordLine(line))continue;
  const auto parts=line.mid(int(sizeof prefix)-1).split('\t');const auto& kind=parts[0];
  if(kind=="begin"&&parts.size()>=5) {
   if(begun)break; // one record per text
   begun=true;record={{"recordVersion",parts[1].toInt()},{"process",QString::fromUtf8(parts[2]).left(64)},{"pid",parts[3].toLongLong()},{"thread",parts[4].toLongLong()}};
  } else if(!begun) continue;
  else if(kind=="version"&&parts.size()>=2)record["version"]=QString::fromUtf8(parts[1]).left(128);
  else if(kind=="signal"&&parts.size()>=5) {record["signal"]=parts[1].toInt();record["signalName"]=QString::fromLatin1(parts[2]).left(16);record["code"]=parts[3].toInt();record["faultAddress"]=address(parts[4]);}
  else if(kind=="time"&&parts.size()>=2)record["time"]=QDateTime::fromSecsSinceEpoch(parts[1].toLongLong()).toUTC().toString(Qt::ISODate);
  else if(kind=="pc"&&parts.size()>=2)record["pc"]=address(parts[1]);
  else if(kind=="frame"&&parts.size()>=5&&frames.size()<maximumFrames) {
   QJsonObject frame{{"index",parts[1].toInt()},{"address",address(parts[2])}};
   bool numeric=false;const int module=parts[3].toInt(&numeric);
   if(numeric){frame["module"]=module;frame["offset"]=address(parts[4]);}
   frames.append(frame);
  } else if(kind=="module"&&parts.size()>=5&&modules.size()<maximumModules) {
   static const QRegularExpression buildId("^[0-9a-f]{2,64}$");
   const auto id=QString::fromLatin1(parts[3]);
   modules.append(QJsonObject{{"index",parts[1].toInt()},{"loadBias",address(parts[2])},{"buildId",buildId.match(id).hasMatch()?id:QString()},
    {"file",QString::fromUtf8(parts.mid(4).join('\t')).left(512)}});
  } else if(kind=="end"){complete=true;break;}
 }
 if(!begun)return {};
 record["frames"]=frames;record["modules"]=modules;record["complete"]=complete;
 return record;
}
}
