#include "ui/VisualStyle.h"
#include "core/AllocatorPolicy.h"
#include "core/BuildInfo.h"
#include "core/Log.h"
#include "core/RunMarker.h"
#include "storage/s3/S3Provider.h"
#include "storage/proton/ProtonProvider.h"
#include "app/LocalServer.h"
#include "app/DiagnosticsBundle.h"
#include "app/LogService.h"
#include "app/LoginStartup.h"
#include "app/UiProcessController.h"
#include "app/RecordingHelperBackend.h"
#include "app/SetupService.h"
#include "app/StorageLocations.h"
#include "ui/ResidentTheme.h"
#include "ui/ViewState.h"
#include "ui/TrayController.h"
#include <QApplication>
#include <QScreen>
#include "platform/NativeSession.h"
#include <QCommandLineParser>
#include <QStandardPaths>
#include <QDir>
#include <QFileInfo>
#include <QSaveFile>
#include <QSysInfo>
#include <QJsonDocument>
#include "app/LocalIpc.h"
#include <QUuid>
#include <QTimer>
#include <atomic>
#include <mutex>
#include <memory>
#include <cstdio>
#include <cstring>
#if defined(Q_OS_LINUX) && defined(__GLIBC__)
#include <malloc.h>
#endif
#ifdef Q_OS_LINUX
#include "core/CrashHandler.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusReply>
#endif
#ifdef PUTGD_NATIVE_X11
#include "platform/x11/X11SourceAdapter.h"
#include "platform/x11/X11CoreSourceAdapter.h"
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
// Launcher results are for the person or script that started put.gd. Qt
// builds with journald support (Fedora) send qWarning to the journal whenever
// stderr is not a terminal, so write these lines to stderr directly.
void launcherError(const QString& line) {
 const auto bytes=line.toUtf8();std::fwrite(bytes.constData(),1,size_t(bytes.size()),stderr);std::fputc('\n',stderr);std::fflush(stderr);
}
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
 if(code=="ui_incompatible")launcherError("ui_incompatible: The installed UI version does not match the running app. Restart put.gd after pending uploads finish.");
 else launcherError(code+": Could not confirm that the requested window opened. Retry opening the window; check the resident if this continues.");
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
 if(argc==2&&std::strcmp(argv[1],"--version-json")==0){
  std::printf("%s\n",putgd::buildManifestJson());return 0;
 }
 if(argc==2&&(std::strcmp(argv[1],"--version")==0||std::strcmp(argv[1],"-v")==0)) {
  std::printf("putgd %s\n",putgd::buildVersion());return 0;
 }
 bool coreResident=false;
#ifdef PUTGD_NATIVE_X11
 coreResident=putgd::cinnamonCoreSessionEnvironment();
 // Honor an explicit non-X11 QPA request; do not route an offscreen launcher
 // into real desktop services just because its inherited DISPLAY is present.
 for(int i=1;i<argc;++i)if(std::strcmp(argv[i],"-platform")==0)coreResident=false;
#endif
 bool gnomeCoreResident=false;
#ifdef PUTGD_NATIVE_GNOME
 gnomeCoreResident=putgd::gnomeCoreSessionEnvironment()&&putgd::gnomeCoreSessionArguments(argc,argv);
#endif
 bool kdeCoreResident=false;
#ifdef PUTGD_NATIVE_KDE
 kdeCoreResident=putgd::kdeCoreSessionEnvironment()&&putgd::kdeCoreSessionArguments(argc,argv);
#endif
#if defined(Q_OS_LINUX) && defined(__GLIBC__)
 // Every resident handles large transient rasters: captures, bridge transfers
 // and Edit & upload staging/handoffs. A fixed mmap threshold returns them on
 // free instead of growing per-thread arenas that malloc_trim cannot shrink.
 // Keep the existing idle cleanup gate active for small-chunk fragmentation.
 if(!putgd::applyResidentAllocatorPolicy())
  qWarning("Could not apply the resident transient allocation threshold");
