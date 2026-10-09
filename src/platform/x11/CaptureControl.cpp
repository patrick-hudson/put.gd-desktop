#include "CaptureControl.h"
#include <QJsonDocument>
#include <QJsonParseError>
#include <QStringList>
#include <limits>
namespace putgd {
namespace {bool known(const QString& type){return QStringList{"bootstrap","ready","preview","begin","result","accept","cancel","cancelled","error"}.contains(type);}}
CaptureControl::CaptureControl(QIODevice& device,QObject* parent):QObject(parent),device_(&device){rate_.start();connect(&device,&QIODevice::readyRead,this,&CaptureControl::read);connect(&device,&QIODevice::bytesWritten,this,[this]{write();});}
void CaptureControl::stop(){stopped_=true;input_.clear();output_.clear();}
void CaptureControl::fail(){if(stopped_)return;stop();emit failed({"capture_protocol","The capture helper connection failed",{}});}
bool CaptureControl::send(const QString& type,const QJsonObject& payload){
 if(stopped_||!device_||!known(type)||sent_==std::numeric_limits<quint64>::max())return false;
 const auto bytes=QJsonDocument(QJsonObject{{"version",1},{"sequence",QString::number(sent_+1)},{"type",type},{"payload",payload}}).toJson(QJsonDocument::Compact)+'\n';if(bytes.size()>maximumFrameBytes||output_.size()+device_->bytesToWrite()+bytes.size()>maximumFrameBytes+16384){fail();return false;}++sent_;output_+=bytes;QPointer<CaptureControl> guard(this);write();return guard&&!stopped_;
}
void CaptureControl::write(){if(stopped_||writing_||!device_)return;writing_=true;while(!output_.isEmpty()){const auto n=device_->write(output_.constData(),output_.size());if(n<0){writing_=false;fail();return;}if(!n)break;output_.remove(0,n);}writing_=false;}
void CaptureControl::read(){
 if(stopped_||!device_)return;
 while(device_->bytesAvailable()>0){const auto chunk=device_->read(16384);if(chunk.isEmpty())return;if(rate_.elapsed()>=1000){rate_.restart();rateBytes_=0;rateFrames_=0;}rateBytes_+=chunk.size();if(rateBytes_>4*maximumFrameBytes){fail();return;}input_+=chunk;
  for(;;){const auto pos=input_.indexOf('\n');if(pos<0)break;if(pos+1>maximumFrameBytes||++rateFrames_>64){fail();return;}QJsonParseError parse;const auto doc=QJsonDocument::fromJson(input_.first(pos),&parse);input_.remove(0,pos+1);const auto o=doc.object();bool ok=false;const auto s=o["sequence"].toString();const auto seq=s.toULongLong(&ok);if(parse.error!=QJsonParseError::NoError||!doc.isObject()||o.size()!=4||o["version"]!=1||!known(o["type"].toString())||!o["payload"].isObject()||!ok||seq==0||seq!=received_+1||QString::number(seq)!=s){fail();return;}received_=seq;QPointer<CaptureControl> guard(this);emit frame(o["type"].toString(),o["payload"].toObject());if(!guard||stopped_)return;}
  if(input_.size()>=maximumFrameBytes){fail();return;}
 }
}
}
