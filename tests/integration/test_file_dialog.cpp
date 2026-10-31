// #259: the shared file chooser against a fake org.freedesktop.portal.FileChooser
// on the private session bus of dbus-run-session. No real portal is ever asked.
#include "ui/FileDialog.h"
#include "ui/Theme.h"
#include <QApplication>
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDBusContext>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QFileDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>
#include <QToolButton>
#include <QTreeView>
#include <QWidget>
#include <optional>
#include "fixtures/Waits.h"

using namespace putgd;

namespace {
const QString service=QStringLiteral("org.freedesktop.portal.Desktop");
const QString path=QStringLiteral("/org/freedesktop/portal/desktop");
}

class FakeRequest final:public QObject {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.freedesktop.portal.Request")
public:
 using QObject::QObject;
 int closed=0;
public slots:
 void Close(){++closed;}
};

class FakeChooser final:public QObject,protected QDBusContext {
 Q_OBJECT
 Q_CLASSINFO("D-Bus Interface","org.freedesktop.portal.FileChooser")
 Q_PROPERTY(uint version READ version)
public:
 // Close: Response 2 later, as GTK and GNOME answer Escape or the window's
 // close button (#781). Fail: Response 2 at once, as the portal answers a
 // backend that fails. Error: the call itself errors.
 enum class Answer { Choose, Cancel, Close, Fail, Error, Wait, NoReply };
 explicit FakeChooser(QDBusConnection peer):peer_(peer){}
 uint version()const{return versionValue;}
 uint versionValue=4;
 Answer answer=Answer::Choose;
 QString uri;
 int delayMs=20; // how long the chooser stays open before its Response
 QString method,parent,title,handle;
 QVariantMap options;
 int calls=0;
 FakeRequest* request=nullptr;
 // Sends the Response signal on the last request, as the portal does.
 void respond(uint code,const QVariantMap& results){
  auto signal=QDBusMessage::createSignal(handle,"org.freedesktop.portal.Request","Response");
  signal<<code<<results;peer_.send(signal);
 }
public slots:
 QDBusObjectPath OpenFile(const QString& parentWindow,const QString& dialogTitle,const QVariantMap& dialogOptions){return start("OpenFile",parentWindow,dialogTitle,dialogOptions);}
 QDBusObjectPath SaveFile(const QString& parentWindow,const QString& dialogTitle,const QVariantMap& dialogOptions){return start("SaveFile",parentWindow,dialogTitle,dialogOptions);}
private:
 QDBusObjectPath start(const QString& name,const QString& parentWindow,const QString& dialogTitle,const QVariantMap& dialogOptions){
  ++calls;method=name;parent=parentWindow;title=dialogTitle;options=dialogOptions;
  handle=path+"/request/"+message().service().mid(1).replace('.','_')+"/"+dialogOptions.value("handle_token").toString();
  if(request){peer_.unregisterObject(request->property("path").toString());delete request;}
  request=new FakeRequest(this);request->setProperty("path",handle);
  peer_.registerObject(handle,request,QDBusConnection::ExportAllSlots);
  if(answer==Answer::NoReply){setDelayedReply(true);return {};}
  if(answer==Answer::Error){sendErrorReply("org.freedesktop.portal.Error.Failed","The backend could not open a chooser");return {};}
  const auto code=answer==Answer::Choose?0u:answer==Answer::Cancel?1u:2u;
  if(answer!=Answer::Wait)QTimer::singleShot(answer==Answer::Fail?20:delayMs,this,[this,code]{respond(code,code==0?QVariantMap{{"uris",QStringList{uri}}}:QVariantMap{});});
  return QDBusObjectPath(handle);
 }
 QDBusConnection peer_;
};