#endif
 std::unique_ptr<QCoreApplication> application;
 if(coreResident||gnomeCoreResident||kdeCoreResident)application=std::make_unique<QCoreApplication>(argc,argv);
 else {
  std::unique_ptr<QApplication> graphical;
  {putgd::GenericResidentTheme theme;graphical=std::make_unique<QApplication>(argc,argv);}
  putgd::VisualStyle::apply(*graphical);
  graphical->setDesktopFileName("putgd");
  graphical->setWindowIcon(putgd::VisualStyle::icon("brand"));
  graphical->setQuitOnLastWindowClosed(false);
  application=std::move(graphical);
 }
 auto& app=*application;
 QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd");QCoreApplication::setApplicationVersion(putgd::buildVersion());
 QCommandLineParser parser;parser.addHelpOption();parser.addVersionOption();parser.addOption({"background","Start in tray after setup"});parser.addOption({"history","Show upload history"});parser.addOption({"activity","Show recent activity"});parser.process(app);
 const auto backgroundOnly=parser.isSet("background")&&!parser.isSet("history")&&!parser.isSet("activity");
 const auto view=parser.isSet("history")?QStringLiteral("history"):parser.isSet("activity")?QStringLiteral("activity"):QStringLiteral("settings");
 const int activation=activateResident(view,backgroundOnly);if(activation>=0)return activation;
 // Resolve the private roots, moving an earlier putgd/putgd layout into place,
 // before settings or the journal open anything.
 const auto locations=putgd::StorageLocations::prepare();
 if(locations.error) {
  QStringList paths;for(const auto& value:locations.error.details)paths.append(value.toString());
  launcherError(locations.error.code+": "+locations.error.message+(paths.isEmpty()?QString():" ("+paths.join(" -> ")+")"));return 1;
 }
 putgd::CredentialStore credentials;
 putgd::ProviderRegistry providers;
 auto s3=std::make_shared<putgd::S3Provider>(credentials);providers.add("s3",s3);
 auto proton=std::make_shared<putgd::ProtonProvider>();providers.add("proton-drive",proton);
 putgd::SettingsService settings(locations.configPath(),[&providers](const auto& id,const auto& value) {return providers.validate(id,value);},nullptr,[&providers](const auto& id,const auto& value){return providers.validateCredentials(id,value);});
 std::unique_ptr<putgd::SourceAdapter> source;
 std::function<void()> bindSource;
 const auto bindings=[&settings]{QHash<QString,QString> result;const auto snapshot=settings.snapshot();for(auto i=snapshot.actions.cbegin();i!=snapshot.actions.cend();++i)result.insert(i.key(),i.value().shortcut);return result;};
 const auto wayland=gnomeCoreResident||kdeCoreResident||(!coreResident&&QGuiApplication::platformName().startsWith("wayland")&&qEnvironmentVariable("XDG_SESSION_TYPE")=="wayland");
 const auto desktop=qEnvironmentVariable("XDG_CURRENT_DESKTOP").split(':',Qt::SkipEmptyParts);

