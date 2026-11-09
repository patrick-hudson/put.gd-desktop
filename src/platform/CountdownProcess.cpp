#include "CountdownProcess.h"
#include "core/BuildInfo.h"
#include <QCoreApplication>
#include <QFileInfo>
#include <utility>
namespace putgd {
namespace {
Error unavailable(const QString& check,QJsonObject details={}){details["check"]=check;return {"delay_unavailable","The capture countdown could not be shown; restart put.gd if this continues",details};}
}
CountdownProcess::CountdownProcess(QString helper,QObject* parent):QObject(parent),helper_(helper.contains('/')?std::move(helper):QCoreApplication::applicationDirPath()+'/'+helper){
 deadline_.setSingleShot(true);
 connect(&deadline_,&QTimer::timeout,this,[this]{if(!process_)return;process_->kill();finish(unavailable("deadline"));});
}
CountdownProcess::~CountdownProcess(){
 reply_={};
 if(process_){process_->disconnect(this);process_->kill();process_->waitForFinished(1000);}
}
void CountdownProcess::start(int seconds,const QJsonObject& appearance,std::function<void(Error)> reply){
 if(process_){reply({"busy_selecting","Another capture countdown is running",{}});return;}
 if(seconds<1||seconds>60){reply(unavailable("seconds"));return;}
 if(!QFileInfo(helper_).isExecutable()){reply(unavailable("helper_missing"));return;}
 reply_=std::move(reply);cancelling_=false;
 auto* child=new QProcess(this);process_=child;
 child->setProcessChannelMode(QProcess::ForwardedErrorChannel);child->setStandardInputFile(QProcess::nullDevice());child->setStandardOutputFile(QProcess::nullDevice());
 connect(child,&QProcess::errorOccurred,this,[this,child](QProcess::ProcessError error){if(process_==child&&error==QProcess::FailedToStart)finish(unavailable("start"));});
 connect(child,qOverload<int,QProcess::ExitStatus>(&QProcess::finished),this,[this,child](int code,QProcess::ExitStatus status){
  if(process_!=child)return;
  if(cancelling_||(status==QProcess::NormalExit&&code==cancelledExit))finish({"cancelled","Capture countdown cancelled",{}});
  else if(status==QProcess::NormalExit&&code==0)finish({});
  else if(status==QProcess::NormalExit&&code==buildExit)finish({"capture_unavailable","put.gd was updated while it was running; quit and start it again",{{"check","countdown_build"}}});
  else finish(unavailable("exit",{{"exitCode",code},{"crashed",status!=QProcess::NormalExit}}));
 });
 // The helper may take a moment to map its pill; the delay itself is its own.
 deadline_.start((seconds+15)*1000);
 child->start(helper_,{QString::fromLatin1(argument),QString::number(seconds),appearance["scheme"].toString()=="light"?QStringLiteral("light"):QStringLiteral("dark"),QString::fromLatin1(buildVersion())});
}
void CountdownProcess::cancel(){
 if(!process_||cancelling_)return;
 cancelling_=true;process_->terminate();
 // A helper that ignores SIGTERM is killed; either way the pill's process is gone.
 QTimer::singleShot(1000,process_,[process=process_]{if(process->state()!=QProcess::NotRunning)process->kill();});
}
void CountdownProcess::finish(Error error){
 deadline_.stop();
 if(auto* child=std::exchange(process_,nullptr)){child->disconnect(this);if(child->state()!=QProcess::NotRunning)child->kill();child->deleteLater();}
 if(auto reply=std::exchange(reply_,{}))reply(std::move(error));
}
}
