#pragma once
#include "app/RecordingBackend.h"
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <functional>

namespace putgd::test {
// A synchronous stand-in for the recording helper supervisor: it never touches
// media, writes a synthetic export file and records every command it saw.
class FakeRecordingBackend final:public RecordingBackend {
public:
 Event sink;QStringList log;QString staging;bool failStart=false,failExport=false,available=true,deferExports=false;QList<std::function<void()>> pendingExports;int exports=0,previews=0,discards=0,cancels=0;qint64 durationUs=12000000;
 QByteArray exportBytes=QByteArray("\0\0\0\x18" "ftypisom",12)+QByteArray(4096,'r');RecordingEdit lastEdit;RecordingConfig lastConfig;qint64 lastPreviewTime=-1;
 QJsonObject capabilities() const override {return {{"available",available},{"reason",available?QString():QString("fake_off")},{"targets",QJsonArray{"monitor","desktop","region","window"}},{"formats",QJsonArray{"mp4","webm","gif","webp"}},{"codecs",QJsonObject{{"mp4",QJsonArray{"h264","h265"}},{"webm",QJsonArray{"vp9"}}}},{"audio",QJsonObject{{"system",true},{"microphone",false}}},{"overlayExclusion",QJsonObject{{"region",true}}}};}
 void setEventSink(Event e) override {sink=std::move(e);}
 // placement: the overlayPlacement the helper reports ("compositor" asks the resident's shell); deferPrepare keeps the reply for the test.
 QString placement;qint64 pid=0;bool deferPrepare=false;Reply pendingPrepare;RecordingTarget lastTarget;
 void prepare(const QString&,const RecordingTarget& target,const RecordingConfig& config,const QString& dir,Reply reply) override {
  log<<"prepare";staging=dir;lastConfig=config;lastTarget=target;
  QJsonObject result{{"canvas",QJsonArray{1280,720}},{"rect",target.rect.isEmpty()?QJsonArray{300,250,1280,720}:QJsonArray{target.rect.x(),target.rect.y(),target.rect.width(),target.rect.height()}},{"overlayExcluded",placement.isEmpty()||placement=="outside"}};
  if(!placement.isEmpty())result["overlayPlacement"]=placement;
  if(deferPrepare){pendingPrepare=[reply,result](Error error,QJsonObject){reply(error,error?QJsonObject{}:result);};return;}
  reply({},result);
 }
 qint64 processId(const QString&) const override {return pid;}
 void control(const QString&,const QString& command,const QJsonObject& params,Reply reply) override {
  log<<command;lastControlParams=params;
  if(command=="cancel"){++cancels;reply({},{});return;}
  if(command=="start"&&failStart){reply({"recording_start_failed","Synthetic source refused",{}},{});return;}
  if(command=="stop"){reply({},{{"durationUs",double(durationUs)},{"canvas",QJsonArray{1280,720}}});return;}
  reply({},{});
 }
 void preview(const QString&,const RecordingEdit& edit,const RecordingConfig&,qint64 time,int,Reply reply) override {++previews;lastEdit=edit;lastPreviewTime=time;reply({},{{"png",QString::fromLatin1(QByteArray("\x89PNG\r\n\x1a\n",8).toBase64())}});}
 void exportClip(const QString&,const RecordingEdit& edit,const RecordingConfig& config,const QString& output,Reply reply) override {
  ++exports;lastEdit=edit;lastConfig=config;log<<"export";
  auto finish=[this,edit,output,reply]{
   if(failExport){reply({"recording_export_failed","Synthetic export failure",{}},{});return;}
   QFile file(output);if(!file.open(QIODevice::WriteOnly)){reply({"recording_io","cannot write",{}},{});return;}file.write(exportBytes);file.close();
   reply({},{{"bytes",double(exportBytes.size())},{"durationUs",double(edit.exportDuration(durationUs))}});
  };
  if(deferExports)pendingExports.append(std::move(finish));else finish();
 }
 void discard(const QString&) override {++discards;}
 int editsPushed=0;RecordingEdit pushedEdit;
 QJsonObject pushedLimits,lastControlParams;RecordingConfig pushedConfig;
 void setEdit(const QString&,const RecordingEdit& edit,const RecordingConfig& config,const QJsonObject& limits) override {++editsPushed;pushedEdit=edit;pushedConfig=config;pushedLimits=limits;}
};
}
