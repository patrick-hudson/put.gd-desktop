#include <QApplication>
#include <QTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonDocument>
#include <QFont>
#include <cstdio>
#include <unistd.h>
int main(int argc,char**argv){
 QApplication app(argc,argv);
 app.setQuitOnLastWindowClosed(false);
 QTimer::singleShot(5000,&app,[&]{
  QFile maps("/proc/self/maps");maps.open(QIODevice::ReadOnly);const auto mapped=maps.readAll();
  const QJsonObject result{{"pid",static_cast<qint64>(getpid())},{"qtVersion",qVersion()},
   {"platform",QGuiApplication::platformName()},{"fontFamily",app.font().family()},
   {"fontPointSize",app.font().pointSizeF()},{"gtkPlatformThemeMapped",mapped.contains("libqgtk3.so")}};
  const auto bytes=QJsonDocument(result).toJson(QJsonDocument::Compact);
  fwrite(bytes.constData(),1,bytes.size(),stdout);fputc('\n',stdout);fflush(stdout);
 });
 QTimer::singleShot(10000,&app,&QApplication::quit);
 return app.exec();
}
