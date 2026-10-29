// History gallery responsiveness: opens History with an established synthetic
// history (N image captures with previews, some failed), switches to the
// gallery and times the switch, full repaints, scrolling and hover, and counts
// viewport paints while idle. Synthetic measurement only; not desktop evidence.
#include "fixtures/UiSessionFixture.h"
#include "fixtures/Waits.h"
#include "ui/UiApplicationController.h"
#include "ui/MainWindow.h"
#include <QApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListView>
#include <QMouseEvent>
#include <QPainter>
#include <QSaveFile>
#include <QScrollBar>
#include <QStandardPaths>
#include <QToolButton>
#include <algorithm>
using namespace putgd;
using namespace putgd::test;
namespace {
class ControlDevice final:public QIODevice {
public:
 QByteArray input,output;quint64 sequence=0;
 ControlDevice(){open(ReadWrite|Unbuffered);}
 bool isSequential()const override{return true;}
 qint64 bytesAvailable()const override{return input.size();}
 qint64 readData(char* data,qint64 max)override{const auto n=qMin(max,qint64(input.size()));memcpy(data,input.constData(),n);input.remove(0,n);return n;}
 qint64 writeData(const char* data,qint64 n)override{output.append(data,n);if(output.size()>(1<<20))output=output.right(1<<16);return n;}
 void send(const QString& type,QJsonObject payload={}){input+=QJsonDocument(QJsonObject{{"version",1},{"type",type},{"sequence",QString::number(++sequence)},{"payload",payload}}).toJson(QJsonDocument::Compact)+'\n';emit readyRead();}
};
class PaintCounter final:public QObject {
public:int paints=0;
protected:bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Paint)++paints;return false;}
};
bool waitUntil(const std::function<bool()>& condition,int ms=8000){return putgd::test::waitFor(condition,ms);}
QImage capture(int index){
 QImage image(1920,1080,QImage::Format_RGB32);image.fill(QColor::fromHsv((index*37)%360,90,70));QPainter p(&image);
 p.setPen(Qt::white);QFont font;font.setPixelSize(64);p.setFont(font);p.drawText(image.rect(),Qt::AlignCenter,QString("Synthetic %1").arg(index));return image;
}
double median(QList<double> values){if(values.isEmpty())return 0;std::sort(values.begin(),values.end());const auto n=values.size();return n%2?values[n/2]:(values[n/2-1]+values[n/2])/2;}
QJsonObject summary(const QList<double>& values){return {{"median",median(values)},{"max",values.isEmpty()?0:*std::max_element(values.begin(),values.end())},{"min",values.isEmpty()?0:*std::min_element(values.begin(),values.end())}};}
qint64 rssKb(){QFile status("/proc/self/status");if(!status.open(QIODevice::ReadOnly))return -1;for(const auto& line:status.readAll().split('\n'))if(line.startsWith("VmRSS:"))return line.mid(6).trimmed().split(' ').value(0).toLongLong();return -1;}
double ms(const QElapsedTimer& t){return t.nsecsElapsed()/1e6;}
}
int main(int argc,char** argv){
 QApplication app(argc,argv);app.setApplicationName("putgd-bench-history-gallery");
 const auto args=app.arguments();if(args.size()<2){qCritical("Usage: putgd-bench-history-gallery OUTPUT.json [ITEMS] [ROUNDS]");return 2;}
 const int items=args.size()>2?qBound(1,args.at(2).toInt(),2000):50,rounds=args.size()>3?qBound(2,args.at(3).toInt(),100):10;
 const auto socket=QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).filePath("putgd/session.sock");if(socket.toUtf8().size()>107){qCritical("Resolved Unix socket exceeds 107 UTF-8 bytes");return 2;}
 QTemporaryDir data("/tmp/pgh-XXXXXX");if(!data.isValid())return 2;
 auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=data.filePath("copies");doc["archive"]=archive;
 SettingsService settings(data.filePath("config.json"),[](const QString&,const QJsonObject&){return Error{};});if(!settings.save(doc,{}))return 2;
 ProviderRegistry registry;Source source;OperationService operations(data.filePath("data"),settings,registry,source);if(!waitUntil([&]{return operations.ready();}))return 2;
 const auto now=QDateTime::currentDateTimeUtc();
 {HistoryStore ledger(operations.databasePath());for(int i=0;i<items;++i){
  const auto path=data.filePath(QString("synthetic-%1.png").arg(i));if(!capture(i).save(path))return 2;
  const bool failed=i%7==3;
  Operation op;op.id=QString("synthetic-operation-%1").arg(i);op.action=i%3==0?"region":i%3==1?"window":"desktop";op.created=now.addSecs(-60-i*7200).toString(Qt::ISODateWithMs);op.state=failed?"failed":"completed";op.copyStatus="not_requested";op.settings=settings.snapshot();
  OperationItem item;item.artifact.id=QString("synthetic-file-%1").arg(i);item.artifact.name=QString("Screenshot from 2026-10-%1 %2.png").arg(1+i%28,2,10,QChar('0')).arg(i);item.artifact.mime="image/png";item.artifact.category="images";item.artifact.path=path;item.artifact.bytes=QFileInfo(path).size();item.artifact.previewPath=path;item.local.status="saved";item.local.path=path;
  item.uploadStatus=failed?"failed":"local_only";if(failed)item.error={"synthetic_failure","The connection was interrupted while sending the file. Check the destination and retry.",{}};
  op.items.append(item);if(ledger.saveOperation(op))return 2;}}
 HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
 const auto rssStart=rssKb();
 QElapsedTimer open;open.start();
 device.send("activate",{{"view","history"}});
 MainWindow* window=nullptr;for(auto* w:QApplication::topLevelWidgets())if(auto* m=qobject_cast<MainWindow*>(w))window=m;
 if(!window){qCritical("Main window did not open");return 2;}window->resize(1320,880);
 auto* list=window->findChild<QListView*>("historyList");if(!list||!waitUntil([&]{return list->model()->rowCount()==qMin(items,50);})){qCritical("Synthetic history did not load: %d rows",list?list->model()->rowCount():-1);return 2;}
 QCoreApplication::sendPostedEvents();window->repaint();const double listOpenMs=ms(open);
 putgd::test::settle(500);
 PaintCounter counter;list->viewport()->installEventFilter(&counter);
 // Gallery switch: click, settle the posted layout, paint the next frame.
 QElapsedTimer t;t.start();window->findChild<QToolButton*>("historyGridMode")->click();QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();window->repaint();const double gridSwitchMs=ms(t);
 // Previews load in the background; wait for them before steady-state timing.
 putgd::test::settle(3000);const auto rssGallery=rssKb();
 QList<double> repaint,scroll,hover;
 // Turn the event loop before each sample, as between frames: per-pass layout memos expire then.
 for(int round=0;round<rounds;++round){QCoreApplication::processEvents();t.restart();list->viewport()->repaint();repaint.append(ms(t));}
 auto* bar=list->verticalScrollBar();
 for(int round=0;round<rounds;++round){QCoreApplication::processEvents();t.restart();bar->setValue(round%2?0:qMin(bar->maximum(),bar->pageStep()));QCoreApplication::sendPostedEvents();list->viewport()->repaint();scroll.append(ms(t));}
 bar->setValue(0);QCoreApplication::processEvents();
 // Hover over cards in turn, as the pointer moving across the grid.
 QList<QRect> visible;for(int row=0;row<list->model()->rowCount();++row)if(const auto rect=list->visualRect(list->model()->index(row,0));list->viewport()->rect().contains(rect.center()))visible.append(rect);
 if(visible.isEmpty()){qCritical("No gallery card is visible");return 2;}
 for(int round=0;round<rounds;++round){const auto rect=visible.at(round%qMin(6,int(visible.size())));QCoreApplication::processEvents();t.restart();QMouseEvent move(QEvent::MouseMove,QPointF(rect.center()),list->viewport()->mapToGlobal(QPointF(rect.center())),Qt::NoButton,Qt::NoButton,Qt::NoModifier);QApplication::sendEvent(list->viewport(),&move);QCoreApplication::sendPostedEvents();QCoreApplication::processEvents();list->viewport()->repaint();hover.append(ms(t));}
 // Idle with a card hovered: a settled view should not keep repainting.
 putgd::test::settle(300);counter.paints=0;QElapsedTimer idle;idle.start();putgd::test::settle(2000);const int idlePaints=counter.paints;const double idleMs=ms(idle);
 const auto rssEnd=rssKb();
 const QJsonObject result{{"kind","synthetic-history-gallery-timing"},{"desktopAcceptance",false},{"qt",QString::fromLatin1(qVersion())},{"platform",QGuiApplication::platformName()},{"historyItems",items},{"rows",list->model()->rowCount()},{"rounds",rounds},
  {"listOpenMs",listOpenMs},{"gridSwitchMs",gridSwitchMs},{"repaintMs",summary(repaint)},{"scrollMs",summary(scroll)},{"hoverMs",summary(hover)},
  {"idlePaintsPerSecond",idlePaints*1000.0/idleMs},{"rssKb",QJsonObject{{"start",rssStart},{"gallery",rssGallery},{"end",rssEnd}}}};
 QSaveFile out(args.at(1));if(!out.open(QIODevice::WriteOnly))return 2;out.write(QJsonDocument(result).toJson(QJsonDocument::Indented));if(!out.commit())return 2;
 std::fprintf(stdout,"%s\n",QJsonDocument(result).toJson(QJsonDocument::Compact).constData());std::fflush(stdout);
 // PUTGD_BENCH_HOLD_MS keeps the hovered gallery on screen for a window capture.
 if(const int hold=qEnvironmentVariableIntValue("PUTGD_BENCH_HOLD_MS");hold>0){window->raise();window->activateWindow();putgd::test::settle(qMin(hold,120000));}
 return 0;
}
