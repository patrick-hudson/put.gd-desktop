#include "VisualStyle.h"
#include <QApplication>
#include <QAction>
#include <QElapsedTimer>
#include <QFile>
#include <QFont>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMenu>
#include <QStyle>
#include <QSystemTrayIcon>
#include <QTimer>
#include <cstdio>
#include <cstdlib>
#include <malloc.h>
#include <memory>
#include <unistd.h>

namespace {
QElapsedTimer elapsed;
QString variant;
QJsonObject memory() {
 QFile file("/proc/self/smaps_rollup");
 if (!file.open(QIODevice::ReadOnly)) { std::fprintf(stderr,"smaps_rollup unavailable\n"); std::exit(3); }
 QJsonObject out;
 const auto lines=file.readAll().split('\n');
 for (const auto& line:lines) {
  const auto colon=line.indexOf(':');if(colon<0)continue;
  const auto key=line.left(colon);
  if(key!="Rss"&&key!="Pss"&&key!="Private_Clean"&&key!="Private_Dirty")continue;
  const auto parts=line.mid(colon+1).simplified().split(' ');
  bool ok=false;const auto value=parts.value(0).toLongLong(&ok);
  if(!ok||parts.value(1)!="kB"){std::fprintf(stderr,"invalid smaps_rollup field\n");std::exit(3);}
  out[QString::fromLatin1(key)+"Bytes"]=value*1024;
 }
 if(out.size()!=4){std::fprintf(stderr,"missing smaps_rollup fields\n");std::exit(3);}
 out["PrivateBytes"]=out["Private_CleanBytes"].toInteger()+out["Private_DirtyBytes"].toInteger();
 return out;
}
void sample(const QString& stage,QJsonObject extra={}) {
 extra["stage"]=stage;extra["variant"]=variant;extra["elapsedMs"]=elapsed.elapsed();
 extra["pid"]=qint64(getpid());extra["memory"]=memory();
 const auto encoded=QJsonDocument(extra).toJson(QJsonDocument::Compact);
 std::fwrite(encoded.constData(),1,encoded.size(),stdout);std::fputc('\n',stdout);std::fflush(stdout);
}
QJsonArray dependencies() {
 QFile file("/proc/self/maps");if(!file.open(QIODevice::ReadOnly))std::exit(3);
 QStringList paths;
 for(const auto& line:file.readAll().split('\n')) {
  const auto slash=line.indexOf('/');if(slash<0)continue;
  const QString path=QString::fromLocal8Bit(line.mid(slash).trimmed());
  if(path.contains(".so")&&!paths.contains(path))paths.append(path);
 }
 paths.sort();QJsonArray result;for(const auto& path:paths)result.append(path);return result;
}
}
int main(int argc,char** argv) {
 if(argc!=2||(QString::fromLocal8Bit(argv[1])!="full"&&QString::fromLocal8Bit(argv[1])!="native"))return 2;
 variant=QString::fromLocal8Bit(argv[1]);elapsed.start();
 QApplication app(argc,argv);app.setApplicationName("put.gd temporary style diagnostic");app.setQuitOnLastWindowClosed(false);
 if(app.platformName()!="xcb") {std::fprintf(stderr,"actual Cinnamon xcb required\n");return 2;}
 sample("qapplication-constructed",{{"qtVersion",qVersion()},{"platform",app.platformName()},{"font",app.font().toString()},{"styleClass",app.style()->metaObject()->className()}});
 if(variant=="full")putgd::VisualStyle::apply(app);
 sample("style-applied-or-native",{{"font",app.font().toString()},{"styleClass",app.style()->metaObject()->className()},{"styleSheetCharacters",app.styleSheet().size()}});
 app.setWindowIcon(putgd::VisualStyle::icon("brand"));
 auto menu=std::make_unique<QMenu>();
 for(const auto& action:QStringList{"region","window","desktop","clipboard"})menu->addAction(putgd::VisualStyle::icon(action),"Capture "+action);
 menu->addSeparator();menu->addAction(putgd::VisualStyle::icon("preferences"),"Settings and setup");
 menu->addAction(putgd::VisualStyle::icon("history"),"Upload history");
 menu->addAction(putgd::VisualStyle::icon("activity"),"Recent activity");
 menu->addSeparator();menu->addAction("About put.gd");menu->addAction("Quit");
 // Deliberately no actions/connections: this locates allocation cost only.
 QSystemTrayIcon tray(putgd::VisualStyle::icon("brand"));
 tray.setToolTip("put.gd — temporary style memory diagnostic");tray.setContextMenu(menu.get());
 const bool available=QSystemTrayIcon::isSystemTrayAvailable();
 if(!available){std::fprintf(stderr,"native tray unavailable\n");return 4;}
 tray.show();sample("brand-and-tray-created",{{"trayAvailable",available},{"trayVisible",tray.isVisible()},{"menuActions",menu->actions().size()}});
 QTimer::singleShot(2000,&app,[&]{const int trimmed=malloc_trim(0);sample("two-second-idle-cleanup",{{"mallocTrimReturn",trimmed}});});
 QTimer::singleShot(5000,&app,[&]{sample("quiet-settled",{{"trayVisible",tray.isVisible()},{"loadedSharedObjects",dependencies()}});tray.hide();tray.setContextMenu(nullptr);app.quit();});
 return app.exec();
}