class FileDialogTest final:public QObject {
 Q_OBJECT
 QDBusConnection peer_=QDBusConnection::connectToBus(QDBusConnection::SessionBus,"putgd-fake-portal");
 FakeChooser* chooser_=nullptr;
 QWidget* window_=nullptr;
 FileDialog::Result last_;
 void registerPortal(){QVERIFY(peer_.registerService(service));}
 void unregisterPortal(){peer_.unregisterService(service);}
 // Runs one chooser and returns its answer once done is called.
 std::optional<QString> run(FileDialog& dialog,const FileDialog::Options& options,int timeout=3000){
  std::optional<QString> result;last_={};
  if(!dialog.open(options,[&](const FileDialog::Result& r){result=r.path;last_=r;}))return std::nullopt;
  (void)putgd::test::waitFor([&]{return result||dialog.fallbackDialog();},timeout);
  return result;
 }
 static QFileDialog* waitForFallback(FileDialog& dialog){
  (void)putgd::test::waitFor([&]{return dialog.fallbackDialog()&&dialog.fallbackDialog()->isVisible();},4000);
  return dialog.fallbackDialog();
 }
private slots:
 void initTestCase(){
  QVERIFY2(QDBusConnection::sessionBus().isConnected(),"Run under dbus-run-session");
  // The bus must not be able to start the desktop's real portal for the fallback cases.
  const auto activatable=QDBusConnection::sessionBus().interface()->activatableServiceNames().value();
  QVERIFY2(!activatable.contains(service),"Run under dbus-run-session --config-file=tests/fixtures/dbus-session-isolated.conf");
  QVERIFY(peer_.isConnected());
  portal::registerTypes();
  chooser_=new FakeChooser(peer_);
  QVERIFY(peer_.registerObject(path,chooser_,QDBusConnection::ExportAllSlots|QDBusConnection::ExportAllProperties));
  window_=new QWidget;window_->resize(400,300);window_->show();
  FileDialog::setPortalCallTimeoutMs(400);
 }
 void cleanupTestCase(){delete window_;unregisterPortal();}
 void init(){qunsetenv("PUTGD_FILE_DIALOG");chooser_->answer=FakeChooser::Answer::Choose;chooser_->versionValue=4;chooser_->calls=0;chooser_->delayMs=20;chooser_->uri.clear();FileDialog::setPortalFailureWindowMs(250);registerPortal();}
 void cleanup(){for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QFileDialog*>(widget))dialog->reject();QApplication::processEvents();}

 void saveAsksThePortalAndReturnsTheLocalPath(){
  chooser_->uri="file:///tmp/put.gd%20test/clip%C3%A9.webm";
  FileDialog dialog(window_);QSignalSpy changed(&dialog,&FileDialog::stateChanged);
  const auto chosen=run(dialog,{FileDialog::Mode::Save,"Save a copy","/tmp/start dir","recording.webm",{{"WEBM",{"*.webm"}}}});
  QVERIFY(chosen.has_value());
  QCOMPARE(*chosen,QString::fromUtf8("/tmp/put.gd test/clipé.webm"));
  QVERIFY(last_.chosen());QCOMPARE(last_.path,*chosen);
  QCOMPARE(chooser_->method,QString("SaveFile"));QCOMPARE(chooser_->title,QString("Save a copy"));
  // Offscreen has no X11 window: no parent handle.
  QCOMPARE(chooser_->parent,QString());
  QCOMPARE(chooser_->options["current_name"].toString(),QString("recording.webm"));
  QCOMPARE(chooser_->options["current_folder"].toByteArray(),QByteArray("/tmp/start dir\0",15));
  QVERIFY(chooser_->options["modal"].toBool());
  QVERIFY(chooser_->handle.endsWith("/"+chooser_->options["handle_token"].toString()));
  QList<portal::Filter> filters;chooser_->options["filters"].value<QDBusArgument>()>>filters;
  QCOMPARE(filters.size(),1);QCOMPARE(filters[0].label,QString("WEBM"));QCOMPARE(filters[0].rules.size(),1);
  QCOMPARE(filters[0].rules[0].type,0u);QCOMPARE(filters[0].rules[0].pattern,QString("*.webm"));
  const auto state=dialog.state();
  QVERIFY(!state["open"].toBool());
  QCOMPARE(state["last"].toObject()["status"].toString(),QString("chosen"));
  QCOMPARE(state["last"].toObject()["backend"].toString(),QString("portal"));
  QCOMPARE(state["last"].toObject()["path"].toString(),*chosen);
  QVERIFY(changed.size()>=2);
 }
 void openAndFolderModes(){
  chooser_->uri="file:///tmp/picture.png";
  FileDialog dialog(window_);
  QCOMPARE(run(dialog,{FileDialog::Mode::Open,"Edit an image file","/tmp",{},{{"Images",{"*.png","*.jpg"}}}}).value_or("none"),QString("/tmp/picture.png"));
  QCOMPARE(chooser_->method,QString("OpenFile"));QVERIFY(!chooser_->options.contains("directory"));QVERIFY(!chooser_->options.contains("current_name"));
  chooser_->uri="file:///tmp/folder";
  QCOMPARE(run(dialog,{FileDialog::Mode::Folder,"Choose output folder","/tmp",{},{}}).value_or("none"),QString("/tmp/folder"));
  QCOMPARE(chooser_->method,QString("OpenFile"));QVERIFY(chooser_->options["directory"].toBool());
 }
 void cancelInThePortalIsACancel(){
  chooser_->answer=FakeChooser::Answer::Cancel;
  FileDialog dialog(window_);
  QCOMPARE(run(dialog,{FileDialog::Mode::Save,"Save a copy",{},"a.png",{}}).value_or("none"),QString());
  QVERIFY(!dialog.fallbackDialog());
  QCOMPARE(dialog.state()["last"].toObject()["status"].toString(),QString("cancelled"));
  QCOMPARE(last_.status,FileDialog::Result::Status::Cancelled);QVERIFY(last_.error.isEmpty());
 }
 // #781: Escape in the GTK chooser (Cinnamon, GNOME) answers Response 2, not 1.
 void closingThePortalChooserIsACancel_data(){QTest::addColumn<int>("mode");for(const auto mode:{FileDialog::Mode::Open,FileDialog::Mode::Save,FileDialog::Mode::Folder})QTest::newRow(qPrintable(QString::number(int(mode))))<<int(mode);}
 void closingThePortalChooserIsACancel(){
  QFETCH(int,mode);
  // Well past the default failure window, as a person's Escape is.
  chooser_->answer=FakeChooser::Answer::Close;chooser_->delayMs=1500;
  FileDialog dialog(window_);bool fellBack=false;
  connect(&dialog,&FileDialog::stateChanged,this,[&]{fellBack=fellBack||dialog.fallbackDialog()||dialog.state()["backend"].toString()=="fallback";});
  QCOMPARE(run(dialog,{FileDialog::Mode(mode),"Pick",{},{},{}},5000).value_or("none"),QString());
  QVERIFY(!fellBack);QVERIFY(!dialog.fallbackDialog());QVERIFY(!dialog.isOpen());
  QCOMPARE(last_.status,FileDialog::Result::Status::Cancelled);QVERIFY(last_.path.isEmpty());QVERIFY(last_.error.isEmpty());
  const auto last=dialog.state()["last"].toObject();
  QCOMPARE(last["status"].toString(),QString("cancelled"));QCOMPARE(last["backend"].toString(),QString("portal"));QVERIFY(last["fallbackReason"].isNull());
  PUTGD_TRY_VERIFY(!QApplication::activeModalWidget());
  // Nothing opens afterwards either.
  const auto noChooser=[&]{
   if(dialog.fallbackDialog())return false;
   for(auto* widget:QApplication::topLevelWidgets())if(qobject_cast<QFileDialog*>(widget)&&widget->isVisible())return false;
   return true;
  };
  PUTGD_VERIFY_STEADY(!fellBack&&noChooser(),200);
 }
 void whileOpenTheWindowIsBlockedAndCancelClosesTheRequest(){
  chooser_->answer=FakeChooser::Answer::Wait;
  FileDialog dialog(window_);bool answered=false;QString answer="unset";
  QVERIFY(dialog.open({FileDialog::Mode::Save,"Save a copy",{},"a.png",{}},[&](const FileDialog::Result& r){answered=true;answer=r.path;}));
  QVERIFY(!dialog.open({FileDialog::Mode::Save,"Again",{},{},{}},[](const FileDialog::Result&){}));
  PUTGD_TRY_COMPARE(chooser_->calls,1);
  // Modal like QFileDialog::exec, with nothing drawn by Qt.
  QVERIFY(QApplication::activeModalWidget());QVERIFY(!dialog.fallbackDialog());
  const auto state=dialog.state();QVERIFY(state["open"].toBool());QCOMPARE(state["backend"].toString(),QString("portal"));QCOMPARE(state["mode"].toString(),QString("save"));
  putgd::test::settle(100);PUTGD_TRY_VERIFY(!answered);
  dialog.cancel();
  QVERIFY(answered);QCOMPARE(answer,QString());
  PUTGD_TRY_COMPARE(chooser_->request->closed,1);
  PUTGD_TRY_VERIFY(!QApplication::activeModalWidget());
  // A late Response after the cancel changes nothing.
  chooser_->respond(0,{{"uris",QStringList{"file:///tmp/late.png"}}});putgd::test::settle(50);
  PUTGD_TRY_COMPARE(dialog.state()["last"].toObject()["status"].toString(),QString("cancelled"));
 }
 void destroyingTheOwnerClosesTheRequest(){
  chooser_->answer=FakeChooser::Answer::Wait;
  auto* dialog=new FileDialog(window_);bool answered=false;
  QVERIFY(dialog->open({FileDialog::Mode::Open,"Edit",{},{},{}},[&](const FileDialog::Result&){answered=true;}));
  PUTGD_TRY_COMPARE(chooser_->calls,1);
  delete dialog;
  PUTGD_TRY_COMPARE(chooser_->request->closed,1);QVERIFY(!answered);
 }
 // A 2 before any chooser could have been seen is the portal failing.
 void portalFailureFallsBack(){
  chooser_->answer=FakeChooser::Answer::Fail;FileDialog::setPortalFailureWindowMs(5000);
  FileDialog dialog(window_);bool answered=false;
  QVERIFY(dialog.open({FileDialog::Mode::Save,"Save a copy",{},"a.png",{}},[&](const FileDialog::Result&){answered=true;}));
  auto* fallback=waitForFallback(dialog);QVERIFY(fallback);QVERIFY(!answered);
  QCOMPARE(dialog.state()["backend"].toString(),QString("fallback"));QCOMPARE(dialog.state()["fallbackReason"].toString(),QString("portal_failed"));
  QCOMPARE(fallback->acceptMode(),QFileDialog::AcceptSave);
  fallback->reject();QVERIFY(answered);
  QCOMPARE(dialog.state()["last"].toObject()["backend"].toString(),QString("fallback"));
 }
 void portalErrorFallsBack(){
  chooser_->answer=FakeChooser::Answer::Error;
  FileDialog dialog(window_);bool answered=false;
  QVERIFY(dialog.open({FileDialog::Mode::Open,"Edit",{},{},{}},[&](const FileDialog::Result&){answered=true;}));
  auto* fallback=waitForFallback(dialog);QVERIFY(fallback);QVERIFY(!answered);
  QCOMPARE(dialog.state()["fallbackReason"].toString(),QString("org.freedesktop.portal.Error.Failed"));
  QCOMPARE(fallback->fileMode(),QFileDialog::ExistingFile);
  fallback->reject();QVERIFY(answered);
 }
 void noPortalFallsBack(){
  unregisterPortal();
  FileDialog dialog(window_);
  QVERIFY(dialog.open({FileDialog::Mode::Open,"Edit",{},{},{}},[](const FileDialog::Result&){}));
  QVERIFY(waitForFallback(dialog));
  QVERIFY2(dialog.state()["fallbackReason"].toString().startsWith("org.freedesktop.DBus.Error."),qPrintable(dialog.state()["fallbackReason"].toString()));
  QCOMPARE(chooser_->calls,0);
 }
 void unansweredCallFallsBackAndClosesTheRequest(){
  chooser_->answer=FakeChooser::Answer::NoReply;
  FileDialog dialog(window_);
  QVERIFY(dialog.open({FileDialog::Mode::Open,"Edit",{},{},{}},[](const FileDialog::Result&){}));
  QVERIFY(waitForFallback(dialog));
  QCOMPARE(dialog.state()["fallbackReason"].toString(),QString("org.freedesktop.DBus.Error.NoReply"));
  PUTGD_TRY_COMPARE(chooser_->request->closed,1);
 }
 void oldPortalFallsBackForFolders(){
  chooser_->versionValue=2;
  FileDialog dialog(window_);
  QVERIFY(dialog.open({FileDialog::Mode::Folder,"Choose output folder",{},{},{}},[](const FileDialog::Result&){}));
  auto* fallback=waitForFallback(dialog);QVERIFY(fallback);
  QCOMPARE(dialog.state()["fallbackReason"].toString(),QString("portal_version"));QCOMPARE(chooser_->calls,0);
  QCOMPARE(fallback->fileMode(),QFileDialog::Directory);
 }
 void nonLocalChoiceFails(){
  chooser_->uri="https://example.invalid/file.png";
  FileDialog dialog(window_);
  QCOMPARE(run(dialog,{FileDialog::Mode::Open,"Edit",{},{},{}}).value_or("none"),QString());
  QCOMPARE(dialog.state()["last"].toObject()["status"].toString(),QString("failed"));
  // The caller learns it failed, and why; it is not a cancel.
  QVERIFY(last_.failed());QVERIFY(!last_.chosen());QVERIFY(last_.path.isEmpty());
  QVERIFY(!last_.error.isEmpty());QCOMPARE(dialog.state()["last"].toObject()["error"].toString(),last_.error);
 }
 void environmentSkipsThePortal(){
  qputenv("PUTGD_FILE_DIALOG","fallback");
  QTemporaryDir folder;QVERIFY(folder.isValid());
  FileDialog dialog(window_);QString answer="unset";
  QVERIFY(dialog.open({FileDialog::Mode::Save,"Save a copy",folder.path(),"copy.png",{{"PNG image",{"*.png"}}}},[&](const FileDialog::Result& r){answer=r.path;}));
  auto* fallback=waitForFallback(dialog);QVERIFY(fallback);QCOMPARE(chooser_->calls,0);
  QCOMPARE(dialog.state()["fallbackReason"].toString(),QString("disabled"));
  // Qt's widget dialog, never a platform theme's native one that may not map (#863).
  QVERIFY(fallback->testOption(QFileDialog::DontUseNativeDialog));QVERIFY(fallback->findChild<QLineEdit*>("fileNameEdit"));
  QCOMPARE(fallback->windowTitle(),QString("Save a copy"));QCOMPARE(fallback->nameFilters(),QStringList{"PNG image (*.png)"});
  // selectFile leaves a focused name field alone; type the name as a user does.
  auto* field=fallback->findChild<QLineEdit*>("fileNameEdit");QVERIFY(field);field->setText("chosen.png");QMetaObject::invokeMethod(fallback,"accept");
  QCOMPARE(answer,folder.filePath("chosen.png"));
  QCOMPARE(dialog.state()["last"].toObject()["status"].toString(),QString("chosen"));
 }
 // The fallback's labels, list and fields in the scheme's colours, both schemes.
 void fallbackIsThemed_data(){QTest::addColumn<int>("scheme");QTest::newRow("dark")<<int(Theme::Scheme::Dark);QTest::newRow("light")<<int(Theme::Scheme::Light);}
 void fallbackIsThemed(){
  QFETCH(int,scheme);Theme::setScheme(Theme::Scheme(scheme));
  qputenv("PUTGD_FILE_DIALOG","fallback");
  QTemporaryDir folder;QVERIFY(folder.isValid());for(const auto* name:{"a.png","b.png"}){QFile file(folder.filePath(name));QVERIFY(file.open(QIODevice::WriteOnly));}
  FileDialog dialog(window_);
  QVERIFY(dialog.open({FileDialog::Mode::Save,"Save a copy",folder.path(),"copy.png",{{"PNG image",{"*.png"}}}},[](const FileDialog::Result&){}));
  auto* fallback=waitForFallback(dialog);QVERIFY(fallback);
  QCOMPARE(fallback->property("themeScheme").toString(),Theme::name(Theme::Scheme(scheme)));
  const auto& p=Theme::palette(Theme::Scheme(scheme));
  const auto labels=fallback->findChildren<QLabel*>();QVERIFY(!labels.isEmpty());
  for(auto* label:labels){
   if(!label->isVisible())continue;
   const auto text=label->palette().color(QPalette::WindowText);
   QVERIFY2(Theme::contrast(text,p.bg)>=4.5,qPrintable(label->objectName()+": "+text.name()+" on "+p.bg.name()));
  }
  QAbstractItemView* list=fallback->findChild<QListView*>("listView");QVERIFY(list);
  if(!list->isVisible())list=fallback->findChild<QTreeView*>("treeView");
  QVERIFY(list&&list->isVisible());
  PUTGD_TRY_VERIFY(list->model()&&list->model()->rowCount(list->rootIndex())>=2);
  // The list draws on the scheme's surface, not Fusion's white.
  const auto image=fallback->grab().toImage();
  const auto inList=list->viewport()->mapTo(fallback,QPoint(list->viewport()->width()-6,list->viewport()->height()-6));
  const QColor drawn=image.pixelColor(inList*image.devicePixelRatio());
  QVERIFY2(qAbs(drawn.lightness()-p.surface.lightness())<=12,qPrintable(drawn.name()+" vs "+p.surface.name()));
  auto* name=fallback->findChild<QLineEdit*>("fileNameEdit");QVERIFY(name);
  QVERIFY(Theme::contrast(name->palette().color(QPalette::Text),p.surfaceRaised)>=4.5);
  for(const auto* button:{"backButton","toParentButton","newFolderButton"})QVERIFY2(fallback->findChild<QToolButton*>(button)&&!fallback->findChild<QToolButton*>(button)->icon().isNull(),button);
  // A scheme switch while it is open restyles it.
  const auto other=Theme::Scheme(scheme)==Theme::Scheme::Dark?Theme::Scheme::Light:Theme::Scheme::Dark;
  Theme::setScheme(other);
  QCOMPARE(fallback->property("themeScheme").toString(),Theme::name(other));
  QCOMPARE(fallback->palette().color(QPalette::Window),Theme::palette(other).bg);
  fallback->reject();
 }
 void parentWindowOnX11(){
  if(QGuiApplication::platformName()!="xcb")QSKIP("Runs as file_dialog_x11_parent under Xvfb");
  QVERIFY(window_->winId());
  QCOMPARE(FileDialog::parentWindow(window_),QStringLiteral("x11:%1").arg(quint64(window_->winId()),0,16));
  auto* child=new QLabel(window_);QCOMPARE(FileDialog::parentWindow(child),FileDialog::parentWindow(window_));
  chooser_->uri="file:///tmp/x.png";
  FileDialog dialog(child);
  QCOMPARE(run(dialog,{FileDialog::Mode::Open,"Edit",{},{},{}}).value_or("none"),QString("/tmp/x.png"));
  QCOMPARE(chooser_->parent,FileDialog::parentWindow(window_));
 }
};

int main(int argc,char** argv){QApplication app(argc,argv);FileDialogTest test;return QTest::qExec(&test,argc,argv);}
#include "test_file_dialog.moc"
