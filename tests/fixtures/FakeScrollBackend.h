#pragma once
#include "app/ScrollBackend.h"
#include <QFile>
#include <QImage>
#include <QCryptographicHash>
namespace putgd::test {
struct FakeScrollBackend final:ScrollBackend {
 std::function<void(const QString&)> onPrepare;
 QStringList commands;QJsonObject preparedParams;int probes=0;bool probed=false;
 void refreshCapabilities(std::function<void()> done)override{++probes;probed=true;done();}
 Event sink;QString directory;QString corruption;bool stopEmpty=false;bool heightCap=false;
 // The finished object of the next stop: its endReason when set, and topReached: false with it.
 QString endReason;bool topMissed=false;int discarded=0;bool started=false;
 QJsonObject capabilities()const override{return {{"available",probed},{"injector","fake"},{"frames","fake"}};}
 void setEventSink(Event s)override{sink=std::move(s);}
 void prepare(const QString& id,const QJsonObject& p,const QString& d,Reply reply)override{preparedParams=p;if(onPrepare)onPrepare(id);directory=d;reply({},{{"state",p.contains("rect")?"ready":"selecting"}});}
 void control(const QString&,const QString& cmd,const QJsonObject&,Reply reply)override{
  commands.append(cmd);
  if(cmd=="stop"&&stopEmpty){reply({"cancelled","No frame yet",{}},{});return;}
  if(cmd=="stop") {QImage image(64,64,QImage::Format_RGB32);image.fill(Qt::blue);const auto path=directory+"/result.png";image.save(path);QFile f(path);f.open(QIODevice::ReadOnly);auto png=f.readAll();QJsonObject finished{{"path",path},{"bytes",png.size()+(corruption=="size"?1:0)},{"sha256",corruption=="hash"?QString("bad"):QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex())},{"width",corruption=="ihdr"?65:64},{"height",64},{"endReason",!endReason.isEmpty()?endReason:QString(heightCap?"height_cap":"stopped")},{"appliedMaxHeight",30000},{"truncated",true}};if(topMissed)finished["topReached"]=false;reply({},finished);return;}
  if(cmd=="start")started=true;
  reply({},{{"state",cmd=="start"?"capturing":"ready"}});
 }
 void discard(const QString&)override{++discarded;}
};
}
