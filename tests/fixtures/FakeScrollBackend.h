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
 // The finished object of the next stop: its endReason when set, topReached: false with it,
 // and fields that replace the usual ones (how it was made, #1515).
 QString endReason;bool topMissed=false;bool started=false;QJsonObject overrides;
 QJsonObject capabilities()const override{return {{"available",probed},{"injector","fake"},{"frames","fake"}};}
 void setEventSink(Event s)override{sink=std::move(s);}
 void prepare(const QString&,const QJsonObject& p,const QString& d,Reply reply)override{preparedParams=p;directory=d;reply({},{{"state",p.contains("rect")?"ready":"selecting"}});}
 void control(const QString&,const QString& cmd,const QJsonObject& p,Reply reply)override{
  commands.append(cmd);
  if(holdCommands.contains(cmd)){held[cmd]=std::move(reply);return;}
  if(cmd=="stop"){reply({},finished());return;}
  if(cmd=="start")started=true;
  if(cmd=="autoScroll"||cmd=="pause"||cmd=="resume"){const bool on=cmd=="autoScroll"?p["on"].toBool():cmd=="resume";reply({},{{"state","capturing"},{"driver",on?"putgd":"user"},{"paused",false}});return;}
  reply({},{{"state",cmd=="start"?"capturing":"ready"}});
 }
 // What a stop that kept its image answers, and the finished event carries.
 QJsonObject finished(){
  {QImage image(64,64,QImage::Format_RGB32);image.fill(Qt::blue);const auto path=directory+"/result.png";image.save(path);QFile f(path);f.open(QIODevice::ReadOnly);auto png=f.readAll();QJsonObject finished{{"path",path},{"bytes",png.size()},{"sha256",corruption=="hash"?QString("bad"):QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex())},{"width",corruption=="ihdr"?65:64},{"height",64},{"endReason",!endReason.isEmpty()?endReason:QString("done")},{"appliedMaxHeight",30000},{"scrolledBy","user"},{"framesPlaced",12},{"framesSkipped",0},{"endedLost",false},{"reachedEnd",false}};
   const auto reason=finished["endReason"].toString();finished["truncated"]=reason=="height_cap"||reason=="lost_track"||reason=="top_not_reached";if(reason=="lost_track")finished["endedLost"]=true;if(reason=="end_of_content"||reason=="top_not_reached")finished["reachedEnd"]=true;
   if(topMissed)finished["topReached"]=false;for(auto i=overrides.begin();i!=overrides.end();++i)finished[i.key()]=i.value();return finished;}
 }
 void discard(const QString&)override{}
};
}
