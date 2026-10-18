#include "app/UiControl.h"
#include "ui/StdioDevice.h"
#include <QCoreApplication>
#include <QTimer>
#include <cstdio>
#include <fcntl.h>
#include <unistd.h>
using namespace putgd;
int main(int argc,char**argv){
 QCoreApplication app(argc,argv);StdioDevice pipes;if(!pipes.isOpen())return 2;UiControl control(pipes);
 const auto mode=qEnvironmentVariable("PUTGD_TEST_UI_MODE");
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,&QCoreApplication::quit);
 QObject::connect(&control,&UiControl::failed,&app,[&]{app.exit(3);});
 QObject::connect(&control,&UiControl::frame,&app,[&](QString type,QString seq,QJsonObject payload){
  if(type=="bootstrap"){
   if(mode=="mismatch"){control.send("ready",{{"build",QJsonObject{}},{"residentEpoch",payload["residentEpoch"]}});return;}
   if(mode=="expect-state"&&payload["state"].toObject()["settings"].toObject()["draft"]!="retained"){control.send("incompatible");return;}
   control.send("ready",{{"build",payload["build"]},{"residentEpoch",payload["residentEpoch"]}});
  }else if(type=="activate"){
   if(mode=="crash"){app.exit(77);return;}
   if(mode=="stall")return;
   if(mode=="flood"){fcntl(STDOUT_FILENO,F_SETFL,fcntl(STDOUT_FILENO,F_GETFL)&~O_NONBLOCK);const QByteArray block(16384,'x');for(int i=0;i<8192;++i)std::fwrite(block.constData(),1,block.size(),stdout);std::fflush(stdout);return;}
   control.send("shown",{{"replyTo",seq},{"view",payload["view"]}});
   if(mode=="close"||mode=="crash-after-close"||(mode=="cancel"&&payload["view"]=="settings"))control.send("closeRequested",{{"state",QJsonObject{{"schemaVersion",1},{"settings",QJsonObject{{"draft","retained"},{"section",1},{"captureRouting",QJsonObject{{"default",QJsonObject{{"profileId","synthetic"}}}}}}}}}});
   if(mode.startsWith("recording-")){
    control.send("shortcutRecording",{{"active",true}});
    if(mode=="recording-crash-pending")QTimer::singleShot(100,&app,[&]{app.exit(77);});
   }
   if(mode=="bad-state")control.send("closeRequested",{{"state",QJsonObject{{"unexpected","discard nothing"}}}});
  }else if(type=="shortcutRecordingChanged"){
   if(mode=="recording-rejected"&&payload["error"].toObject()["code"]=="synthetic_failure")app.exit(0);
   if(payload["active"].toBool()){
    if(mode=="recording-crash")app.exit(77);
    else if(mode=="recording-cycle")control.send("shortcutRecording",{{"active",false}});
   }
  }else if(type=="closeAccepted"){
   if(mode=="cancel"){QTimer::singleShot(250,&app,[&,payload]{control.send("closeCancel",{{"replyTo",payload["replyTo"]}});});return;}
   control.send("closeConfirmed",{{"replyTo",payload["replyTo"]}});QTimer::singleShot(100,&app,[&]{app.exit(mode=="crash-after-close"?77:0);});}
  else if(type=="invalidated"){if(mode=="cancel")QTimer::singleShot(50,&app,[&,seq]{control.send("observed",{{"replyTo",seq}});});else control.send("observed",{{"replyTo",seq}});}
  else if(type=="shutdown")app.quit();
 });
 return app.exec();
}
