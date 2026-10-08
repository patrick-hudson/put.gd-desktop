#include "ui/VisualStyle.h"
#include "storage/s3/S3Provider.h"
#include "app/LocalServer.h"
#include "app/LoginStartup.h"
#include "ui/SettingsWindow.h"
#include "ui/RecentOperations.h"
#include "ui/HistoryWindow.h"
#include "ui/TrayController.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QUuid>
#include <atomic>
#include <mutex>
#include <memory>
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
bool activateResident(const QString& view,bool background) {
 QLocalSocket socket;const auto path=putgd::LocalServer::defaultSocketPath();socket.connectToServer(path);
 if(!socket.waitForConnected(200))return false;
 if(background)return true;
 const auto id=QUuid::createUuid().toString(QUuid::WithoutBraces);
 const QJsonObject request{{"schemaVersion",1},{"requestId",id},{"session",path},{"method","ui.show"},{"params",QJsonObject{{"view",view}}}};
 socket.write(QJsonDocument(request).toJson(QJsonDocument::Compact)+'\n');
 if(!socket.waitForBytesWritten(3000))return true;
 if(!socket.waitForReadyRead(3000))qWarning("Resident activation response unavailable");
 return true;
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
 QApplication app(argc,argv);
 putgd::VisualStyle::apply(app);
 app.setDesktopFileName("putgd");
 app.setWindowIcon(putgd::VisualStyle::icon("brand"));
 QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd");QCoreApplication::setApplicationVersion("0.1.0");
 QCommandLineParser parser;parser.addHelpOption();parser.addVersionOption();parser.addOption({"background","Start in tray after setup"});parser.addOption({"history","Show upload history"});parser.addOption({"activity","Show recent activity"});parser.process(app);
 const auto backgroundOnly=parser.isSet("background")&&!parser.isSet("history")&&!parser.isSet("activity");
 const auto view=parser.isSet("history")?QStringLiteral("history"):parser.isSet("activity")?QStringLiteral("activity"):QStringLiteral("settings");
 if(activateResident(view,backgroundOnly))return 0;
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
 const auto listening=server.listen();if(!listening) {qWarning("%s",qPrintable(listening.error.code));return listening.error.code=="session_running"?(activateResident(view,backgroundOnly)?0:1):1;}
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
 // Retain each view after its first activation so drafts and selections survive hiding.
 std::unique_ptr<putgd::SettingsWindow> window;
 std::unique_ptr<putgd::RecentOperations> activity;
 std::unique_ptr<putgd::HistoryWindow> historyWindow;
 // This context disconnects source callbacks before the view owners are destroyed.
 QObject viewLifetime;
 const auto refreshSettingsDiagnostics=[&]{if(window)window->refreshDiagnostics();};
#ifdef PUTGD_NATIVE_X11
 if(native)QObject::connect(native,&putgd::X11SourceAdapter::capabilitiesChanged,&viewLifetime,refreshSettingsDiagnostics);
#endif
#ifdef PUTGD_NATIVE_GNOME
 if(gnome)QObject::connect(gnome,&putgd::GnomeAdapter::capabilitiesChanged,&viewLifetime,refreshSettingsDiagnostics);
#endif
#ifdef PUTGD_NATIVE_KDE
 if(kde)QObject::connect(kde,&putgd::KdeSourceAdapter::capabilitiesChanged,&viewLifetime,refreshSettingsDiagnostics);
#endif
 const auto settingsView=[&]() -> putgd::SettingsWindow& {
  if(!window)window=std::make_unique<putgd::SettingsWindow>(settings,credentials,tester,[&]{auto diagnostics=operations.diagnostics();diagnostics["loginStartup"]=loginStatus;diagnostics["shortcuts"]=source->capabilities()["shortcuts"];return diagnostics;});
  return *window;
 };
 const auto historyView=[&]() -> putgd::HistoryWindow& {
  if(!historyWindow)historyWindow=std::make_unique<putgd::HistoryWindow>(history,operations);
  return *historyWindow;
 };
 const auto activityView=[&]() -> putgd::RecentOperations& {
  if(!activity)activity=std::make_unique<putgd::RecentOperations>(operations);
  return *activity;
 };
 putgd::TrayController tray(operations,settingsView,historyView,activityView);
 QObject::connect(&tray,&putgd::TrayController::quitRequested,&app,&QCoreApplication::quit);
 server.setUiActivation([&](const QString& requested){if(requested=="history")tray.showHistory();else if(requested=="activity")tray.showActivity();else tray.showSettings();});
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
 app.setQuitOnLastWindowClosed(!tray.available());
 if(!parser.isSet("background")||!tray.available()||settings.revision().isEmpty()) {if(view=="history")tray.showHistory();else if(view=="activity")tray.showActivity();else tray.showSettings();}
 QObject::connect(&app,&QCoreApplication::aboutToQuit,&operations,&putgd::OperationService::shutdown);
 return app.exec();
}
