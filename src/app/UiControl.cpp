#include "UiControl.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <limits>
namespace putgd {
namespace {
bool knownType(const QString& type){
 static const QStringList types{"bootstrap","ready","incompatible","activate","shown","activationFailed","invalidated","observed","checkpoint","checkpointAccepted","checkpointRejected","closeRequested","closeAccepted","closeRejected","closeCancel","closeCancelled","closeConfirmed","shutdown","focusChanged","shortcutRecording","shortcutRecordingChanged",
  // Image editor helper: progress, agent commands and their results.
  "editing","finalizing","command","commandResult"};
 return types.contains(type);
}
}
UiControl::UiControl(QIODevice& device,QObject* parent):QObject(parent),device_(&device){
 rate_.start();
 connect(&device,&QIODevice::readyRead,this,&UiControl::read);
 connect(&device,&QIODevice::bytesWritten,this,[this]{write();});
}
QString UiControl::send(const QString& type,const QJsonObject& payload){
 if(stopped_||!device_)return {};
 if(!knownType(type)||sent_==std::numeric_limits<quint64>::max()){fail("ui_protocol");return {};}
 const auto sequence=QString::number(sent_+1);
 auto bytes=QJsonDocument(QJsonObject{{"version",1},{"type",type},{"sequence",sequence},{"payload",payload}}).toJson(QJsonDocument::Compact)+'\n';
 if(bytes.size()>maximumFrameBytes||output_.size()+device_->bytesToWrite()+bytes.size()>maximumFrameBytes+65536){fail("ui_control_limit");return {};}
 ++sent_;output_+=bytes;
 QPointer<UiControl> guard(this);write();
 return guard&&!guard->stopped_?sequence:QString{};
}
void UiControl::stop(){stopped_=true;input_.clear();output_.clear();}
void UiControl::fail(const QString& code){
 if(stopped_)return;
 stop();emit failed({code,"The interface connection failed. Reopen the window from the tray.",{}});
}
void UiControl::write(){
 if(stopped_||writing_||!device_)return;
 writing_=true;
 while(!output_.isEmpty()){
  const auto written=device_->write(output_.constData(),output_.size());
  if(written<0){writing_=false;fail("ui_control_write");return;}
  if(!written)break;
  output_.remove(0,written);
 }
 writing_=false;
}
void UiControl::read(){
 if(stopped_||!device_)return;
 while(device_->bytesAvailable()>0){
  const auto chunk=device_->read(16384);
  if(chunk.isEmpty())return;
  if(rate_.elapsed()>=1000){rate_.restart();receivedBytes_=0;receivedFrames_=0;}
  receivedBytes_+=chunk.size();
  if(receivedBytes_>4*maximumFrameBytes){fail("ui_control_flood");return;}
  input_+=chunk;
  for(;;){
   const auto newline=input_.indexOf('\n');
   if(newline<0)break;
   if(newline+1>maximumFrameBytes||++receivedFrames_>256){fail("ui_control_limit");return;}
   QJsonParseError parse;const auto document=QJsonDocument::fromJson(input_.first(newline),&parse);
   input_.remove(0,newline+1);
   const auto object=document.object();
   bool numeric=false;const auto sequence=object["sequence"].toString();const auto number=sequence.toULongLong(&numeric);
   if(parse.error!=QJsonParseError::NoError||!document.isObject()||object.size()!=4||object["version"]!=1||!object["type"].isString()||!knownType(object["type"].toString())||!object["payload"].isObject()||!numeric||number!=received_+1||QString::number(number)!=sequence||number==0){fail("ui_protocol");return;}
   received_=number;
   QPointer<UiControl> guard(this);emit frame(object["type"].toString(),sequence,object["payload"].toObject());
   if(!guard||guard->stopped_)return;
  }
  if(input_.size()>=maximumFrameBytes){fail("ui_control_limit");return;}
 }
}
}