#ifdef PUTGD_NATIVE_X11
 putgd::X11SourceAdapter* native=nullptr;
 putgd::X11CoreSourceAdapter* coreNative=nullptr;
 if(coreResident){auto adapter=std::make_unique<putgd::X11CoreSourceAdapter>();coreNative=adapter.get();source=std::move(adapter);}
 if(!coreResident&&!gnomeCoreResident&&!kdeCoreResident&&QGuiApplication::platformName()=="xcb"&&qEnvironmentVariable("XDG_SESSION_TYPE")!="wayland"&&qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
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
 const auto dataRoot=locations.dataRoot;
 putgd::OperationService operations(dataRoot,settings,providers,*source);
 operations.setLocations(locations.report);
 // The recording helper is probed lazily; a missing binary reports recording_helper_missing.
 operations.setRecordingBackend(std::make_unique<putgd::RecordingHelperBackend>());
#ifdef PUTGD_NATIVE_GNOME
 // GNOME keeps clients from placing their own windows: the Shell extension
 // selects recording regions, frames them and moves the control bar.
 if(gnome)operations.recording().setShell(gnome);
#endif
 putgd::HistoryService history(operations);
 putgd::LocalServer server(settings,operations,history);
 const auto listening=server.listen();if(!listening) {launcherError(listening.error.code);if(listening.error.code=="session_running"){const int retry=activateResident(view,backgroundOnly);return retry>=0?retry:1;}return 1;}
 // This process now owns the session: open its persistent log (#74) and the
 // running marker. Helpers inherit QT_FORCE_STDERR_LOGGING, so their Qt
 // warnings reach the stderr pipe the resident collects instead of the journal.
 qputenv("QT_FORCE_STDERR_LOGGING","1");
 const auto stateRoot=putgd::logging::stateRoot();
 putgd::logging::open(stateRoot+"/logs",{settings.snapshot().logDays,settings.snapshot().logBytes},"putgd");
 putgd::RunMarker::begin(stateRoot,putgd::buildVersion());
 // The crash handler (#124) writes this resident's record to running.crash;
 // the next start turns it into a log record. Modules loaded after startup
 // (the S3 module, Qt plugins) join its map once the session has settled.
#ifdef Q_OS_LINUX
 putgd::crash::setOutput(putgd::RunMarker::crashOutput());
 QTimer::singleShot(15000,&app,[]{putgd::crash::refreshModules();});
#endif
 putgd::LogService logService(settings,stateRoot);logService.apply();
 putgd::DiagnosticsBundles bundles(operations,stateRoot);server.setDiagnosticsBundles(&bundles);
 QJsonObject portalStatus;
 QObject::connect(&app,&QCoreApplication::aboutToQuit,&logService,[]{putgd::logging::info(putgd::logApp(),"Resident stopping");});
#ifdef PUTGD_NATIVE_X11
 if(coreNative) {
  const auto bind=[&] {coreNative->bindShortcuts(bindings());};
  bind();QObject::connect(&settings,&putgd::SettingsService::accepted,&app,bind);
 }
 if(native) {
  const auto bind=[&] {native->bindShortcuts(bindings());};
  bind();QObject::connect(&settings,&putgd::SettingsService::accepted,&app,bind);
 }
#endif
 if(bindSource){bindSource();QObject::connect(&settings,&putgd::SettingsService::accepted,&app,bindSource);}
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
 setup.setProviderSetup([&](const QString& kind,const putgd::Destination& destination,const QJsonObject& input,putgd::JsonReply reply,std::function<void(QJsonObject)> event){
  if(kind=="testDestination"&&destination.providerId=="s3"){
   const QJsonObject profile{{"provider",destination.providerId},{"settings",destination.settings},{"credentials",destination.credentialReference}};
   tester(destination.profileId,profile,[reply=std::move(reply)](putgd::Error error){reply(error,{});});return;
  }
  if(destination.providerId!="proton-drive"){reply({"setup_unsupported","This provider has no account setup",{}},{});return;}
  const auto method=kind=="connectAccount"?QString("connect"):kind=="storeSharePassword"?QString("storeSharePassword"):kind;
  operations.background([proton,method,destination,input,event](putgd::Error& error){return proton->setup(method,destination,input,error,event);},std::move(reply));
 });
 server.setSetupService(&setup);
 // After the tray settles, read saved S3 credentials once so uploads work
 // without a manual Unlock; a locked keyring shows its own prompt here.
 QTimer::singleShot(3000,&setup,[&setup]{setup.unlockSavedCredentials();});
 const auto helperPath=QDir(QCoreApplication::applicationDirPath()).filePath("putgd-ui");
 bool recordingShortcut=false;
 putgd::UiProcessController ui(server.socketPath(),server.residentEpoch(),helperPath,putgd::validateUiState);
 ui.setShortcutRecordingHandler([&](bool active,std::function<void(putgd::Error)> reply){
  // Gate dispatch before asking native services to release their registrations.
  // The adapter acknowledgement, not this flag, makes capture safe to start.
  recordingShortcut=active;
#ifdef PUTGD_NATIVE_X11
  if(coreNative){coreNative->setShortcutRecording(active,std::move(reply));return;}
  if(native){native->setShortcutRecording(active,std::move(reply));return;}
#endif
#ifdef PUTGD_NATIVE_GNOME
  if(gnome){gnome->setShortcutRecording(active,std::move(reply));return;}
#endif
#ifdef PUTGD_NATIVE_KDE
  if(kde){kde->setShortcutRecording(active,std::move(reply));return;}
#endif
  reply(active?putgd::Error{"shortcut_recording_unavailable","Global shortcut recording is unavailable in this session.",{}}:putgd::Error{});
 });
 QObject::connect(&ui,&putgd::UiProcessController::helperPidChanged,&app,[&](qint64 pid){source->setUiProcess(pid);});
 const auto refreshSettingsDiagnostics=[&]{ui.invalidate();};
#ifdef PUTGD_NATIVE_X11
 if(native)QObject::connect(native,&putgd::X11SourceAdapter::capabilitiesChanged,&ui,refreshSettingsDiagnostics);
 if(coreNative)QObject::connect(coreNative,&putgd::X11CoreSourceAdapter::capabilitiesChanged,&ui,refreshSettingsDiagnostics);
#endif
#ifdef PUTGD_NATIVE_GNOME
 if(gnome)QObject::connect(gnome,&putgd::GnomeAdapter::capabilitiesChanged,&ui,refreshSettingsDiagnostics);
#endif
#ifdef PUTGD_NATIVE_KDE
 if(kde)QObject::connect(kde,&putgd::KdeSourceAdapter::capabilitiesChanged,&ui,refreshSettingsDiagnostics);
#endif
 const auto quitApplication=[&]{ui.shutdown([&]{app.quit();});};
 QTimer startupRecovery;startupRecovery.setSingleShot(true);startupRecovery.setTimerType(Qt::PreciseTimer);
 putgd::TrayController tray(operations,[&](const QString& requested){startupRecovery.stop();ui.activate(requested);},quitApplication);
 // Native shortcuts use the tray feedback with selection-safe re-entry handling.
 const auto requested=[&tray,&recordingShortcut](const QString& action){if(!recordingShortcut)tray.invokeShortcut(action);};
#ifdef PUTGD_NATIVE_X11
 if(coreNative)QObject::connect(coreNative,&putgd::X11CoreSourceAdapter::actionRequested,&tray,requested);
 if(native)QObject::connect(native,&putgd::X11SourceAdapter::actionRequested,&tray,requested);
#endif
#ifdef PUTGD_NATIVE_GNOME
 if(gnome)QObject::connect(gnome,&putgd::GnomeAdapter::actionRequested,&tray,requested);
#endif
#ifdef PUTGD_NATIVE_KDE
 if(kde)QObject::connect(kde,&putgd::KdeSourceAdapter::actionRequested,&tray,requested);
#endif
 server.setUiQuit(quitApplication);
 QObject::connect(&ui,&putgd::UiProcessController::failure,&tray,&putgd::TrayController::notifyFailure);
 server.setUiActivation([&](const QString& requested,std::function<void(putgd::Error)> reply){startupRecovery.stop();ui.activate(requested,std::move(reply));});
 server.setUiViewState([&]{return ui.viewState();});
 server.setUiCommand([&](const QString& name,const QJsonObject& params,putgd::JsonReply reply){ui.command(name,params,std::move(reply));});
 // Save diagnostics for a failed upload from its notification, into the folder Settings last chose (#122).
 tray.setDiagnosticsSaver([&](const QString& id,putgd::JsonReply reply){putgd::BundleRequest request;request.operationId=id;const auto folder=operations.settingsSnapshot().document["preferences"].toObject()["diagnostics"].toObject()["packageFolder"].toString();if(QFileInfo(folder).isDir())request.output=folder;bundles.create(request,std::move(reply));});
 server.setDiagnosticsNotifications([&](const QJsonObject& result){tray.notifyDiagnosticsSaved(result);},&putgd::TrayController::revealPackage);
 const auto fullDiagnostics=[&]{auto diagnostics=operations.diagnostics();diagnostics["loginStartup"]=loginStatus;diagnostics["shortcuts"]=source->capabilities()["shortcuts"];diagnostics["ui"]=ui.status();diagnostics["trayAvailable"]=tray.available();return diagnostics;};
 server.setUiDiagnostics(fullDiagnostics);
 bundles.setDiagnostics(fullDiagnostics);
 // environment.json's session-side facts: no pixels, only geometry scale.
 bundles.setEnvironment([&]{
  QJsonArray screens;
  if(qobject_cast<QGuiApplication*>(QCoreApplication::instance()))for(const auto* screen:QGuiApplication::screens())screens.append(QJsonObject{{"devicePixelRatio",screen->devicePixelRatio()},{"logicalDpi",screen->logicalDotsPerInch()}});
  return QJsonObject{{"platform",coreResident||gnomeCoreResident||kdeCoreResident?QStringLiteral("none"):QGuiApplication::platformName()},
   {"resident",coreResident?"x11-core":gnomeCoreResident?"gnome-core":kdeCoreResident?"kde-core":"graphical"},{"trayAvailable",tray.available()},
   {"portal",portalStatus},{"screenCount",screens.size()},{"screens",screens}};
 });
 QObject::connect(&settings,&putgd::SettingsService::statusChanged,&ui,&putgd::UiProcessController::invalidate);
 QObject::connect(&operations,&putgd::OperationService::initialized,&ui,&putgd::UiProcessController::invalidate);
 QObject::connect(&operations,&putgd::OperationService::changed,&ui,&putgd::UiProcessController::invalidate);
 QObject::connect(&history,&putgd::HistoryService::changed,&ui,&putgd::UiProcessController::invalidate);
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
 const auto showInitialView=[&]{if(view=="history")tray.showHistory();else if(view=="activity")tray.showActivity();else tray.showSettings();};
 if(!parser.isSet("background")||settings.revision().isEmpty())showInitialView();
 else if(!tray.available()) {
  // XDG autostart can precede the desktop's tray watcher or host. Give them a
  // bounded startup grace period before exposing the no-tray recovery window.
  QObject::connect(&startupRecovery,&QTimer::timeout,&tray,[&]{if(!tray.available())showInitialView();});
  startupRecovery.start(10000);
 }
 {
  QJsonObject startup{{"build",QJsonDocument::fromJson(putgd::buildManifestJson()).object()},{"os",QSysInfo::prettyProductName()},
   {"kernel",QSysInfo::kernelVersion()},{"architecture",QSysInfo::currentCpuArchitecture()},{"desktop",qEnvironmentVariable("XDG_CURRENT_DESKTOP")},
   {"sessionType",qEnvironmentVariable("XDG_SESSION_TYPE")},{"qtRuntime",qVersion()},{"qtCompiled",QT_VERSION_STR},
   {"platform",coreResident||gnomeCoreResident||kdeCoreResident?QStringLiteral("none"):QGuiApplication::platformName()},
   {"resident",coreResident?"x11-core":gnomeCoreResident?"gnome-core":kdeCoreResident?"kde-core":"graphical"},
   {"trayAvailable",tray.available()},{"locations",locations.report},{"capabilities",source->capabilities()}};
  putgd::logging::info(putgd::logApp(),"Resident started",startup);
#ifdef Q_OS_LINUX
  // The session bus may be slow to answer; ask off the session thread.
  operations.background([](putgd::Error&){
   auto bus=QDBusConnection::sessionBus();
   if(!bus.isConnected())return QJsonObject{{"sessionBus",false}};
   const auto running=bus.interface()->isServiceRegistered("org.freedesktop.portal.Desktop");
   const QDBusReply<QStringList> activatable=bus.interface()->call("ListActivatableNames");
   return QJsonObject{{"sessionBus",true},{"portalRunning",running.isValid()&&running.value()},{"portalActivatable",activatable.isValid()&&activatable.value().contains("org.freedesktop.portal.Desktop")}};
  },[&portalStatus](putgd::Error,QJsonObject portal){portalStatus=portal;putgd::logging::info(putgd::logApp(),"Desktop portal availability",portal);});
#endif
 }
 QObject::connect(&app,&QCoreApplication::aboutToQuit,&operations,[&]{proton->shutdown();operations.shutdown();});
 const int status=app.exec();
 putgd::logging::info(putgd::logApp(),"Resident stopped",{{"exitCode",status}});
#ifdef Q_OS_LINUX
 putgd::crash::setOutput(-1);
#endif
 putgd::RunMarker::end();
 putgd::logging::close();
 return status;
}
