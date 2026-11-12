#pragma once
#include "app/ScrollBackend.h"
#include <QFile>
#include <QHash>
#include <QImage>
#include <QCryptographicHash>
namespace putgd::test {
struct FakeScrollBackend final:ScrollBackend {
 QStringList commands;QJsonObject preparedParams;bool probed=false;
 void refreshCapabilities(std::function<void()> done)override{probed=true;done();}
 Event sink;QString directory;QString corruption;
 // Several commands kept back at once, each answered when the test says so
 // (the real helper answers a cancel only when its worker has ended).
 QStringList holdCommands;QHash<QString,Reply> held;
 // The finished object of the next stop: its endReason when set, and topReached: false with it.
 QString endReason;bool topMissed=false;bool started=false;
 QJsonObject capabilities()const override{return {{"available",probed},{"injector","fake"},{"frames","fake"}};}
 void setEventSink(Event s)override{sink=std::move(s);}
 void prepare(const QString&,const QJsonObject& p,const QString& d,Reply reply)override{preparedParams=p;directory=d;reply({},{{"state",p.contains("rect")?"ready":"selecting"}});}
 void control(const QString&,const QString& cmd,const QJsonObject&,Reply reply)override{
  commands.append(cmd);
  if(holdCommands.contains(cmd)){held[cmd]=std::move(reply);return;}
  if(cmd=="stop"){reply({},finished());return;}
  if(cmd=="start")started=true;
  reply({},{{"state",cmd=="start"?"capturing":"ready"}});
 }
 // What a stop that kept its image answers, and the finished event carries.
 QJsonObject finished(){
  {QImage image(64,64,QImage::Format_RGB32);image.fill(Qt::blue);const auto path=directory+"/result.png";image.save(path);QFile f(path);f.open(QIODevice::ReadOnly);auto png=f.readAll();QJsonObject finished{{"path",path},{"bytes",png.size()},{"sha256",corruption=="hash"?QString("bad"):QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex())},{"width",corruption=="ihdr"?65:64},{"height",64},{"endReason",!endReason.isEmpty()?endReason:QString("stopped")},{"appliedMaxHeight",30000},{"truncated",true}};if(topMissed)finished["topReached"]=false;return finished;}
 }
 void discard(const QString&)override{}
};
}
