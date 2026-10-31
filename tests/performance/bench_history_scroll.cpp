// History list keyboard paging (#1064): opens History in list mode over an
// established synthetic history (default 10,000 image/video captures with
// previews), focuses the list and times each PageDown from the key press to the
// end of the list's next viewport paint, the frame that shows the new selection.
// Synthetic measurement only; not desktop evidence.
#include "fixtures/UiSessionFixture.h"
#include "fixtures/Waits.h"
#include "ui/UiApplicationController.h"
#include "ui/MainWindow.h"
#include "ui/HistoryModel.h"
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
#include <QPlainTextEdit>
#include <QPainter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTest>
#include <algorithm>
#include <cmath>
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
// Counts viewport paints; a sample ends when the paint the key caused has run.
class PaintCounter final:public QObject {
public:int paints=0;
protected:bool eventFilter(QObject*,QEvent* event)override{if(event->type()==QEvent::Paint)++paints;return false;}
};
QImage picture(int index,QSize size){
 QImage image(size,QImage::Format_RGB32);image.fill(QColor::fromHsv((index*53)%360,90,70));QPainter p(&image);
 p.setPen(Qt::white);QFont font;font.setPixelSize(size.height()/8);p.setFont(font);p.drawText(image.rect(),Qt::AlignCenter,QString("Synthetic %1").arg(index));return image;
}
QJsonObject summary(QList<double> values){
 std::sort(values.begin(),values.end());if(values.isEmpty())return {};
 const auto at=[&](double q){return values.at(qBound(qsizetype(0),qsizetype(std::ceil(values.size()*q))-1,values.size()-1));};
 return {{"samples",int(values.size())},{"p50",values.at((values.size()-1)/2)},{"p95",at(.95)},{"max",values.last()},{"min",values.first()}};
}
double ms(const QElapsedTimer& t){return t.nsecsElapsed()/1e6;}
// Selection clears the details view; the item's history.detail reply refills it.
bool detailShown(const QListView* list,const QPlainTextEdit* details){
 const auto id=list->currentIndex().data(HistoryModel::RecordRole).toJsonObject()["id"].toString();
 return !id.isEmpty()&&details->toPlainText().contains(QStringLiteral("\"%1\"").arg(id));
}
struct Sample {double handled=0,painted=0,detailed=0;bool ok=false;};
// One timed PageDown, kept out of line so a profiler can collect only these.
Q_NEVER_INLINE Sample pageDown(QListView* list,QPlainTextEdit* details,const PaintCounter& counter){
 Sample sample;const int before=list->currentIndex().row(),paints=counter.paints;
 QElapsedTimer t;t.start();QTest::keyClick(list,Qt::Key_PageDown);sample.handled=ms(t);
 if(list->currentIndex().row()==before){qCritical("PageDown did not move the selection");return sample;}
 while(counter.paints==paints&&t.elapsed()<5000)QCoreApplication::processEvents();
 if(counter.paints==paints){qCritical("PageDown painted no frame within 5 s");return sample;}
 sample.painted=ms(t);
 // The inspector follows: its history.detail reply fills the details view.
 if(!waitFor([&]{return detailShown(list,details);}))return sample;
 sample.detailed=ms(t);sample.ok=true;return sample;
}
}
int main(int argc,char** argv){
 QApplication app(argc,argv);app.setApplicationName("putgd-bench-history-scroll");
 const auto args=app.arguments();if(args.size()<2){qCritical("Usage: putgd-bench-history-scroll OUTPUT.json [ITEMS] [ROUNDS]");return 2;}
 const int items=args.size()>2?qBound(50,args.at(2).toInt(),10000):10000,rounds=args.size()>3?qBound(1,args.at(3).toInt(),50):5;
 const auto socket=QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).filePath("putgd/session.sock");if(socket.toUtf8().size()>107){qCritical("Resolved Unix socket exceeds 107 UTF-8 bytes");return 2;}
 QTemporaryDir data(QDir::tempPath()+"/pgs-XXXXXX");if(!data.isValid())return 2;
 auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=data.filePath("copies");doc["archive"]=archive;
 SettingsService settings(data.filePath("config.json"),[](const QString&,const QJsonObject&){return Error{};});if(!settings.save(doc,{}))return 2;
 ProviderRegistry registry;Source source;OperationService operations(data.filePath("data"),settings,registry,source);if(!waitFor([&]{return operations.ready();}))return 2;
 // Six originals and their previews, as the physical fixture's six publications;
 // every capture names its own preview path, written for the first pages.
 QStringList originals;for(int i=0;i<6;++i){originals.append(data.filePath(QString("original-%1.png").arg(i)));if(!picture(i,{1920,1080}).save(originals.last()))return 2;}
 const auto previews=QFileInfo(operations.databasePath()).absolutePath()+"/previews";if(!QDir().mkpath(previews))return 2;
 const auto now=QDateTime::currentDateTimeUtc();QElapsedTimer seed;seed.start();
 {HistoryStore ledger(operations.databasePath());for(int i=0;i<items;++i){
  const bool video=i%2;Operation op;op.id=QString("synthetic-operation-%1").arg(i);op.action=video?"record":i%4==0?"region":"window";
  op.created=now.addSecs(-60-i*600).toString(Qt::ISODateWithMs);op.state="completed";op.copyStatus="copied";op.settings=settings.snapshot();
  OperationItem item;item.artifact.id=QString("synthetic-file-%1").arg(i);item.artifact.name=video?QString("Recording %1.webm").arg(i):QString("Screenshot %1.png").arg(i);
  item.artifact.mime=video?"video/webm":"image/png";item.artifact.category=video?"videos":"images";item.artifact.path=originals.at(i%6);item.artifact.bytes=QFileInfo(originals.at(i%6)).size();
  item.artifact.previewPath=previews+"/"+item.artifact.id+".png";if(i<200&&!picture(i,{960,540}).save(item.artifact.previewPath))return 2;
  item.uploadStatus="local_only";item.local.status="saved";item.local.path=originals.at(i%6);
  op.items.append(item);if(ledger.saveOperation(op))return 2;}}
 const double seedMs=ms(seed);
 HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
 device.send("activate",{{"view","history"}});
 MainWindow* window=nullptr;for(auto* w:QApplication::topLevelWidgets())if(auto* m=qobject_cast<MainWindow*>(w))window=m;
 if(!window){qCritical("Main window did not open");return 2;}window->resize(1320,880);
 auto* list=window->findChild<QListView*>("historyList");auto* details=window->findChild<QPlainTextEdit*>("historyDetails");
 if(!list||!details||!waitFor([&]{return list->model()->rowCount()==50;},20000)){qCritical("Synthetic history did not load: %d rows",list?list->model()->rowCount():-1);return 2;}
 window->activateWindow();list->setFocus();list->setCurrentIndex(list->model()->index(0,0));
 // Thumbnails and the first preview load in the background before timing.
 settle(2000);
 PaintCounter counter;list->viewport()->installEventFilter(&counter);
 QList<double> keyToPaint,keyHandled,keyToDetail;int steps=0;
 for(int round=0;round<rounds;++round){
  QTest::keyClick(list,Qt::Key_Home);if(!waitFor([&]{return detailShown(list,details);}))return 2;settle(300);
  while(list->currentIndex().row()<list->model()->rowCount()-1){
   QCoreApplication::processEvents();settle(100);
   const auto sample=pageDown(list,details,counter);if(!sample.ok)return 2;
   keyHandled.append(sample.handled);keyToPaint.append(sample.painted);keyToDetail.append(sample.detailed);++steps;
  }
 }
 const QJsonObject result{{"kind","synthetic-history-list-paging"},{"desktopAcceptance",false},{"qt",QString::fromLatin1(qVersion())},{"platform",QGuiApplication::platformName()},
  {"historyItems",items},{"rows",list->model()->rowCount()},{"rounds",rounds},{"steps",steps},{"seedMs",seedMs},
  {"keyHandledMs",summary(keyHandled)},{"keyToPaintMs",summary(keyToPaint)},{"keyToDetailMs",summary(keyToDetail)}};
 QSaveFile out(args.at(1));if(!out.open(QIODevice::WriteOnly))return 2;out.write(QJsonDocument(result).toJson(QJsonDocument::Indented));if(!out.commit())return 2;
 std::fprintf(stdout,"%s\n",QJsonDocument(result).toJson(QJsonDocument::Compact).constData());std::fflush(stdout);
 return 0;
}
