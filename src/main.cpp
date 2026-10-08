#include "storage/s3/SdkRuntime.h"
#include "storage/s3/S3Provider.h"
#include "app/LocalServer.h"
#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QStandardPaths>
#include <QDir>
#ifdef PUTGD_NATIVE_X11
#include "platform/x11/X11SourceAdapter.h"
#endif
int main(int argc,char* argv[]) {
 QApplication app(argc,argv);
 QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd");QCoreApplication::setApplicationVersion("0.1.0");
 putgd::SdkRuntime sdk;
 putgd::CredentialStore credentials;
 putgd::ProviderRegistry providers;
 providers.add("s3",std::make_shared<putgd::S3Provider>(credentials));
 putgd::SettingsService settings(QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("config.json"),[&providers](const auto& id,const auto& value) {return providers.validate(id,value);});
 std::unique_ptr<putgd::SourceAdapter> source;
#ifdef PUTGD_NATIVE_X11
 putgd::X11SourceAdapter* native=nullptr;
 if(QGuiApplication::platformName()=="xcb"&&qEnvironmentVariable("XDG_SESSION_TYPE")!="wayland"&&qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY")) {
  auto adapter=std::make_unique<putgd::X11SourceAdapter>();native=adapter.get();source=std::move(adapter);
 }
#endif
 if(!source)source=std::make_unique<putgd::UnavailableSource>();
 putgd::OperationService operations(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),settings,providers,*source);
#ifdef PUTGD_NATIVE_X11
 if(native) {
  const auto bind=[&] {QHash<QString,QString> bindings;const auto snapshot=settings.snapshot();for(auto i=snapshot.actions.cbegin();i!=snapshot.actions.cend();++i)bindings.insert(i.key(),i.value().shortcut);native->bindShortcuts(bindings);};
  bind();QObject::connect(&settings,&putgd::SettingsService::accepted,&app,bind);
  QObject::connect(native,&putgd::X11SourceAdapter::actionRequested,&operations,[&](const QString& action){operations.invoke(action,[](putgd::Error error,QJsonObject){if(error)qWarning("Action rejected: %s",qPrintable(error.code));});});
 }
#endif
 putgd::HistoryService history(operations);
 putgd::LocalServer server(settings,operations,history);
 const auto listening=server.listen();if(!listening) {qWarning("%s",qPrintable(listening.error.code));return listening.error.code=="session_running"?0:1;}
 QMainWindow window;window.setWindowTitle("put.gd");window.setCentralWidget(new QLabel("Shared operation services ready. Destination and native desktop integration setup required.",&window));window.resize(720,480);window.show();return app.exec();
}
