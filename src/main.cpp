#include "ui/VisualStyle.h"
#include "core/BuildInfo.h"
#include "storage/s3/S3Provider.h"
#include "app/LocalServer.h"
#include "app/LoginStartup.h"
#include "app/UiProcessController.h"
#include "app/SetupService.h"
#include "ui/ViewState.h"
#include "ui/TrayController.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include "app/LocalIpc.h"
#include <QUuid>
#include <atomic>
#include <mutex>
#include <memory>
#include <cstdio>
#include <cstring>
#ifdef PUTGD_NATIVE_X11
#include "platform/x11/X11SourceAdapter.h"
#endif
#ifdef PUTGD_NATIVE_GNOME
#include "platform/gnome/GnomeAdapter.h"
#endif
#ifdef PUTGD_NATIVE_KDE
#include "platform/kde/KdeSourceAdapter.h"
#endif
namespace {
// -1 means no connection was made and normal resident startup may proceed.
// Once connected, a failed UI request must never fall through to a second resident.
int activateResident(const QString& view,bool background) {
 const auto path=putgd::LocalServer::defaultSocketPath();
 if(background)return putgd::activateLocalSession(path,{}).connected?0:-1;
 const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
 const QJsonObject request{{"schemaVersion",1},{"requestId",id},{"session",path},{"method","ui.show"},{"params",QJsonObject{{"view",view}}}};
 const auto activation=putgd::activateLocalSession(path,QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');
 if(!activation.connected)return -1;
 QString code=activation.errorCode;
 if(code.isEmpty()) {
  QJsonParseError parse;const auto document=QJsonDocument::fromJson(activation.response,&parse);
  const auto response=document.object();const auto error=response["error"].toObject();
  const bool valid=parse.error==QJsonParseError::NoError&&document.isObject()&&response.size()==5&&
   response["schemaVersion"]==1&&response["requestId"]==id&&response["ok"].isBool()&&
   response["result"].isObject()&&response["error"].isObject()&&error.size()==3&&
   error["code"].isString()&&error["message"].isString()&&error["details"].isObject();
  if(!valid)code="invalid_response";
  else if(response["ok"].toBool()) {
   if(error["code"].toString().isEmpty()&&response["result"].toObject()==QJsonObject{{"shown",view}})return 0;
   code="invalid_response";
  } else if(error["code"].toString().isEmpty()||!response["result"].toObject().isEmpty())code="invalid_response";
  else {
   // Only known protocol codes are printable; never echo peer messages/details
   // or an arbitrary error-code string into launcher logs.
   const QStringList known{"ui_incompatible","ui_unavailable","ui_start_failed","ui_start_timeout","ui_show_timeout",
    "ui_close_timeout","ui_stopping","ui_busy","ui_crashed","ui_protocol","wrong_session","unsupported_version"};
   code=known.contains(error["code"].toString())?error["code"].toString():QStringLiteral("ui_activation_failed");
  }
 }
 if(code=="ui_incompatible")qWarning("ui_incompatible: The installed UI version does not match the running app. Restart put.gd after pending uploads finish.");
 else qWarning("%s: Could not confirm that the requested window opened. Retry opening the window; check the resident if this continues.",qPrintable(code));
 return 1;
}
putgd::Error checkpointFile(const QString& path,const QJsonObject& data) {
 const auto parent=QFileInfo(path).absolutePath();
 if(!QDir().mkpath(parent)||!QFile::setPermissions(parent,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner))return {"setup_journal_io","Cannot create private destination test journal",{}};
 QJsonArray checkpoints;
 QFile existing(path);
 if(existing.exists()) {
  if(!existing.open(QIODevice::ReadOnly)||existing.size()>1024*1024)return {"setup_journal_io","Cannot read bounded destination test journal",{}};
  QJsonParseError parse;const auto previous=QJsonDocument::fromJson(existing.readAll(),&parse);
  if(parse.error!=QJsonParseError::NoError||!previous.isObject())return {"setup_journal_io","Destination test journal is invalid; original target preserved",{}};
  checkpoints=previous.object()["checkpoints"].toArray();
 }
 if(checkpoints.size()>=64)return {"setup_journal_io","Destination test checkpoint limit reached; target preserved",{}};
 checkpoints.append(data);
 QSaveFile file(path);file.setDirectWriteFallback(false);
 if(!file.open(QIODevice::WriteOnly)||!file.setPermissions(QFile::ReadOwner|QFile::WriteOwner))return {"setup_journal_io","Cannot open destination test journal",{}};
 const auto bytes=QJsonDocument(QJsonObject{{"schemaVersion",1},{"checkpoints",checkpoints}}).toJson(QJsonDocument::Compact);
 if(file.write(bytes)!=bytes.size()||!file.commit())return {"setup_journal_io","Cannot commit destination test journal",{}};
 return {};
}
}
int main(int argc,char* argv[]) {
 if(argc==2&&(std::strcmp(argv[1],"--version")==0||std::strcmp(argv[1],"-v")==0)) {
  std::printf("putgd %s\n",putgd::buildVersion());return 0;
 }
 QApplication app(argc,argv);
 putgd::VisualStyle::apply(app);
 app.setDesktopFileName("putgd");
 app.setWindowIcon(putgd::VisualStyle::icon("brand"));
 QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd");QCoreApplication::setApplicationVersion(putgd::buildVersion());
 QCommandLineParser parser;parser.addHelpOption();parser.addVersionOption();parser.addOption({"background","Start in tray after setup"});parser.addOption({"history","Show upload history"});parser.addOption({"activity","Show recent activity"});parser.process(app);
 const auto backgroundOnly=parser.isSet("background")&&!parser.isSet("history")&&!parser.isSet("activity");
 const auto view=parser.isSet("history")?QStringLiteral("history"):parser.isSet("activity")?QStringLiteral("activity"):QStringLiteral("settings");
 const int activation=activateResident(view,backgroundOnly);if(activation>=0)return activation;
 putgd::CredentialStore credentials;
 putgd::ProviderRegistry providers;
 auto s3=std::make_shared<putgd::S3Provider>(credentials);providers.add("s3",s3);
 putgd::SettingsService settings(QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("config.json"),[&providers](const auto& id,const auto& value) {return providers.validate(id,value);});
 std::unique_ptr<putgd::SourceAdapter> source;
 std::function<void()> bindSource;
 const auto bindings=[&settings]{QHash<QString,QString> result;const auto snapshot=settings.snapshot();for(auto i=snapshot.actions.cbegin();i!=snapshot.actions.cend();++i)result.insert(i.key(),i.value().shortcut);return result;};
 const auto wayland=QGuiApplication::platformName().startsWith("wayland")&&qEnvironmentVariable("XDG_SESSION_TYPE")=="wayland";
 const auto desktop=qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts);

#ifdef PUTGD_NATIVE_X11
 putgd::X11SourceAdapter* native=nullptr;
 if(QGuiApplication::platformName()=="xcb"&&qEnvironmentVariable("XDG_SESSION_TYPE")!="wayland"&&qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
  auto adapter=std::make_unique<putgd::X11SourceAdapter>();native=adapter.get();source=std::move(adapter);
 }
#endif
#ifdef PUTGD_NATIVE_GNOME
 putgd::GnomeAdapter* gnome=nullptr;
 if(wayland&&desktop.contains("GNOME",Qt::CaseInsensitive)){auto adapter=std::make_unique<putgd::GnomeAdapter>();gnome=adapter.get();source=std::move(adapter);bindSource=[gnome,bindings]{gnome->bindShortcuts(bindings());};}
#endif
#ifdef PUTGD_NATIVE_KDE
 putgd::KdeSourceAdapter* kde=nullptr;
 if(wayland&&desktop.contains("KDE",Qt::CaseInsensitive)){auto adapter=std::make_unique<putgd::KdeSourceAdapter>();kde=adapter.get();source=std::move(adapter);bindSource=[kde,bindings,&settings]{kde->setResourceLimits(settings.snapshot().limits);kde->bindShortcuts(bindings());};}
#endif
 if(!source)source=std::make_unique<putgd::UnavailableSource>();
 const auto dataRoot=QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
 putgd::OperationService operations(dataRoot,settings,providers,*source);
 putgd::HistoryService history(operations);
 putgd::LocalServer server(settings,operations,history);
 const auto listening=server.listen();if(!listening) {qWarning("%s",qPrintable(listening.error.code));if(listening.error.code=="session_running"){const int retry=activateResident(view,backgroundOnly);return retry>=0?retry:1;}return 1;}
 const auto requested=[&operations](const QString& action){operations.invoke(action,[](putgd::Error error,QJsonObject){if(error)qWarning("Action rejected: %s",qPrintable(error.code));});};
#ifdef PUTGD_NATIVE_X11
 if(native) {
  const auto bind=[&] {native->bindShortcuts(bindings());};
  bind();QObject::connect(&settings,&putgd::SettingsService::accepted,&app,bind);
  QObject::connect(native,&putgd::X11SourceAdapter::actionRequested,&operations,requested);
 }
#endif
 if(bindSource){bindSource();QObject::connect(&settings,&putgd::SettingsService::accepted,&app,bindSource);}
#ifdef PUTGD_NATIVE_GNOME
 if(gnome)QObject::connect(gnome,&putgd::GnomeAdapter::actionRequested,&operations,requested);
#endif
#ifdef PUTGD_NATIVE_KDE
 if(kde)QObject::connect(kde,&putgd::KdeSourceAdapter::actionRequested,&operations,requested);
#endif
 auto tester=[&](const QString& profileId,const QJsonObject& profile,std::function<void(putgd::Error)> reply) {
  if(profile["provider"]!="s3"){reply({"test_unsupported","This provider has no destination test",{}});return;}
  const putgd::Destination destination{"s3",profileId,settings.revision(),profile["settings"].toObject(),profile["credentials"].toObject()};
  const auto path=dataRoot+"/destination-tests/"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".json";
  auto checkpoint=[path](const QJsonObject& data,std::function<void(putgd::Error)> ack){ack(checkpointFile(path,data));};
  s3->testDestination(destination,checkpoint,[path,reply](QJsonObject result,putgd::Error error){
   auto journalError=checkpointFile(path,{{"schemaVersion",1},{"phase","finished"},{"result",result},{"error",error.toJson()}});
   if(!error&&journalError)error=journalError;
   if(!error&&(!result["publicReachable"].toBool()||!QStringList{"removed","missing"}.contains(result["cleanupStatus"].toString())))error={"destination_test_incomplete","Destination test publication or cleanup needs attention; its recorded target is retained",{}};
   reply(error);
  });
 };
 QJsonObject loginStatus;
 putgd::SetupService setup(settings,credentials,tester);
 server.setSetupService(&setup);
 const auto helperPath=QDir(QCoreApplication::applicationDirPath()).filePath("putgd-ui");
 putgd::UiProcessController ui(server.socketPath(),server.residentEpoch(),helperPath,putgd::validateUiState);
 QObject::connect(&ui,&putgd::UiProcessController::helperPidChanged,&app,[&](qint64 pid){source->setUiProcess(pid);});
 const auto refreshSettingsDiagnostics=[&]{ui.invalidate();};
#ifdef PUTGD_NATIVE_X11
 if(native)QObject::connect(native,&putgd::X11SourceAdapter::capabilitiesChanged,&ui,refreshSettingsDiagnostics);
#endif
#ifdef PUTGD_NATIVE_GNOME
 if(gnome)QObject::connect(gnome,&putgd::GnomeAdapter::capabilitiesChanged,&ui,refreshSettingsDiagnostics);
#endif
#ifdef PUTGD_NATIVE_KDE
 if(kde)QObject::connect(kde,&putgd::KdeSourceAdapter::capabilitiesChanged,&ui,refreshSettingsDiagnostics);
#endif
 const auto quitApplication=[&]{ui.shutdown([&]{app.quit();});};
 putgd::TrayController tray(operations,[&](const QString& requested){ui.activate(requested);},quitApplication);
 server.setUiQuit(quitApplication);
 QObject::connect(&ui,&putgd::UiProcessController::failure,&tray,&putgd::TrayController::notifyFailure);
 server.setUiActivation([&](const QString& requested,std::function<void(putgd::Error)> reply){ui.activate(requested,std::move(reply));});
 server.setUiDiagnostics([&]{auto diagnostics=operations.diagnostics();diagnostics["loginStartup"]=loginStatus;diagnostics["shortcuts"]=source->capabilities()["shortcuts"];diagnostics["ui"]=ui.status();diagnostics["trayAvailable"]=tray.available();return diagnostics;});
 QObject::connect(&settings,&putgd::SettingsService::statusChanged,&ui,&putgd::UiProcessController::invalidate);
 QObject::connect(&operations,&putgd::OperationService::initialized,&ui,&putgd::UiProcessController::invalidate);
 QObject::connect(&operations,&putgd::OperationService::changed,&ui,&putgd::UiProcessController::invalidate);
 QObject::connect(&setup,&putgd::SetupService::changed,&ui,&putgd::UiProcessController::invalidate);
 const auto loginMutex=std::make_shared<std::mutex>();
 const auto loginGeneration=std::make_shared<std::atomic_ullong>(0);
 const auto configureLogin=[&] {
  const auto ticket=++*loginGeneration;
  const auto configRoot=QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  const auto launcher=QFileInfo(QCoreApplication::applicationFilePath()).dir().filePath("putgd-launcher");
  const auto executable=QFileInfo::exists(launcher)?launcher:QCoreApplication::applicationFilePath();
  const auto enabled=settings.snapshot().launchAtLogin;
  operations.background([configRoot,executable,enabled,loginMutex,loginGeneration,ticket](putgd::Error& error){std::lock_guard lock(*loginMutex);if(ticket!=loginGeneration->load())return QJsonObject{};error=putgd::LoginStartup::apply(configRoot,executable,enabled);return QJsonObject{{"enabled",enabled},{"applied",!error}};},[&,loginGeneration,ticket](putgd::Error error,QJsonObject status){if(ticket!=loginGeneration->load())return;loginStatus=status;if(error)loginStatus["error"]=error.toJson();refreshSettingsDiagnostics();});
 };
 configureLogin();QObject::connect(&settings,&putgd::SettingsService::accepted,&app,configureLogin);
 app.setQuitOnLastWindowClosed(false);
 if(!parser.isSet("background")||!tray.available()||settings.revision().isEmpty()) {if(view=="history")tray.showHistory();else if(view=="activity")tray.showActivity();else tray.showSettings();}
 QObject::connect(&app,&QCoreApplication::aboutToQuit,&operations,&putgd::OperationService::shutdown);
 return app.exec();
}
