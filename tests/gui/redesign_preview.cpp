// Synthetic visual verification only. This does not establish desktop support.
#include "fixtures/UiSessionFixture.h"
#include "ui/MainWindow.h"
#include "ui/HistoryWindow.h"
#include "ui/HistoryModel.h"
#include "ui/SettingsWindow.h"
#include "ui/RedesignTheme.h"
#include "ui/VisualStyle.h"
#include "ui/EditorWindow.h"
#include "editor/EditorCommands.h"
#include <QApplication>
#include <QComboBox>
#include <QCheckBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QJsonDocument>
#include <QListView>
#include <QListWidget>
#include <QPainter>
#include <QSaveFile>
#include <QScrollArea>
#include <QScrollBar>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTabWidget>
#include <QToolButton>
#include <QElapsedTimer>
#include "fixtures/Waits.h"
#include "ui/Theme.h"
#include <QFile>
#include <QScreen>
#include <QWindow>
#ifdef PUTGD_PREVIEW_REVIEW
#include "record/ReviewWindow.h"
#include "record/Overlays.h"
#endif
using namespace putgd;
using namespace putgd::test;
namespace {
// Native capture mode (Graphite acceptance, 2026-10-26): with
// PUTGD_PREVIEW_HANDSHAKE=DIR the tool runs on the real platform under an
// owned magenta backdrop, publishes each state's window and waits for the
// workstation's RFB capture driver to write DIR/<state>.done. Still synthetic
// content; the frames are real compositor pixels of these windows.
struct Native {
 QString dir=qEnvironmentVariable("PUTGD_PREVIEW_HANDSHAKE");QWidget* backdrop=nullptr;
 bool active()const{return !dir.isEmpty();}
 void cover(){
  if(!active()||qEnvironmentVariable("PUTGD_PREVIEW_BACKDROP")!="1")return;
  backdrop=new QWidget;backdrop->setObjectName("putgd-preview-backdrop");backdrop->setWindowTitle("put.gd preview backdrop");backdrop->setWindowFlag(Qt::FramelessWindowHint);
  QPalette palette;palette.setColor(QPalette::Window,QColor(255,0,255));backdrop->setPalette(palette);backdrop->setAutoFillBackground(true);backdrop->showFullScreen();
  putgd::test::settle(800);
 }
 static QJsonObject geometry(QWidget& w){const auto f=w.frameGeometry();return {{"x",f.x()},{"y",f.y()},{"width",f.width()},{"height",f.height()},{"maximized",w.isMaximized()},{"visible",w.isVisible()},{"platform",QGuiApplication::platformName()}};}
 bool step(QWidget& w,const QString& name,int timeoutMs=45000){
  if(!active())return true;
  w.raise();w.activateWindow();putgd::test::settle(300);
  QSaveFile ready(dir+"/"+name+".ready");if(!ready.open(QIODevice::WriteOnly))return false;ready.write(QJsonDocument(geometry(w)).toJson(QJsonDocument::Compact));if(!ready.commit())return false;
  return putgd::test::waitFor([&]{return QFile::exists(dir+"/"+name+".done");},timeoutMs);
 }
 // The window's geometry, rewritten whenever it moves, resizes or changes state,
 // while the driver moves, maximizes, resizes and closes it with real input.
 bool chrome(QWidget& w,const QString& name){
  if(!active())return true;
  class Watch final:public QObject {public:QString path;QWidget* w;bool eventFilter(QObject*,QEvent* e)override{if(e->type()==QEvent::Move||e->type()==QEvent::Resize||e->type()==QEvent::WindowStateChange||e->type()==QEvent::Close||e->type()==QEvent::Hide){QSaveFile f(path);if(f.open(QIODevice::WriteOnly)){auto g=geometry(*w);g["event"]=int(e->type());f.write(QJsonDocument(g).toJson(QJsonDocument::Compact));f.commit();}}return false;}};
  Watch watch;watch.path=dir+"/"+name+".geometry";watch.w=&w;w.installEventFilter(&watch);
  const bool done=step(w,name,180000);w.removeEventFilter(&watch);return done;
 }
};
bool waitUntil(const std::function<bool()>& condition){return putgd::test::waitFor(condition,8000);}
QImage iconSpecimen(){
 QImage image(1200,460,QImage::Format_RGB32);image.fill(QColor("#101216"));QPainter p(&image);
 p.setPen(QColor("#e7e9ef"));p.drawText(24,28,"Lift + Duotone · production renderer · synthetic specimen");
 VisualStyle::icon("brand").paint(&p,QRect(24,57,72,72));
 const QStringList names{"region","window","desktop","clipboard","history","destinations","preferences","info","shortcuts","files"};
 for(int i=0;i<names.size();++i){const int x=135+i*105;VisualStyle::icon(names[i]).paint(&p,QRect(x,60,48,48));p.drawText(QRect(x-15,120,92,24),Qt::AlignCenter,names[i]);}
 p.drawText(24,185,"Green fill");
 for(int i=0;i<5;++i){const int x=155+i*125;VisualStyle::icon("tray",QColor("#62d69b"),i*25).paint(&p,QRect(x,170,72,72));p.drawText(QRect(x,250,72,24),Qt::AlignCenter,QString::number(i*25)+"%");}
 for(int i=0;i<2;++i){const int x=860+i*130;VisualStyle::icon("tray",QColor(i?"#9bbcff":"#ffb4ab")).paint(&p,QRect(x,170,72,72));p.drawText(QRect(x-10,250,92,24),Qt::AlignCenter,i?"Blink off":"Blink on");}
 p.drawText(24,308,"Actual tray sizes: 16 / 22 / 24 / 32 px");
 for(int row=0;row<2;++row){const int y=330+row*56;p.fillRect(QRect(24,y,1152,48),QColor(row?"#e4e5e9":"#20242d"));
  int x=44;for(int size:{16,22,24,32}){VisualStyle::icon("tray").paint(&p,QRect(x,y+(48-size)/2,size,size));x+=60;}
  for(int i=0;i<5;++i)VisualStyle::icon("tray",QColor("#62d69b"),i*25).paint(&p,QRect(300+i*130,y+12,24,24));
  for(int i=0;i<2;++i)VisualStyle::icon("tray",QColor(i?"#9bbcff":"#ffb4ab")).paint(&p,QRect(980+i*80,y+12,24,24));
 }
 return image;
}
QImage editorIcons(){
 QImage image(1060,120,QImage::Format_RGB32);image.fill(QColor("#101216"));QPainter p(&image);p.setPen(QColor("#e7e9ef"));
 const QStringList names{"edit","select","redact","blur","crop","annotate-arrow","rectangle","highlight","text","undo","redo","zoom-fit","zoom-actual","save"};
 for(int i=0;i<names.size();++i){const int x=20+i*74;VisualStyle::icon(names[i]).paint(&p,QRect(x+13,18,40,40));p.drawText(QRect(x-6,68,86,24),Qt::AlignCenter,names[i]);}
 return image;
}
QImage syntheticCapture(int index){
 using namespace RedesignTheme;QImage image(960,600,QImage::Format_RGB32);image.fill(bg());QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(surface());p.drawRoundedRect(QRect(24,20,912,560),16,16);p.setBrush(border());p.drawRoundedRect(QRect(24,20,912,45),16,16);for(int i=0;i<3;++i){p.setBrush(i==0?danger():i==1?warning():success());p.drawEllipse(QPoint(47+20*i,43),5,5);}QFont f(RedesignTheme::uiFontFamily());f.setPixelSize(24);f.setBold(true);p.setFont(f);p.setPen(text());const QStringList titles{"Design review","Build summary","Project notes","Release checklist","Weekly highlights","Product research","Interface library","Upload overview"};p.drawText(QRect(61,91,830,44),titles.value(index%titles.size()));f.setBold(false);f.setPixelSize(14);p.setFont(f);p.setPen(textMuted());p.drawText(QRect(61,140,830,25),"Synthetic sample content for the put.gd redesign experiment");
 for(int col=0;col<3;++col){const QRect card(61+col*276,194,256,130);p.setPen(QPen(borderStrong()));p.setBrush(surfaceRaised());p.drawRoundedRect(card,10,10);p.setPen(textMuted());p.drawText(card.adjusted(18,16,-18,-80),QStringList{"CAPTURES","UPLOADED","SAVED LOCALLY"}.at(col));f.setPixelSize(31);f.setBold(true);p.setFont(f);p.setPen(col==1?success():accent());p.drawText(card.adjusted(18,53,-18,-16),QString::number((index+1)*7+col*3));f.setPixelSize(14);f.setBold(false);p.setFont(f);}
 p.setPen(textSecondary());p.drawText(QRect(61,355,800,24),"Latest activity");for(int row=0;row<4;++row){int y=402+row*34;p.setPen(QPen(border()));p.drawLine(61,y+22,888,y+22);p.setPen(row==0?accent():textSecondary());p.drawText(QRect(68,y-2,580,24),QStringList{"Review capture ready to share","Notes and reference files","Updated component library","Final checks complete"}.at((row+index)%4));p.setPen(textMuted());p.drawText(QRect(750,y-2,130,24),Qt::AlignRight,"Just now");}return image;
}
QJsonObject previewConfig(const QString& root){
 auto doc=Settings::developmentTemplate();auto s3=doc["profiles"].toArray().first().toObject();s3["id"]="synthetic-archive";s3["label"]="Project archive";auto s=s3["settings"].toObject();s["bucket"]="design-captures";s["region"]="us-east-1";s["endpoint"]="https://s3.us-east-1.amazonaws.com";s["objectPrefix"]="screenshots/";s["aclMode"]="omit";s["publicUrlBase"]="https://captures.example.invalid";s3["settings"]=s;s3["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};
 QJsonObject proton{{"id","synthetic-proton"},{"label","Proton Drive"},{"provider","proton-drive"},{"settings",QJsonObject{{"accountRootUid","synthetic-root"},{"folderUid","synthetic-folder"},{"folderName","put.gd"},{"shareByDefault",true},{"linkExpirationDays",0},{"sharePasswordReference",QJsonValue::Null},{"requestTimeoutMs",120000}}},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}};doc["profiles"]=QJsonArray{proton,s3};auto actions=doc["actions"].toObject();int index=1;for(const auto& name:Settings::actionNames()){auto a=actions[name].toObject();a["profileId"]="synthetic-proton";a["shortcut"]="Ctrl+"+QString::number(index++);actions[name]=a;}doc["actions"]=actions;auto archive=doc["archive"].toObject();archive["root"]=root+"/local-copies";archive["keepUploads"]=true;doc["archive"]=archive;auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;return doc;
}
}
int main(int argc,char** argv){
 QApplication app(argc,argv);app.setApplicationName("putgd-redesign-preview");app.setWindowIcon(VisualStyle::icon("brand"));Native native;if(qEnvironmentVariable("PUTGD_PREVIEW_THEME")=="light")Theme::setScheme(Theme::Scheme::Light);if(app.arguments().size()!=2){qCritical("Usage: redesign_preview OUTPUT_DIRECTORY");return 2;}const auto output=QFileInfo(app.arguments().at(1)).absoluteFilePath();if(!QDir().mkpath(output)){qCritical("Cannot create preview output directory");return 2;}
 if(!iconSpecimen().save(QDir(output).filePath("00-icon-specimen.png")))return 2;
 // The fixture calls the resident dispatcher directly and never opens a socket,
 // but enforce the same Linux path precondition before constructing the app.
 const auto socket=QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).filePath("putgd/session.sock");if(socket.toUtf8().size()>107){qCritical("Resolved Unix socket exceeds 107 UTF-8 bytes");return 2;}
 QTemporaryDir data("/tmp/pgr-XXXXXX");if(!data.isValid())return 2;const auto config=previewConfig(data.path());SettingsService settings(data.filePath("synthetic-config.json"),[](const QString&,const QJsonObject&){return Error{};});if(!settings.save(config,{})){qCritical("Cannot write synthetic preview configuration");return 2;}
 // No production provider or capture action is used. Credentials below are
 // synthetic and stored only in the fixture session, never the OS keychain.
 ProviderRegistry registry;Source source;OperationService operations(data.filePath("data"),settings,registry,source);if(!waitUntil([&]{return operations.ready();})){qCritical("Synthetic resident did not initialize");return 2;}
 const auto now=QDateTime::currentDateTimeUtc();{HistoryStore ledger(operations.databasePath());for(int i=0;i<8;++i){const auto path=data.filePath(QString("synthetic-preview-%1.png").arg(i));if(!syntheticCapture(i).save(path))return 2;Operation op;op.id=QString("synthetic-preview-operation-%1").arg(i);op.action=i==5?"window":i==6?"clipboard":i==7?"desktop":"region";op.created=now.addSecs(i==7?-86400:-110-i*18).toString(Qt::ISODateWithMs);op.state=i==3?"partial":"completed";op.copyStatus=i==0?"copied":"not_requested";op.settings=settings.snapshot();op.destination.providerId="proton-drive";op.destination.profileId="synthetic-proton";op.destination.profileRevision="synthetic-revision";op.destination.settings=config["profiles"].toArray().first().toObject()["settings"].toObject();OperationItem item;item.artifact.id=QString("synthetic-preview-file-%1").arg(i);item.artifact.name=i==1?"Quarterly-release-readiness-review-with-accessibility-and-localisation-notes-final-v12.png":i==5?"Window — Design review.png":i==6?"Clipboard image.png":i==7?"Desktop.png":"Screenshot "+now.addSecs(-110-i*18).toLocalTime().toString("HH:mm:ss")+".png";item.artifact.mime="image/png";item.artifact.category="images";item.artifact.bytes=QFileInfo(path).size();item.artifact.previewPath=path;item.local.status="saved";item.local.path=path;item.uploadStatus=i==3?"failed":"uploaded";item.remoteStatus="present";item.shareStatus="shared";item.receipt.destination=op.destination;item.receipt.publicationId="synthetic-publication";item.receipt.objectReference=QJsonObject{{"fileUid","synthetic-file"}};item.receipt.directUrl="https://drive.proton.me/urls/synthetic-preview#example";if(i==3)item.error={"synthetic_network","Connection interrupted. Try again.",{}};op.items.append(item);if(ledger.saveOperation(op))return 2;}
  Operation cancelled;cancelled.id="synthetic-cancelled";cancelled.action="region";cancelled.created=now.addSecs(-500).toString(Qt::ISODateWithMs);cancelled.state="cancelled";cancelled.cancelled=true;cancelled.settings=settings.snapshot();if(ledger.saveOperation(cancelled))return 2;
 }
 HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);JsonReply loadFolders;fixture.setup.setProviderSetup([&](const QString&,const Destination&,const QJsonObject&,JsonReply reply,auto){loadFolders=std::move(reply);});fixture.server.setUiDiagnostics([]{return QJsonObject{{"capabilities",QJsonObject{{"providers",QJsonObject{{"proton-drive",QJsonObject{}}}}}}};});fixture.session.refreshSnapshot();if(!waitUntil([&]{return fixture.session.diagnostics()["capabilities"].toObject()["providers"].toObject().contains("proton-drive");}))return 2;native.cover();MainWindow window(fixture.session);window.resize(1440,960);window.statusBar()->addPermanentWidget(new QLabel("Synthetic preview"));window.show();auto* list=window.findChild<QListView*>("historyList");if(!list||!waitUntil([&]{return list->model()->rowCount()==8;})){qCritical("Synthetic history did not load");return 2;}list->selectionModel()->select(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(0,0));
 QJsonArray captures;auto save=[&](const QString& name){putgd::test::settle(450);app.processEvents();if(name=="01-history-list"||name=="10-history-compact"){auto* selectedPreview=window.findChild<QLabel*>("historyPreview");if(!selectedPreview||!waitUntil([&]{return !selectedPreview->pixmap().isNull();}))return false;}const auto image=window.grab();const auto path=QDir(output).filePath(name+".png");if(!image.save(path))return false;if(!native.step(window,name))return false;captures.append(QJsonObject{{"file",name+".png"},{"width",image.width()},{"height",image.height()}});return true;};
 auto* preview=window.findChild<QLabel*>("historyPreview");if(!preview||!waitUntil([&]{return !preview->pixmap().isNull();}))return 2;
 if(!save("01-history-list"))return 2;
 window.findChild<QToolButton*>("historyGridMode")->click();if(!save("02-history-gallery"))return 2;window.findChild<QToolButton*>("historyListMode")->click();window.navigate("destinations");auto* destinations=window.findChild<QListWidget*>("destinationList");if(!destinations)return 2;destinations->setCurrentRow(0);if(!waitUntil([&]{return bool(loadFolders);}))return 2;loadFolders({},{{"accountRootUid","synthetic-root"},{"accountEmail","designer@example.invalid"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","put.gd"}}}}});if(!waitUntil([&]{return window.findChild<QLabel*>("protonAccountStatus")->text().contains("designer@example.invalid");}))return 2;if(!save("03-destinations-proton"))return 2;window.findChild<QPushButton*>("inspectProtonAccount")->click();if(!waitUntil([&]{return window.findChild<QLabel*>("protonFolderStatus")->isVisible();})||!save("16-proton-loading-folders"))return 2;loadFolders({},{{"accountRootUid","synthetic-root"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","put.gd"}}}}});if(!waitUntil([&]{return !window.findChild<QLabel*>("protonFolderStatus")->isVisible();}))return 2;destinations->setCurrentRow(1);if(!save("04-destinations-s3"))return 2;
 window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");if(!tabs)return 2;tabs->setCurrentIndex(3);if(!save("05-settings-advanced"))return 2;tabs->setCurrentIndex(2);if(!save("06-settings-shortcuts"))return 2;tabs->setCurrentIndex(0);if(!save("07-settings-general"))return 2;tabs->setCurrentIndex(1);if(!save("08-settings-capture"))return 2;
 window.navigate("destinations");destinations->setCurrentRow(1);window.findChild<QLineEdit*>("credentialAccessKey")->setText("EXAM-synthetic-preview-access");window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-preview-secret");window.findChild<QPushButton*>("storeCredentials")->click();if(!waitUntil([&]{return window.findChild<QLabel*>("storedCredentialHint")->text().contains("EXAM");}))return 2;auto* destinationScroll=window.findChild<QScrollArea*>("destinationScroll");destinationScroll->verticalScrollBar()->setValue(destinationScroll->verticalScrollBar()->maximum());if(!save("09-stored-credential-hint"))return 2;
 window.resize(1060,680);window.navigate("history");if(!save("10-history-compact")||window.width()!=1060||window.height()!=680)return 2;window.navigate("settings");tabs->setCurrentIndex(3);if(!save("11-advanced-compact"))return 2;window.navigate("destinations");destinations->setCurrentRow(0);destinationScroll->verticalScrollBar()->setValue(0);if(!save("12-proton-compact"))return 2;
 window.navigate("settings");tabs->setCurrentIndex(1);if(!save("13-capture-compact"))return 2;window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);if(!save("14-capture-exceptions-compact"))return 2;
 window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(false);auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");destination->setCurrentIndex(destination->findData(QString{}));window.findChild<QCheckBox*>("keepUploadCopies")->setChecked(false);if(!waitUntil([&]{return !settings.snapshot().archive.keepUploads&&settings.snapshot().actions["region"].destination=="local";}))return 2;if(!save("15-capture-no-output"))return 2;
 // Editor: the same widgets the --edit-image helper shows, over a synthetic capture.
 if(!editorIcons().save(QDir(output).filePath("00-editor-icons.png")))return 2;
 {
  editor::EditDocument doc(syntheticCapture(1));QJsonArray created;QString code,message;
  const QJsonArray edits{
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{61,194,256,40}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","blur"},{"rect",QJsonArray{337,194,256,130}},{"strength",18}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","arrow"},{"from",QJsonArray{820,520}},{"to",QJsonArray{700,330}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","rectangle"},{"rect",QJsonArray{605,186,272,146}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","highlight"},{"rect",QJsonArray{61,348,180,28}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","text"},{"at",QJsonArray{620,530}},{"text","New total"}}}}};
  if(!editor::Commands::apply(doc,edits,created,code,message)){qCritical("%s",qPrintable(message));return 2;}
  EditorWindow editor(doc);editor.resize(1180,820);editor.show();
  auto grab=[&](const QString& name){putgd::test::settle(450);app.processEvents();const auto image=editor.grab();const auto path=QDir(output).filePath(name+".png");if(!image.save(path))return false;if(!native.step(editor,name))return false;captures.append(QJsonObject{{"file",name+".png"},{"width",image.width()},{"height",image.height()}});return true;};
  editor.selectTool(editor::Tool::Redact);if(!grab("20-editor-redact"))return 2;
  editor.selectTool(editor::Tool::Blur);if(!grab("21-editor-blur-warning"))return 2;
  editor.selectTool(editor::Tool::Arrow);if(!grab("22-editor-arrow-style"))return 2;
  editor.selectTool(editor::Tool::Crop);if(!grab("23-editor-crop"))return 2;
  editor.canvas().cancelCrop();editor.resize(900,620);if(!grab("24-editor-compact"))return 2;
  editor.setBusy("Preparing the edited image…");if(!grab("25-editor-finalizing"))return 2;
  editor.allowClose();editor.close();
 }
#ifdef PUTGD_PREVIEW_REVIEW
 {
  // The private review and the ready control bar, over a synthetic frame.
  record::ReviewWindow::Options o;o.workingClip=QDir(output).filePath("no-clip.mkv");o.durationUs=32000000;o.canvas=QSize(960,600);o.formats={"mp4","webm","gif","webp"};o.codecs=QJsonObject{{"mp4",QJsonArray{"h264","h265"}},{"webm",QJsonArray{"vp9"}}};
  RecordingConfig config;config.format="mp4";config.codec="h264";config.fps=30;
  record::ReviewWindow review(o,config);review.resize(record::ReviewWindow::defaultSize());review.show();
  if(auto* view=review.findChild<record::FrameView*>())view->setFrame(syntheticCapture(2),12000000);
  RecordingEdit edit;edit.revision=1;RecordingMask solid;solid.id="m-solid";solid.kind="solid";solid.rect=QRect(61,194,256,40);solid.start=8000000;solid.end=21000000;RecordingMask blur=solid;blur.id="m-blur";blur.kind="blur";blur.rect=QRect(337,194,256,130);blur.start=3000000;blur.end=12000000;edit.masks={solid,blur};edit.trimStart=2000000;edit.trimEnd=29000000;review.setEdit(edit);
  auto shot=[&](QWidget& w,const QString& name){putgd::test::settle(450);app.processEvents();const auto image=w.grab();if(!image.save(QDir(output).filePath(name+".png")))return false;if(!native.step(w,name))return false;captures.append(QJsonObject{{"file",name+".png"},{"width",image.width()},{"height",image.height()}});return true;};
  if(!shot(review,"30-review"))return 2;
  review.drive({{"action","selectMask"},{"maskId","m-blur"}});if(!shot(review,"31-review-blur-selected"))return 2;
  review.resize(760,420);if(!shot(review,"32-review-minimum"))return 2;
  review.hide();
  record::ControlBar::Options bar;bar.targetLabel="Region · 960 × 600";bar.formats=o.formats;bar.codecs=o.codecs;bar.audioSupported=true;
  record::ControlBar ready(bar,config);ready.show();if(!shot(ready,"33-record-ready-bar"))return 2;
  ready.setState("recording");ready.setRetained(12300000);if(!shot(ready,"34-record-recording-bar"))return 2;
  ready.setState("paused");if(!shot(ready,"35-record-paused-bar"))return 2;
 }
#endif
 // The custom frame under real input: the driver moves, maximizes, restores,
 // resizes and finally closes the main window.
 window.resize(1060,680);window.showNormal();if(!native.chrome(window,"90-chrome"))return 2;
 QSaveFile manifest(QDir(output).filePath("manifest.json"));if(!manifest.open(QIODevice::WriteOnly))return 2;manifest.write(QJsonDocument(QJsonObject{{"kind","synthetic-ui-preview"},{"desktopAcceptance",false},{"platform",QGuiApplication::platformName()},{"networkActions",0},{"credentialFixture","synthetic session only"},{"sourceReads",source.reads},{"captures",captures}}).toJson(QJsonDocument::Indented));if(!manifest.commit())return 2;return source.reads==0?0:2;
}
