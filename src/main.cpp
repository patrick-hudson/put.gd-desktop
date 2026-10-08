#include "storage/s3/SdkRuntime.h"
#include "app/LocalServer.h"
#include <QApplication>
#include <QLabel>
#include <QMainWindow>
#include <QStandardPaths>
#include <QDir>
int main(int argc,char* argv[]) {
 QApplication app(argc,argv);
 QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd");QCoreApplication::setApplicationVersion("0.1.0");
 putgd::SdkRuntime sdk;
 putgd::ProviderRegistry providers;
 putgd::SettingsService settings(QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation)).filePath("config.json"),[&providers](const auto& id,const auto& value) {return providers.validate(id,value);});
 putgd::UnavailableSource source;
 putgd::OperationService operations(QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation),settings,providers,source);
 putgd::HistoryService history(operations);
 putgd::LocalServer server(settings,operations,history);
 const auto listening=server.listen();if(!listening) {qWarning("%s",qPrintable(listening.error.code));return listening.error.code=="session_running"?0:1;}
 QMainWindow window;window.setWindowTitle("put.gd");window.setCentralWidget(new QLabel("Shared operation services ready. Destination and native desktop integration setup required.",&window));window.resize(720,480);window.show();return app.exec();
}
