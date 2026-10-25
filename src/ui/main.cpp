#include "UiApplicationController.h"
#include "ImageJob.h"
#include "ImageEditor.h"
#include "StdioDevice.h"
#include "ViewState.h"
#include "VisualStyle.h"
#include "core/AllocatorPolicy.h"
#include "core/BuildInfo.h"
#include "core/HostEnvironment.h"
#include <QApplication>
#include <QJsonDocument>
#include <QTimer>
#include <cstdio>
#include <cstring>
#include <memory>
int main(int argc,char** argv){
 if(argc==2&&std::strcmp(argv[1],"--version-json")==0){
  std::printf("%s\n",putgd::buildManifestJson());return 0;
 }
 if(argc==2&&(std::strcmp(argv[1],"--version")==0||std::strcmp(argv[1],"-v")==0)){
  std::printf("putgd-ui %s\n",putgd::buildVersion());return 0;
 }
 putgd::UiControlPipes handles;if(!handles.valid())return 2;
 // The editor draws its own theme. Qt's built-in generic platform theme avoids
 // loading the desktop's GTK/KDE integration stack into this short-lived image
 // process (about 22 MiB RSS on GNOME 50; about 85 MiB on Plasma, whose theme
 // pulls in Qt Quick and the GPU driver); Save a copy uses Qt's file dialog.
 // Qt tries a theme plugin for every candidate name before falling back to the
 // built-in generic theme, and derives candidates from the desktop identity, so
 // that identity is cleared as well.
 if(argc==2&&std::strcmp(argv[1],"--edit-image")==0){
  // A host program it starts gets the desktop's own values (#326).
  for(const char* name:{"QT_QPA_PLATFORMTHEME","XDG_CURRENT_DESKTOP","XDG_SESSION_DESKTOP","DESKTOP_SESSION","KDE_FULL_SESSION","GNOME_DESKTOP_SESSION_ID"})putgd::host::keep(name);
  qputenv("QT_QPA_PLATFORMTHEME","generic");
  for(const char* name:{"XDG_CURRENT_DESKTOP","XDG_SESSION_DESKTOP","DESKTOP_SESSION","KDE_FULL_SESSION","GNOME_DESKTOP_SESSION_ID"})qunsetenv(name);
  // Previews and exports allocate full-size rasters and PNG buffers. Without a
  // fixed mmap threshold the freed ones stay in the heap: on Fedora 44 one
  // 1920x1080 preview kept 36 MiB resident until the editor closed.
  if(!putgd::applyResidentAllocatorPolicy())std::fputs("Could not apply the editor allocation policy\n",stderr);
 }
 QApplication app(argc,argv);
 QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd-ui");
 QCoreApplication::setApplicationVersion(putgd::buildVersion());
 app.setDesktopFileName("putgd-ui");app.setQuitOnLastWindowClosed(false);
 if(argc==2&&std::strcmp(argv[1],"--image-job")==0){
  putgd::StdioDevice pipes(handles.input(),handles.output());if(!pipes.isOpen())return 2;
  return putgd::runImageJob(app,pipes);
 }
 putgd::VisualStyle::apply(app);app.setWindowIcon(putgd::VisualStyle::icon("brand"));
 if(argc==2&&std::strcmp(argv[1],"--edit-image")==0){
  // A separate on-demand process: closing it frees the image regardless of other views.
  putgd::StdioDevice pipes(handles.input(),handles.output());if(!pipes.isOpen())return 2;
  return putgd::runImageEditor(app,pipes);
 }
 putgd::StdioDevice pipes(handles.input(),handles.output());if(!pipes.isOpen())return 2;
 putgd::UiControl control(pipes);
 std::unique_ptr<putgd::SessionClient> client;std::unique_ptr<putgd::UiSession> session;
 std::unique_ptr<putgd::UiApplicationController> windows;
 bool bootstrapped=false;QString epoch;
 const auto build=QJsonDocument::fromJson(putgd::buildManifestJson()).object();
 QTimer deadline;deadline.setSingleShot(true);deadline.start(10000);
 QObject::connect(&deadline,&QTimer::timeout,&app,[&]{app.exit(3);});
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,&QCoreApplication::quit);
 QObject::connect(&control,&putgd::UiControl::failed,&app,[&]{app.exit(3);});
 QObject::connect(&control,&putgd::UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& payload){
  if(type!="bootstrap")return;
  if(bootstrapped||payload["build"].toObject()!=build||payload["residentEpoch"].toString().isEmpty()||!payload["socketPath"].isString()){
   control.send("incompatible");app.exit(4);return;
  }
  bootstrapped=true;epoch=payload["residentEpoch"].toString();
  auto state=payload["state"].toObject();if(state.isEmpty())state={{"schemaVersion",1}};
  if(putgd::validateUiState(state)){control.send("incompatible");app.exit(4);return;}
  client=std::make_unique<putgd::SessionClient>(payload["socketPath"].toString());
  session=std::make_unique<putgd::UiSession>(*client);
  session->refreshSnapshot([&,state](putgd::Error error){
   if(error||session->build()!=build||session->residentEpoch()!=epoch){control.send("incompatible");app.exit(4);return;}
   windows=std::make_unique<putgd::UiApplicationController>(*session,control,state);
   QObject::connect(windows.get(),&putgd::UiApplicationController::exitRequested,&app,&QCoreApplication::quit);
   QObject::connect(session.get(),&putgd::UiSession::statusChanged,&app,[&]{
    if(session->build()!=build||session->residentEpoch()!=epoch){control.send("incompatible");app.exit(4);}
   });
   deadline.stop();control.send("ready",{{"build",build},{"residentEpoch",epoch}});
  });
 });
 return app.exec();
}
