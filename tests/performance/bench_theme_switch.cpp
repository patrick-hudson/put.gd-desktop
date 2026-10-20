// Theme switch timing (#237): picks Dark and Light in Settings → Appearance
// with History (gallery view, synthetic captures) built and Settings showing,
// through the production UiApplicationController and an in-process resident.
// Synthetic measurement only. This does not establish desktop support.
#include "fixtures/UiSessionFixture.h"
#include "ui/UiApplicationController.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QListView>
#include <QPainter>
#include <QSaveFile>
#include <QStandardPaths>
#include <QTabWidget>
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
bool waitUntil(const std::function<bool()>& condition,int ms=8000){QElapsedTimer timer;timer.start();while(!condition()&&timer.elapsed()<ms)QTest::qWait(10);return condition();}
QImage capture(int index){
 QImage image(960,600,QImage::Format_RGB32);image.fill(QColor::fromHsv((index*37)%360,90,70));QPainter p(&image);
 p.setPen(Qt::white);QFont font;font.setPixelSize(48);p.setFont(font);p.drawText(image.rect(),Qt::AlignCenter,QString("Synthetic %1").arg(index));return image;
}
double median(QList<double> values){if(values.isEmpty())return 0;std::sort(values.begin(),values.end());const auto n=values.size();return n%2?values[n/2]:(values[n/2-1]+values[n/2])/2;}
QJsonObject summary(const QList<double>& values){return {{"median",median(values)},{"max",values.isEmpty()?0:*std::max_element(values.begin(),values.end())},{"min",values.isEmpty()?0:*std::min_element(values.begin(),values.end())}};}
}
int main(int argc,char** argv){
 QApplication app(argc,argv);app.setApplicationName("putgd-bench-theme-switch");
 const auto args=app.arguments();if(args.size()<2){qCritical("Usage: putgd-bench-theme-switch OUTPUT.json [ROUNDS]");return 2;}
 const int rounds=args.size()>2?qBound(2,args.at(2).toInt(),100):12;
 const auto socket=QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).filePath("putgd/session.sock");if(socket.toUtf8().size()>107){qCritical("Resolved Unix socket exceeds 107 UTF-8 bytes");return 2;}
 // Connected first: every slot below runs after the production restyle slots.
 QElapsedTimer clock;clock.start();double changeStart=-1;
 QObject::connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,&app,[&]{changeStart=clock.nsecsElapsed()/1e6;});
 QTemporaryDir data("/tmp/pgt-XXXXXX");if(!data.isValid())return 2;
 auto doc=Settings::developmentTemplate();auto archive=doc["archive"].toObject();archive["root"]=data.filePath("copies");doc["archive"]=archive;
 SettingsService settings(data.filePath("config.json"),[](const QString&,const QJsonObject&){return Error{};});if(!settings.save(doc,{}))return 2;
 ProviderRegistry registry;Source source;OperationService operations(data.filePath("data"),settings,registry,source);if(!waitUntil([&]{return operations.ready();}))return 2;
 const int items=24;const auto now=QDateTime::currentDateTimeUtc();
 {HistoryStore ledger(operations.databasePath());for(int i=0;i<items;++i){const auto path=data.filePath(QString("synthetic-%1.png").arg(i));if(!capture(i).save(path))return 2;Operation op;op.id=QString("synthetic-operation-%1").arg(i);op.action="region";op.created=now.addSecs(-60-i*30).toString(Qt::ISODateWithMs);op.state="completed";op.copyStatus="not_requested";op.settings=settings.snapshot();OperationItem item;item.artifact.id=QString("synthetic-file-%1").arg(i);item.artifact.name=QString("Screenshot %1.png").arg(i);item.artifact.mime="image/png";item.artifact.category="images";item.artifact.bytes=QFileInfo(path).size();item.artifact.previewPath=path;item.local.status="saved";item.local.path=path;item.uploadStatus="not_requested";op.items.append(item);if(ledger.saveOperation(op))return 2;}}
 HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);ControlDevice device;UiControl control(device);UiApplicationController controller(fixture.session,control,{});
 double accepted=-1;QObject::connect(&settings,&SettingsService::accepted,&app,[&]{if(accepted<0)accepted=clock.nsecsElapsed()/1e6;});
 device.send("activate",{{"view","history"}});
 auto* window=qobject_cast<MainWindow*>(QApplication::topLevelWidgets().isEmpty()?nullptr:[]{for(auto* w:QApplication::topLevelWidgets())if(qobject_cast<MainWindow*>(w))return w;return static_cast<QWidget*>(nullptr);}());
 if(!window){qCritical("Main window did not open");return 2;}window->resize(1320,880);
 auto* list=window->findChild<QListView*>("historyList");if(!list||!waitUntil([&]{return list->model()->rowCount()==items;})){qCritical("Synthetic history did not load");return 2;}
 window->findChild<QToolButton*>("historyGridMode")->click();QTest::qWait(500);
 device.send("activate",{{"view","settings"}});window->findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(0);
 auto* combo=window->findChild<QComboBox*>("appearanceTheme");if(!combo)return 2;
 if(!waitUntil([&]{return window->isVisible();}))return 2;QTest::qWait(800);
 // Connected last: the production restyle has run; settle posted layout
 // events, then repaint every visible window, as the next frame would.
 double restyled=-1,painted=-1;
 QObject::connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,&app,[&]{
  restyled=clock.nsecsElapsed()/1e6;QCoreApplication::sendPostedEvents();
  for(auto* top:QApplication::topLevelWidgets())if(top->isVisible())top->repaint();
  painted=clock.nsecsElapsed()/1e6;
 });
 QJsonArray picks,direct;QList<double> pickToPainted,pickToApplied,restyle,paint,pickToAccepted,directRestyle,directPaint;
 for(int round=0;round<rounds;++round){
  const auto target=round%2==0?Theme::Scheme::Light:Theme::Scheme::Dark;
  changeStart=restyled=painted=accepted=-1;
  if(qEnvironmentVariableIsSet("PUTGD_BENCH_DEBUG"))std::fprintf(stderr,"round %d: scheme %s, target %s\n",round,qPrintable(Theme::name(Theme::scheme())),qPrintable(Theme::name(target)));
  const double picked=clock.nsecsElapsed()/1e6;
  combo->setCurrentIndex(combo->findData(Theme::name(target)));
  if(!waitUntil([&]{return Theme::scheme()==target&&painted>=0;},5000)){qCritical("Theme did not switch: scheme %s, combo %s, accepted %.1f, painted %.1f, saved theme %s",qPrintable(Theme::name(Theme::scheme())),qPrintable(combo->currentData().toString()),accepted,painted,qPrintable(settings.snapshot().appearanceTheme));return 2;}
  if(!waitUntil([&]{return accepted>=0;},5000)){qCritical("Resident did not accept the theme");return 2;}
  QTest::qWait(700);
  const QJsonObject row{{"scheme",Theme::name(target)},{"pickToApplyMs",changeStart-picked},{"restyleMs",restyled-changeStart},{"paintMs",painted-restyled},{"pickToPaintedMs",painted-picked},{"pickToResidentAcceptedMs",accepted-picked}};
  picks.append(row);pickToApplied.append(changeStart-picked);restyle.append(restyled-changeStart);paint.append(painted-restyled);pickToPainted.append(painted-picked);pickToAccepted.append(accepted-picked);
 }
 // The restyle alone, without the settings path: Theme::setScheme directly.
 for(int round=0;round<rounds;++round){
  const auto target=Theme::scheme()==Theme::Scheme::Dark?Theme::Scheme::Light:Theme::Scheme::Dark;
  changeStart=restyled=painted=-1;Theme::setScheme(target);QTest::qWait(100);
  direct.append(QJsonObject{{"scheme",Theme::name(target)},{"restyleMs",restyled-changeStart},{"paintMs",painted-restyled}});directRestyle.append(restyled-changeStart);directPaint.append(painted-restyled);
 }
 const QJsonObject result{{"kind","synthetic-theme-switch-timing"},{"desktopAcceptance",false},{"qt",QString::fromLatin1(qVersion())},{"platform",QGuiApplication::platformName()},{"widgets",int(QApplication::allWidgets().size())},{"historyItems",items},{"rounds",rounds},
  {"settingsPick",QJsonObject{{"pickToPaintedMs",summary(pickToPainted)},{"pickToApplyMs",summary(pickToApplied)},{"restyleMs",summary(restyle)},{"paintMs",summary(paint)},{"pickToResidentAcceptedMs",summary(pickToAccepted)},{"rounds",picks}}},
  {"directSwitch",QJsonObject{{"restyleMs",summary(directRestyle)},{"paintMs",summary(directPaint)},{"rounds",direct}}}};
 QSaveFile out(args.at(1));if(!out.open(QIODevice::WriteOnly))return 2;out.write(QJsonDocument(result).toJson(QJsonDocument::Indented));if(!out.commit())return 2;
 std::fprintf(stdout,"%s\n",QJsonDocument(QJsonObject{{"qt",result["qt"]},{"widgets",result["widgets"]},{"pickToPaintedMedian",median(pickToPainted)},{"restyleMedian",median(restyle)},{"paintMedian",median(paint)},{"directRestyleMedian",median(directRestyle)}}).toJson(QJsonDocument::Compact).constData());
 return 0;
}
