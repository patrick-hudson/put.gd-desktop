// Synthetic visual verification only. This does not establish desktop support.
#include "fixtures/UiSessionFixture.h"
#include "ui/MainWindow.h"
#include "ui/AboutWindow.h"
#include "ui/HistoryWindow.h"
#include "ui/HistoryModel.h"
#include "ui/FileDialog.h"
#include "ui/TrayController.h"
#include <QFileDialog>
#include "fixtures/TrayCatalog.h"
#include "fixtures/CatalogLabels.h"
#include "ui/SettingsWindow.h"
#include "platform/HostOpen.h"
#include "storage/proton/ProtonProvider.h"
#include "ui/Ui.h"
#include "ui/EditorWindow.h"
#include "editor/EditorCommands.h"
#include "app/DiagnosticsBundle.h"
#include <QApplication>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QContextMenuEvent>
#include <QAbstractSpinBox>
#include <QSpinBox>
#include <QStyleOptionSpinBox>
#include <QComboBox>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QPointer>
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
#include <QPlainTextEdit>
#include <QMessageBox>
#include <QMenu>
#include <QProgressBar>
#include <QPushButton>
#include <QStandardPaths>
#include <QStatusBar>
#include <QSystemTrayIcon>
#include <QTabWidget>
#include <QToolButton>
#include <QElapsedTimer>
#include <QTemporaryDir>
#include <QTimer>
#include <QTest>
#include <atomic>
#include <optional>
#include <thread>
#include <QScopeGuard>
#include <QAbstractButton>
#include <QAbstractItemView>
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QShortcut>
#include <QTabBar>
#include <QTextEdit>
#include "fixtures/Waits.h"
#include "ui/Theme.h"
#include "ui/Fonts.h"
#include <QFile>
#include <QScreen>
#include <QWindow>
#ifdef PUTGD_CATALOG_REVIEW
#include "record/ReviewWindow.h"
#include "fixtures/ReviewLayout.h"
#include "ui/RegionOverlay.h"
#include "record/Overlays.h"
#endif
using namespace putgd;
using namespace putgd::test;
namespace {
// The real build this binary was compiled from (the generated build-info.json),
// not the fixed identity linked for stable renders: the surface driver's live
// gate binds a catalog to that exact commit. Empty when unreadable (refused).
QJsonObject sourceBuild(){QFile file(QStringLiteral(PUTGD_SOURCE_BUILD_INFO));if(!file.open(QIODevice::ReadOnly))return {};return QJsonDocument::fromJson(file.readAll()).object();}
bool checkSteppers(QWidget& window){
 for(auto* spin:window.findChildren<QAbstractSpinBox*>()){
  if(!spin->isVisibleTo(&window)||spin->buttonSymbols()==QAbstractSpinBox::NoButtons)continue;
  QStyleOptionSpinBox option;option.initFrom(spin);option.frame=spin->hasFrame();option.buttonSymbols=spin->buttonSymbols();
  const auto up=spin->style()->subControlRect(QStyle::CC_SpinBox,&option,QStyle::SC_SpinBoxUp,spin);
  const auto down=spin->style()->subControlRect(QStyle::CC_SpinBox,&option,QStyle::SC_SpinBoxDown,spin);
  auto* field=spin->findChild<QLineEdit*>();
  if(up.width()<32||up.height()<32||down.width()<32||down.height()<32||up.intersects(down)||!field||field->width()<field->fontMetrics().horizontalAdvance(field->displayText())+2){qCritical("Stepper does not fit: %s",qPrintable(spin->objectName()));return false;}
 }
 return true;
}
// Surface map (tools/lab/surface_map): every visible control and key binding of
// a captured window, in the capture's pixel coordinates, so hotspots and key
// strips come from the build rather than from hand-placed rectangles.
QJsonObject inventory(QWidget& root){
 QJsonArray controls,keys;
 auto rectOf=[&](QWidget* w){const auto tl=w->mapTo(&root,QPoint());return QJsonArray{tl.x(),tl.y(),w->width(),w->height()};};
 auto add=[&](QWidget* w,const QString& kind,const QString& text,QJsonObject extra={}){QJsonObject c{{"name",w->objectName()},{"class",QString::fromLatin1(w->metaObject()->className())},{"kind",kind},{"text",text},{"rect",rectOf(w)},{"enabled",w->isEnabled()}};for(auto it=extra.begin();it!=extra.end();++it)c[it.key()]=it.value();controls.append(c);};
 if(auto* menu=qobject_cast<QMenu*>(&root))for(auto* a:menu->actions()){if(a->isSeparator())continue;const auto r=menu->actionGeometry(a);controls.append(QJsonObject{{"name",a->objectName()},{"class","QAction"},{"kind","menu-item"},{"text",a->text()},{"rect",QJsonArray{r.x(),r.y(),r.width(),r.height()}},{"enabled",a->isEnabled()},{"submenu",a->menu()!=nullptr}});}
 for(auto* w:root.findChildren<QWidget*>()){
  // Same native window only: a child dialog's coordinates are not this capture's.
  if(w->window()!=root.window()||!w->isVisibleTo(&root)||w->width()<=0||w->height()<=0||w->visibleRegion().isEmpty())continue;
  if(auto* b=qobject_cast<QAbstractButton*>(w)){QJsonObject extra{{"checkable",b->isCheckable()},{"checked",b->isChecked()}};if(!b->shortcut().isEmpty())extra["shortcut"]=b->shortcut().toString(QKeySequence::PortableText);QString text=b->text();if(auto* t=qobject_cast<QToolButton*>(b);t&&t->defaultAction()&&text.isEmpty())text=t->defaultAction()->text();if(text.isEmpty())text=b->toolTip();add(w,"button",text,extra);}
  else if(auto* e=qobject_cast<QLineEdit*>(w)){if(qobject_cast<QAbstractSpinBox*>(w->parentWidget())||qobject_cast<QComboBox*>(w->parentWidget()))continue;add(w,"field",e->placeholderText().isEmpty()?e->toolTip():e->placeholderText(),{{"readOnly",e->isReadOnly()},{"echo",int(e->echoMode())}});}
  else if(qobject_cast<QPlainTextEdit*>(w)||qobject_cast<QTextEdit*>(w))add(w,"text-area",w->toolTip());
  else if(auto* c=qobject_cast<QComboBox*>(w))add(w,"choice",c->currentText(),{{"count",c->count()}});
  else if(auto* s=qobject_cast<QAbstractSpinBox*>(w))add(w,"stepper",s->text());
  else if(auto* s=qobject_cast<QAbstractSlider*>(w)){if(qobject_cast<QScrollBar*>(w))continue;add(w,"slider",w->toolTip(),{{"value",s->value()},{"min",s->minimum()},{"max",s->maximum()}});}
  else if(auto* v=qobject_cast<QAbstractItemView*>(w))add(w,"list",w->toolTip(),{{"rows",v->model()?v->model()->rowCount():0}});
  else if(auto* t=qobject_cast<QTabBar*>(w)){QJsonArray tabs;for(int i=0;i<t->count();++i){const auto r=t->tabRect(i);const auto tl=t->mapTo(&root,r.topLeft());tabs.append(QJsonObject{{"text",t->tabText(i)},{"rect",QJsonArray{tl.x(),tl.y(),r.width(),r.height()}},{"current",i==t->currentIndex()}});}add(w,"tabs",QString(),{{"tabs",tabs}});}
  else if(auto* l=qobject_cast<QLabel*>(w)){const auto kind=catalogLabelKind(*l);if(!kind.isEmpty())add(w,kind,l->text());}
  else if(!w->objectName().isEmpty()&&w->focusPolicy()!=Qt::NoFocus&&!qobject_cast<QAbstractScrollArea*>(w)&&!qobject_cast<QMenu*>(w))add(w,"focusable",w->toolTip());
 }
 for(auto* a:root.findChildren<QAction*>()){QJsonArray s;for(const auto& k:a->shortcuts())if(!k.isEmpty())s.append(k.toString(QKeySequence::PortableText));if(s.isEmpty())continue;keys.append(QJsonObject{{"name",a->objectName()},{"text",a->text()},{"keys",s},{"enabled",a->isEnabled()},{"context",int(a->shortcutContext())}});}
 for(auto* s:root.findChildren<QShortcut*>()){if(s->key().isEmpty())continue;keys.append(QJsonObject{{"name",s->objectName()},{"text",s->whatsThis()},{"keys",QJsonArray{s->key().toString(QKeySequence::PortableText)}},{"enabled",s->isEnabled()},{"context",int(s->context())}});}
 return {{"width",root.width()},{"height",root.height()},{"class",QString::fromLatin1(root.metaObject()->className())},{"controls",controls},{"keys",keys}};
}
bool writeInventory(const QString& output,const QString& name,QWidget& root){
 QSaveFile f(QDir(output).filePath(name+".controls.json"));
 return f.open(QIODevice::WriteOnly)&&f.write(QJsonDocument(inventory(root)).toJson())>0&&f.commit();
}
bool saveDialog(const QString& output,const QString& name,QWidget& dialog,QJsonArray& captures){
 const auto image=dialog.grab();if(!image.save(QDir(output).filePath(name+".png"))||!writeInventory(output,name,dialog))return false;
 captures.append(QJsonObject{{"file",name+".png"},{"controls",name+".controls.json"},{"width",image.width()},{"height",image.height()}});return true;
}
// Native capture mode (2026-10-26): with
// PUTGD_CATALOG_HANDSHAKE=DIR the tool runs on the real platform under an
// owned magenta backdrop, publishes each state's window and waits for the
// workstation's RFB capture driver to write DIR/<state>.done. Still synthetic
// content; the frames are real compositor pixels of these windows.
struct Native {
 QString dir=qEnvironmentVariable("PUTGD_CATALOG_HANDSHAKE");QWidget* backdrop=nullptr;
 bool active()const{return !dir.isEmpty();}
 void cover(){
  if(!active()||qEnvironmentVariable("PUTGD_CATALOG_BACKDROP")!="1")return;
  backdrop=new QWidget;backdrop->setObjectName("putgd-catalog-backdrop");backdrop->setWindowTitle("put.gd catalog backdrop");backdrop->setWindowFlag(Qt::FramelessWindowHint);
  QPalette palette;palette.setColor(QPalette::Window,QColor(255,0,255));backdrop->setPalette(palette);backdrop->setAutoFillBackground(true);backdrop->showFullScreen();
  putgd::test::settle(800);
 }
 static QJsonObject geometry(QWidget& w){const auto f=w.frameGeometry();QJsonObject g{{"x",f.x()},{"y",f.y()},{"width",f.width()},{"height",f.height()},{"maximized",w.isMaximized()},{"minimized",w.isMinimized()},{"visible",w.isVisible()},{"platform",QGuiApplication::platformName()}};
  // The shared header's band and window-control centres, for the chrome driver.
  if(auto* header=w.findChild<QWidget*>("windowHeader");header&&header->isVisible()){g["header"]=header->height();QJsonObject controls;for(const auto* n:{"windowMinimize","windowMaximize","windowClose"})if(auto* b=header->findChild<QWidget*>(n)){const auto c=b->mapTo(&w,b->rect().center());controls[n]=QJsonArray{c.x(),c.y()};}g["controls"]=controls;}
  return g;}
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
  // After a minimize the harness shows the window again (a taskbar click the
  // driver cannot make portably), so the remaining steps can continue. Wayland
  // clients never learn they were minimized and cannot raise themselves, so the
  // driver may also ask for the window to be mapped again (<name>.reshow).
  class Watch final:public QObject {public:QString path;QWidget* w;bool eventFilter(QObject*,QEvent* e)override{if(e->type()==QEvent::Move||e->type()==QEvent::Resize||e->type()==QEvent::WindowStateChange||e->type()==QEvent::Close||e->type()==QEvent::Hide||e->type()==QEvent::Show){QSaveFile f(path);if(f.open(QIODevice::WriteOnly)){auto g=geometry(*w);g["event"]=int(e->type());f.write(QJsonDocument(g).toJson(QJsonDocument::Compact));f.commit();}if(e->type()==QEvent::WindowStateChange&&w->isMinimized())QTimer::singleShot(2500,w,[w=w]{w->showNormal();w->raise();w->activateWindow();});}return false;}};
  Watch watch;watch.path=dir+"/"+name+".geometry";watch.w=&w;w.installEventFilter(&watch);
  QTimer reshow;reshow.setInterval(200);QObject::connect(&reshow,&QTimer::timeout,&w,[&]{if(QFile::remove(dir+"/"+name+".reshow")){w.hide();w.show();w.raise();w.activateWindow();}});reshow.start();
  const bool done=step(w,name,180000);w.removeEventFilter(&watch);return done;
 }
};
bool waitUntil(const std::function<bool()>& condition){return putgd::test::waitFor(condition,8000);}
// Scroll a control into view after pending layout (a resize, a banner) settles.
// ensureWidgetVisible moves the least distance from where the page is, so start
// from the top (as a user would scroll, which also ends any page-driven follow).
void reveal(QScrollArea* scroll,QWidget* widget,int margin){for(int pass=0;pass<2;++pass){putgd::test::settle(100);scroll->verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);scroll->ensureWidgetVisible(widget,0,margin);}}
// The busy indicator sweeps with time (Ui::Style); captures show it mid-sweep.
void freezeBusy(QWidget& root){for(auto* bar:root.findChildren<QProgressBar*>())if(bar->minimum()==0&&bar->maximum()==0)bar->setProperty("uiBusyPhase",0.25);}
// History loads list thumbnails in the background: true once every visible row
// with a preview file on disk has its thumbnail.
bool thumbnailsLoaded(QWidget& root){
 for(auto* rows:root.findChildren<QListView*>("historyList")){
  auto* model=qobject_cast<HistoryModel*>(rows->model());if(!model||!rows->isVisible())continue;
  for(int r=0;r<model->rowCount();++r){const auto index=model->index(r);const auto path=model->record(r)["previewPath"].toString();if(rows->viewport()->rect().intersects(rows->visualRect(index))&&!path.isEmpty()&&QFileInfo(path).isFile()&&model->data(index,HistoryModel::ThumbnailRole).isNull())return false;}
 }
 return true;
}
// A capture for the reference images: wait for History's thumbnails, then,
// since on a loaded machine debounced relayouts can still be landing, grab
// until two frames 150 ms apart are identical.
QPixmap steadyGrab(QWidget& root){
 if(!waitUntil([&]{return thumbnailsLoaded(root);}))qWarning("Thumbnails of %s did not load",qPrintable(root.objectName()));
 freezeBusy(root);auto frame=root.grab();
 for(int attempt=0;attempt<12;++attempt){putgd::test::settle(150);freezeBusy(root);auto next=root.grab();if(next.toImage()==frame.toImage())return next;frame=next;}
 qWarning("Capture of %s did not settle",qPrintable(root.objectName()));return frame;
}
// The synthetic data folders. Their paths show in Settings, so reference
// renders (tools/ci/check_catalog_references.py) name a fixed, empty parent in
// PUTGD_CATALOG_DATA_DIR; otherwise each is a new temporary directory.
class DataDir {
public:
 explicit DataDir(const QString& name){
  const auto root=qEnvironmentVariable("PUTGD_CATALOG_DATA_DIR");
  if(root.isEmpty()){temp_.emplace("/tmp/"+name+"-XXXXXX");if(temp_->isValid())path_=temp_->path();return;}
  const auto path=QDir(root).filePath(name);if(QDir().mkpath(path)&&QDir(path).isEmpty())path_=path;
 }
 bool isValid()const{return !path_.isEmpty();}
 QString path()const{return path_;}
 QString filePath(const QString& name)const{return QDir(path_).filePath(name);}
private:
 std::optional<QTemporaryDir> temp_;QString path_;
};
QImage iconSpecimen(){
 const auto& theme=Theme::current();const auto& tints=Theme::trayTints(Theme::scheme()==Theme::Scheme::Light);
 const QStringList names{"region","window","desktop","record","alert","clipboard","copy","save","history","destinations","settings","preferences","info","shortcuts","images","bucket","proton","add","upload","download","failed-uploads","edit","stop","select","redact","blur","crop","annotate-arrow","rectangle","highlight","chevron-down","text","undo","redo","refresh","zoom-fit","zoom-actual","back","forward","up","folder-new","view-list","view-detail","play","pause","trim-in","trim-out","trim-reset","mask-in","mask-out","remove","file","video"};
 const int rows=(names.size()+7)/8,identityY=64+rows*94;
 QImage image(1280,identityY+340,QImage::Format_RGB32);image.fill(theme.bg);QPainter p(&image);p.setPen(theme.text);p.setFont(Ui::font(Ui::Type::body));
 p.drawText(24,28,"Outline family · 40 px reference + actual 16 px and 20 px (text-scaled) · production renderer");
 auto caption=p.font();caption.setPixelSize(11);p.setFont(caption);
 for(int i=0;i<names.size();++i){const int x=24+(i%8)*156,y=52+(i/8)*94;Ui::icon(names[i]).paint(&p,QRect(x+12,y,40,40));Ui::icon(names[i]).paint(&p,QRect(x+64,y+12,16,16));const int side=Ui::scaled(names[i]=="info"?14:Ui::Metric::icon);Ui::icon(names[i]).paint(&p,QRect(x+88,y+(40-side)/2,side,side));p.drawText(QRect(x,y+48,144,22),Qt::AlignCenter,names[i]);}
 p.drawText(24,identityY,"Solid identity artwork · encoding fill");Ui::icon("brand").paint(&p,QRect(24,identityY+20,64,64));
 for(int i=0;i<5;++i){const int x=155+i*125;Ui::icon("tray",tints.idle,i*25).paint(&p,QRect(x,identityY+20,64,64));p.drawText(QRect(x,identityY+94,64,24),Qt::AlignCenter,QString::number(i*25)+"%");}
 for(int i=0;i<2;++i){const int x=860+i*130;Ui::icon("tray",i?tints.idle:tints.error).paint(&p,QRect(x,identityY+20,64,64));p.drawText(QRect(x-10,identityY+94,92,24),Qt::AlignCenter,i?"Blink off":"Blink on");}
 p.drawText(24,identityY+152,"Actual tray sizes: 16 / 22 / 24 / 32 px");
 for(int row=0;row<2;++row){const int y=identityY+170+row*56;p.fillRect(QRect(24,y,1232,48),QColor(row?"#e4e5e9":"#20242d"));
  int x=44;for(int size:{16,22,24,32}){Ui::icon("tray",Theme::trayTints(row).idle).paint(&p,QRect(x,y+(48-size)/2,size,size));x+=60;}
  for(int i=0;i<5;++i)Ui::icon("tray",Theme::trayTints(row).idle,i*25).paint(&p,QRect(300+i*130,y+12,24,24));
  for(int i=0;i<2;++i)Ui::icon("tray",i?Theme::trayTints(row).idle:Theme::trayTints(row).error).paint(&p,QRect(980+i*80,y+12,24,24));
 }
 return image;
}
// The editor's glyphs in the current scheme: a 40 px reference row, then the
// real 20 px rows (the editor toolbar, and Recording review's mask tools that
// share the redact glyph) at 1:1 and magnified 3x without smoothing.
QImage editorIcons(){
 const auto& t=Theme::current();const int rowHeight=Ui::scaled(20)+16;QImage image(1800,112+3*(3*rowHeight+32),QImage::Format_RGB32);image.fill(t.surface);QPainter p(&image);
 QFont small=p.font();small.setPixelSize(11);p.setFont(small);p.setPen(t.textMuted);
 const QStringList names{"edit","select","redact","blur","crop","annotate-arrow","rectangle","highlight","text","undo","redo","remove","save"};
 for(int i=0;i<names.size();++i){const int x=12+i*86;Ui::icon(names[i]).paint(&p,QRect(x+23,14,40,40));p.drawText(QRect(x,60,86,20),Qt::AlignCenter,names[i]);}
 auto row=[&](const QStringList& icons,int step){
  const int glyph=Ui::scaled(20);step=std::max(step,glyph+12);QImage strip(16+icons.size()*step,glyph+16,QImage::Format_RGB32);strip.fill(t.bg);QPainter q(&strip);
  for(int i=0;i<icons.size();++i){if(icons[i]=="|"){q.fillRect(QRect(8+i*step+step/2,7,1,22),t.border);continue;}Ui::icon(icons[i]).paint(&q,QRect(8+i*step+(step-glyph)/2,8,glyph,glyph));}
  return strip;
 };
 int y=112;auto show=[&](const QString& title,const QImage& strip){p.drawText(QPoint(12,y-6),title);p.drawImage(QPoint(12,y),strip);p.drawImage(QPoint(32+strip.width(),y),strip.scaled(strip.size()*3,Qt::IgnoreAspectRatio,Qt::FastTransformation));y+=3*strip.height()+32;};
 show("Editor toolbar, 20 px (1:1, then 3x)",row({"select","redact","blur","crop","|","annotate-arrow","rectangle","highlight","text"},40));
 show("Editor view tools | document actions",row({"undo","redo","refresh","remove","|","save","upload"},32));
 show("Recording review mask tools, 20 px",row({"blur","redact","|","mask-in","mask-out","remove"},32));
 return image;
}
QImage syntheticCapture(int index){
 const auto& c=Theme::current();QImage image(960,600,QImage::Format_RGB32);image.fill(c.bg);QPainter p(&image);p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(c.surface);p.drawRoundedRect(QRect(24,20,912,560),16,16);p.setBrush(c.border);p.drawRoundedRect(QRect(24,20,912,45),16,16);for(int i=0;i<3;++i){p.setBrush(i==0?c.danger:i==1?c.warning:c.success);p.drawEllipse(QPoint(47+20*i,43),5,5);}QFont f(Fonts::ui());f.setPixelSize(24);f.setBold(true);p.setFont(f);p.setPen(c.text);const QStringList titles{"Design review","Build summary","Project notes","Release checklist","Weekly highlights","Product research","Interface library","Upload overview"};p.drawText(QRect(61,91,830,44),titles.value(index%titles.size()));f.setBold(false);f.setPixelSize(14);p.setFont(f);p.setPen(c.textMuted);p.drawText(QRect(61,140,830,25),"Synthetic sample content for the put.gd UI catalog");
 for(int col=0;col<3;++col){const QRect card(61+col*276,194,256,130);p.setPen(QPen(c.borderStrong));p.setBrush(c.surfaceRaised);p.drawRoundedRect(card,10,10);p.setPen(c.textMuted);p.drawText(card.adjusted(18,16,-18,-80),QStringList{"CAPTURES","UPLOADED","SAVED LOCALLY"}.at(col));f.setPixelSize(31);f.setBold(true);p.setFont(f);p.setPen(col==1?c.success:c.accent);p.drawText(card.adjusted(18,53,-18,-16),QString::number((index+1)*7+col*3));f.setPixelSize(14);f.setBold(false);p.setFont(f);}
 p.setPen(c.textSecondary);p.drawText(QRect(61,355,800,24),"Latest activity");for(int row=0;row<4;++row){int y=402+row*34;p.setPen(QPen(c.border));p.drawLine(61,y+22,888,y+22);p.setPen(row==0?c.accent:c.textSecondary);p.drawText(QRect(68,y-2,580,24),QStringList{"Review capture ready to share","Notes and reference files","Updated component library","Final checks complete"}.at((row+index)%4));p.setPen(c.textMuted);p.drawText(QRect(750,y-2,130,24),Qt::AlignRight,"Just now");}return image;
}
QJsonObject catalogConfig(const QString& root){
 auto doc=Settings::developmentTemplate();auto s3=doc["profiles"].toArray().first().toObject();s3["id"]="synthetic-archive";s3["label"]="Project archive";auto s=s3["settings"].toObject();s["bucket"]="design-captures";s["region"]="us-east-1";s["endpoint"]="https://s3.us-east-1.amazonaws.com";s["objectPrefix"]="screenshots/";s["aclMode"]="omit";s["publicUrlBase"]="https://captures.example.invalid";s3["settings"]=s;s3["credentials"]=QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}};
 QJsonObject proton{{"id","synthetic-proton"},{"label","Proton Drive"},{"provider","proton-drive"},{"settings",QJsonObject{{"accountRootUid","synthetic-root"},{"folderUid","synthetic-folder"},{"folderName","put.gd"},{"shareByDefault",true},{"linkExpirationDays",0},{"sharePasswordReference",QJsonValue::Null},{"requestTimeoutMs",120000}}},{"credentials",QJsonObject{{"mode","session"},{"reference",QJsonValue::Null}}}};doc["profiles"]=QJsonArray{proton,s3};auto actions=doc["actions"].toObject();int index=1;for(const auto& name:Settings::actionNames()){auto a=actions[name].toObject();a["profileId"]="synthetic-proton";a["shortcut"]="Ctrl+"+QString::number(index++);actions[name]=a;}doc["actions"]=actions;auto archive=doc["archive"].toObject();archive["root"]=root+"/local-copies";archive["keepUploads"]=true;doc["archive"]=archive;auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;return doc;
}
bool captureTrayMenus(const QString& output,QJsonArray& captures){
 DataDir data("pgt");if(!data.isValid())return false;
 SettingsService settings(data.filePath("config.json"),[](const QString&,const QJsonObject&){return Error{};});
 auto config=catalogConfig(data.path());auto preferences=config["preferences"].toObject();preferences["notifications"]=false;config["preferences"]=preferences;if(!settings.save(config,{}))return false;
 ProviderRegistry registry;auto provider=std::make_shared<TrayCatalogProvider>();registry.add("proton-drive",provider);
 // Reply storage outlives the drain guard, including replies arriving after a timeout.
 Error invokeError;QString operationId;QJsonObject failed;bool inspected=false;
 Source source;source.payload=textPayload();OperationService operations(data.filePath("data"),settings,registry,source);
 if(!waitUntil([&]{return operations.ready();}))return false;
 // Release future callbacks too: a timed-out wait can precede upload().
 auto drain=qScopeGuard([&]{provider->release();waitUntil([&]{return operations.diagnostics()["activeOperations"].toInt()==0;});});
 TrayController controller(operations,[](const QString&){},[]{});
 auto* tray=controller.findChild<QSystemTrayIcon*>();if(!tray||!tray->contextMenu())return false;
 auto* menu=tray->contextMenu();
 controller.setUpdateStatus({{"channel","stable"},{"result",QJsonObject{{"updateAvailable",true},{"latest","26.1029.9999"}}}});
 auto shot=[&](QMenu& root,const QString& name){
  // QWidget::adjustSize caps top-level windows to two thirds of the offscreen
  // screen. Size to the real menu's hint so the taller upload state keeps every
  // row in the reference instead of cropping its About/Quit group.
  root.ensurePolished();root.resize(root.sizeHint());root.show();putgd::test::settle(40);
  // Unlike the general widget inventory, a menu inventory contains exactly its
  // visible action rows, including QWidgetAction output labels. Hidden
  // conditional actions have no click target; child QLabel geometry is not a row.
  const auto rows=catalogMenuRows(root);if(!rows)return false;
  const auto image=steadyGrab(root);QSaveFile controls(QDir(output).filePath(name+".controls.json"));
  if(!image.save(QDir(output).filePath(name+".png"))||!controls.open(QIODevice::WriteOnly)||controls.write(QJsonDocument(QJsonObject{{"width",root.width()},{"height",root.height()},{"class","QMenu"},{"controls",*rows},{"keys",QJsonArray{}}}).toJson())<=0||!controls.commit())return false;
  captures.append(QJsonObject{{"file",name+".png"},{"controls",name+".controls.json"},{"width",image.width()},{"height",image.height()}});root.close();return true;
 };
 if(!shot(*menu,"00-tray-menu-idle"))return false;
 QAction* editSources=nullptr;for(auto* action:menu->actions())if(action->data()=="edit-sources")editSources=action;
 if(!editSources||!editSources->menu()||!shot(*editSources->menu(),"00-tray-edit-submenu"))return false;
 auto* summary=menu->findChild<QAction*>("trayOutputSummary");if(!summary||!summary->menu()||!shot(*summary->menu(),"00-tray-output-submenu"))return false;
 auto* uploading=menu->findChild<QAction*>("trayUploadStatus");auto* attention=menu->findChild<QAction*>("trayFailedUploads");if(!uploading||!attention)return false;
 operations.invoke("clipboard",[&](Error error,QJsonObject result){invokeError=error;operationId=result["operationId"].toString();});
 if(!waitUntil([&]{return invokeError||(!operationId.isEmpty()&&provider->held()&&uploading->isVisible());})||invokeError)return false;
 if(!shot(*menu,"00-tray-menu-uploading"))return false;
 provider->release();
 if(!waitUntil([&]{return attention->isVisible()&&attention->text()=="Failed uploads (1)"&&!uploading->isVisible();}))return false;
 operations.inspect(operationId,[&](Error error,QJsonObject value){invokeError=error;failed=value;inspected=true;});
 if(!waitUntil([&]{return inspected;})||invokeError||!QStringList{"failed","partial"}.contains(failed["state"].toString())||failed["items"].toArray().isEmpty()||failed["items"].toArray().first().toObject()["error"].toObject()["code"]!="proton_account_busy")return false;
 return shot(*menu,"00-tray-menu-proton-busy");
}
}
int main(int argc,char** argv){
 QApplication app(argc,argv);app.setApplicationName("putgd-ui-catalog");
 // A steady text cursor: a blinking one would differ between otherwise identical captures.
 QApplication::setCursorFlashTime(0);
 // Offscreen renders show put.gd's own frame (header window controls,
 // Review's compact header), as on X11 and Wayland; native runs decide it.
 if(QGuiApplication::platformName()=="offscreen")Ui::WindowHeader::setCustomFrameOverride(1);app.setWindowIcon(Ui::icon("brand"));Native native;if(qEnvironmentVariable("PUTGD_CATALOG_THEME")=="light")Theme::setScheme(Theme::Scheme::Light);if(app.arguments().size()!=2){qCritical("Usage: putgd-ui-catalog OUTPUT_DIRECTORY");return 2;}const auto output=QFileInfo(app.arguments().at(1)).absoluteFilePath();if(!QDir().mkpath(output)){qCritical("Cannot create catalog output directory");return 2;}
 if(!iconSpecimen().save(QDir(output).filePath("00-icon-specimen.png")))return 2;
 {const int side=Ui::scaled(16);QImage providers(360,140,QImage::Format_RGB32);providers.fill(Theme::current().bg);QPainter painter(&providers);painter.setPen(Theme::current().textSecondary);painter.setFont(Ui::font(Ui::Type::metadata));int x=12;for(const auto& name:QStringList{"destinations","bucket","proton"}){const auto glyph=Ui::icon(name).pixmap(side,side).toImage();painter.drawImage(QPoint(x,8),glyph);painter.drawImage(QPoint(x+36,8),glyph.scaled(side*3,side*3,Qt::IgnoreAspectRatio,Qt::FastTransformation));painter.drawText(QRect(x,100,112,32),name);x+=120;}painter.end();if(!providers.save(QDir(output).filePath("provider-icons-16.png")))return 2;}
 // The fixture calls the resident dispatcher directly and never opens a socket,
 // but enforce the same Linux path precondition before constructing the app.
 const auto socket=QDir(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation)).filePath("putgd/session.sock");if(socket.toUtf8().size()>107){qCritical("Resolved Unix socket exceeds 107 UTF-8 bytes");return 2;}
 {
  QImage states(1260,400,QImage::Format_ARGB32_Premultiplied);QPainter painter(&states);painter.setFont(Ui::font(Ui::Type::metadata));
  const QStringList labels{"Idle","Recording","Paused","Uploading + recording","Failed","Failed + recording","Failed + paused"};
  for(const bool light:{false,true}){const int y=light?200:0;const auto& panel=Theme::palette(light?Theme::Scheme::Light:Theme::Scheme::Dark);painter.fillRect(0,y,states.width(),200,panel.bg);painter.setPen(panel.text);const auto& tints=Theme::trayTints(light);
   for(int n=0;n<labels.size();++n){const auto icon=Ui::icon("tray",tints.idle,n>=4?-2:n==3?50:-1,n==1||n==3||n==5?1:n==2||n==6?2:0);painter.drawPixmap(n*180+66,y+8,icon.pixmap(48,48));int x=n*180+32;for(int size:{16,20,24}){painter.drawPixmap(x,y+76,icon.pixmap(size,size));x+=40;}painter.drawText(QRect(n*180,y+112,180,84),Qt::AlignHCenter|Qt::TextWordWrap,labels[n]);}
  }painter.end();if(!states.save(QDir(output).filePath("settings-tray-states.png")))return 2;
 }
 DataDir data("pgr");if(!data.isValid())return 2;const auto config=catalogConfig(data.path());SettingsService settings(data.filePath("synthetic-config.json"),[](const QString&,const QJsonObject&){return Error{};});if(!settings.save(config,{})){qCritical("Cannot write synthetic catalog configuration");return 2;}
 // No production provider or capture action is used. Credentials below are
 // synthetic and stored only in the fixture session, never the OS keychain.
 ProviderRegistry registry;Source source;OperationService operations(data.filePath("data"),settings,registry,source);if(!waitUntil([&]{return operations.ready();})){qCritical("Synthetic resident did not initialize");return 2;}
 const auto now=QDateTime::currentDateTimeUtc();{HistoryStore ledger(operations.databasePath());for(int i=0;i<8;++i){const auto path=data.filePath(QString("synthetic-preview-%1.png").arg(i));if(!syntheticCapture(i).save(path))return 2;Operation op;op.id=QString("synthetic-preview-operation-%1").arg(i);op.action=i==5?"window":i==6?"clipboard":i==7?"desktop":"region";op.created=now.addSecs(i==7?-86400:-110-i*18).toString(Qt::ISODateWithMs);op.state=i==3?"partial":"completed";op.copyStatus=i==0?"copied":"not_requested";op.settings=settings.snapshot();op.destination.providerId="proton-drive";op.destination.profileId="synthetic-proton";op.destination.profileRevision="synthetic-revision";op.destination.settings=config["profiles"].toArray().first().toObject()["settings"].toObject();OperationItem item;item.artifact.id=QString("synthetic-preview-file-%1").arg(i);item.artifact.name=i==1?"Quarterly-release-readiness-review-with-accessibility-and-localisation-notes-final-v12.png":i==5?"Window — Design review.png":i==6?"Clipboard image.png":i==7?"Desktop.png":"Screenshot "+now.addSecs(-110-i*18).toLocalTime().toString("HH:mm:ss")+".png";item.artifact.mime="image/png";item.artifact.category="images";item.artifact.bytes=QFileInfo(path).size();item.artifact.previewPath=path;item.local.status=i==2?"disabled":"saved";item.local.path=i==2?QString{}:path;item.uploadStatus=i==3?"failed":"uploaded";item.remoteStatus="present";item.shareStatus="shared";item.receipt.destination=op.destination;item.receipt.publicationId="synthetic-publication";item.receipt.objectReference=QJsonObject{{"fileUid","synthetic-file"}};item.receipt.directUrl="https://drive.proton.me/urls/synthetic-preview#example";if(i==3){item.error={"synthetic_network","Connection interrupted. Try again.",{}};item.receipt={};item.shareStatus="not_requested";item.remoteStatus="not_requested";}op.items.append(item);if(ledger.saveOperation(op))return 2;}
  Operation cancelled;cancelled.id="synthetic-cancelled";cancelled.action="region";cancelled.created=now.addSecs(-500).toString(Qt::ISODateWithMs);cancelled.state="cancelled";cancelled.cancelled=true;cancelled.settings=settings.snapshot();if(ledger.saveOperation(cancelled))return 2;
 }
 HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
 // Diagnostics packages save into the catalog's temporary state; the upload
 // destination's check is answered here, so nothing reaches the network.
 DiagnosticsBundles bundles(operations,data.filePath("state"));bundles.setDestinationTest([](const QString&,std::function<void(Error)> done){done({});});fixture.server.setDiagnosticsBundles(&bundles);JsonReply loadFolders;fixture.setup.setProviderSetup([&](const QString&,const Destination&,const QJsonObject&,JsonReply reply,auto){loadFolders=std::move(reply);});QJsonObject catalogUpdate;fixture.server.setUiDiagnostics([&]{const QJsonObject available{{"available",true}};return QJsonObject{{"update",catalogUpdate},{"trayAvailable",true},{"capabilities",QJsonObject{{"source",QJsonObject{{"adapter","gnome-wayland"},{"capture",QJsonObject{{"region",available},{"window",available},{"desktop",available}}}}},{"providers",QJsonObject{{"proton-drive",QJsonObject{}}}}}}};});fixture.session.refreshSnapshot();if(!waitUntil([&]{return fixture.session.diagnostics()["capabilities"].toObject()["providers"].toObject().contains("proton-drive");}))return 2;native.cover();MainWindow window(fixture.session);window.resize(1440,960);auto* catalogStatus=new QLabel("Synthetic catalog");Ui::role(catalogStatus,"meta");window.statusBar()->addPermanentWidget(catalogStatus);QObject::connect(&window,&MainWindow::settingsCreated,catalogStatus,[catalogStatus](SettingsWindow* settings){QObject::connect(settings,&SettingsWindow::viewStateChanged,catalogStatus,[catalogStatus,settings]{catalogStatus->setVisible(!settings->changesUnsaved());});});window.show();auto* list=window.findChild<QListView*>("historyList");if(!list||!waitUntil([&]{return list->model()->rowCount()==8;})){qCritical("Synthetic history did not load");return 2;}list->selectionModel()->select(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(0,0));
 // The compact tier is shared API; show it before individual surfaces adopt it.
 {
  Ui::Surface specimen("bg");auto* rows=new QVBoxLayout(&specimen);rows->setContentsMargins(Ui::space(Ui::Space::xl),Ui::space(Ui::Space::xl),Ui::space(Ui::Space::xl),Ui::space(Ui::Space::xl));rows->setSpacing(Ui::space(Ui::Space::l));
  for(const bool compact:{false,true}){auto* caption=Ui::label({},"section");rows->addWidget(caption);auto* row=new QHBoxLayout;row->setSpacing(Ui::space(Ui::Space::s));
   for(const auto* role:{"primary","lead","secondary","quiet"}){auto* button=new QPushButton(role,&specimen);Ui::role(button,role);if(compact)button->setProperty("uiDensity","compact");row->addWidget(button);}
   auto* combo=new QComboBox(&specimen);combo->addItems({"Compatible MP4","Smaller WebM"});if(compact)combo->setProperty("uiDensity","compact");row->addWidget(combo);auto* tool=Ui::toolButton("Start here","trim-in","quiet",&specimen);if(compact)tool->setProperty("uiDensity","compact");row->addWidget(tool);rows->addLayout(row);auto* measured=row->itemAt(0)->widget();measured->ensurePolished();caption->setText(QString("%1 · %2 px / %3 px%4").arg(compact?"Compact":"Standard").arg(measured->sizeHint().height()).arg(measured->font().pixelSize()).arg(compact?" Regular":""));
  }
  specimen.adjustSize();specimen.show();putgd::test::settle(30);if(!steadyGrab(specimen).save(QDir(output).filePath("00-control-tiers.png")))return 2;
 }
 {
  Ui::Surface specimen("bg");auto* rows=new QVBoxLayout(&specimen);rows->setContentsMargins(Ui::space(Ui::Space::xl),Ui::space(Ui::Space::xl),Ui::space(Ui::Space::xl),Ui::space(Ui::Space::xl));rows->setSpacing(Ui::space(Ui::Space::l));
  rows->addWidget(Ui::label("Inactive actions and readable locked values","section"));auto* row=new QHBoxLayout;row->setSpacing(Ui::space(Ui::Space::s));
  auto* active=new QPushButton("Start here",&specimen);row->addWidget(active);auto* inactive=new QPushButton("Start here",&specimen);inactive->setEnabled(false);row->addWidget(inactive);
  auto* locked=new QComboBox(&specimen);locked->addItem("Forever");Ui::setLocked(locked,true);locked->setEnabled(false);row->addWidget(locked);auto* unavailable=new QComboBox(&specimen);unavailable->addItem("Unavailable");unavailable->setEnabled(false);row->addWidget(unavailable);rows->addLayout(row);
  specimen.adjustSize();specimen.show();putgd::test::settle(30);if(!steadyGrab(specimen).save(QDir(output).filePath("00-inactive-locked-controls.png")))return 2;
 }
 QJsonArray captures,skippedCaptures;auto save=[&](const QString& name){putgd::test::settle(450);app.processEvents();if(name=="01a-history-preview-focus"||name=="10a-history-compact-preview-focus"){window.activateWindow();auto* focusedPreview=window.findChild<QLabel*>("historyPreview");if(QGuiApplication::platformName()=="offscreen"&&!focusedPreview->isVisible()){skippedCaptures.append(QJsonObject{{"state",name},{"reason","inspector_hidden"}});return true;}focusedPreview->setFocus(Qt::TabFocusReason);app.processEvents();if(!focusedPreview->hasFocus())return false;}if(name=="01-history-list"||name=="10-history-compact"){auto* selectedPreview=window.findChild<QLabel*>("historyPreview");if(!selectedPreview||!waitUntil([&]{return !selectedPreview->pixmap().isNull();}))return false;}
  // The selected preview's arrival relayouts the inspector; steadyGrab waits for the list's thumbnails.
  putgd::test::settle(100);app.processEvents();
  if(!checkSteppers(window))return false;if(name=="10-history-compact"){QJsonArray commands;auto* band=window.findChild<QWidget*>("commandBand");for(auto* button:band->findChildren<QToolButton*>("captureActionButton"))commands.append(QJsonObject{{"action",button->property("captureAction").toString()},{"label",button->text()},{"labelVisible",button->toolButtonStyle()!=Qt::ToolButtonIconOnly},{"width",button->width()},{"sizeHint",button->sizeHint().width()}});QFile measurements(QDir(output).filePath("command-band-measurements.json"));if(!measurements.open(QIODevice::WriteOnly))return false;measurements.write(QJsonDocument(QJsonObject{{"windowWidth",window.width()},{"bandWidth",band->width()},{"requiredWidth",band->layout()->sizeHint().width()},{"gap",qobject_cast<QGridLayout*>(band->layout())->horizontalSpacing()},{"commands",commands}}).toJson());}const auto image=steadyGrab(window);const auto path=QDir(output).filePath(name+".png");if(!image.save(path))return false;if(!native.step(window,name)||!writeInventory(output,name,window))return false;captures.append(QJsonObject{{"file",name+".png"},{"controls",name+".controls.json"},{"width",image.width()},{"height",image.height()}});return true;};
 const bool recordOnly=qEnvironmentVariableIsSet("PUTGD_CATALOG_RECORD_ONLY");
 const bool editorOnly=qEnvironmentVariableIsSet("PUTGD_CATALOG_EDITOR_ONLY");
 if(!recordOnly){
 if(!editorOnly){
 auto* preview=window.findChild<QLabel*>("historyPreview");if(!preview||!waitUntil([&]{return !preview->pixmap().isNull();}))return 2;
 if(QGuiApplication::platformName()=="offscreen"){
  if(!captureTrayMenus(output,captures))return 2;
  QMenu status(&window);auto* information=status.addAction("Uploading… 50% · 2 MB of 4 MB");information->setEnabled(false);information->setProperty("visualRole","information");status.addSeparator();status.addAction("Cancel upload");status.addSeparator();status.addAction("History");status.show();putgd::test::settle(20);if(!steadyGrab(status).save(QDir(output).filePath("00-tray-upload-status.png"))||!writeInventory(output,"00-tray-upload-status",status))return 2;status.close();
  QMenu menu(&window);menu.addAction("All actions: Shared link · R&&D · No local copy")->setEnabled(false);menu.addAction("Capture region && edit: Nothing is saved or uploaded")->setEnabled(false);menu.addSeparator();menu.addAction("Change output");menu.show();putgd::test::settle(20);const auto image=steadyGrab(menu);if(!image.save(QDir(output).filePath("00-output-menu-literal-ampersand.png"))||!writeInventory(output,"00-output-menu-literal-ampersand",menu))return 2;captures.append(QJsonObject{{"file","00-output-menu-literal-ampersand.png"},{"controls","00-output-menu-literal-ampersand.controls.json"},{"width",image.width()},{"height",image.height()}});menu.close();
 }
 if(!save("01-history-list"))return 2;
 if(QGuiApplication::platformName()=="offscreen"){
  auto* header=window.findChild<Ui::WindowHeader*>("windowHeader");header->setButtonLayout(WindowButtons::fromGSettings("close,minimize,maximize:"));if(!save("01b-history-left-window-buttons"))return 2;header->setButtonLayout(WindowButtons::instance().layout());
 }

 {auto* preview=window.findChild<QLabel*>("historyPreview");const bool open=!preview->isVisible();if(open)window.history()->command({{"action","openDetails"}},[](Error,QJsonObject){});preview->setFocus(Qt::TabFocusReason);if(!save("01a-history-preview-focus"))return 2;if(open)window.history()->command({{"action","closeDetails"}},[](Error,QJsonObject){});list->setFocus();}
 if(QGuiApplication::platformName()=="offscreen"){auto* header=window.findChild<QWidget*>("windowHeader");const QPoint at(header->width()/2,header->height()-4);QContextMenuEvent event(QContextMenuEvent::Mouse,at,header->mapToGlobal(at));app.sendEvent(header,&event);auto* menu=header->findChild<QMenu*>("windowContextMenu");if(!menu)return 2;putgd::test::settle(40);QPixmap shot=steadyGrab(window);{QPainter painter(&shot);painter.drawPixmap(window.mapFromGlobal(menu->mapToGlobal(QPoint())),steadyGrab(*menu));}if(!shot.save(QDir(output).filePath("settings-header-context-menu.png")))return 2;menu->close();}

 list->selectionModel()->select(list->model()->index(1,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(1,0));{auto* inspector=window.findChild<QScrollArea*>("historyInspectorScroll");if(!waitUntil([&]{return window.findChild<QLabel*>("historyTitle")->text().startsWith("Quarterly");}))return 2;putgd::test::settle(50);inspector->verticalScrollBar()->setValue(inspector->verticalScrollBar()->maximum());if(!save("52-history-long-inspector"))return 2;inspector->verticalScrollBar()->setValue(0);}list->selectionModel()->select(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(0,0));
 list->selectionModel()->select(list->model()->index(2,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(2,0));if(!save("54-history-local-copy-guidance"))return 2;list->selectionModel()->select(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(0,0));
 window.findChild<QToolButton*>("historyGridMode")->click();if(!save("02-history-gallery"))return 2;{list->selectionModel()->select(list->model()->index(3,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(3,0));list->scrollTo(list->currentIndex());if(!save("02a-history-gallery-retry"))return 2;auto* retry=window.findChild<QToolButton*>("historyRowPrimary");window.activateWindow();list->setFocus(Qt::TabFocusReason);app.processEvents();QTest::keyClick(list,Qt::Key_Tab);if(!waitUntil([&]{return retry->hasFocus();})||!save("02b-history-gallery-retry-focus"))return 2;list->selectionModel()->select(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(0,0));list->scrollToTop();list->setFocus();}window.history()->command({{"action","openDetails"}},[](Error,QJsonObject){});if(!save("55-history-gallery-details"))return 2;window.history()->command({{"action","closeDetails"}},[](Error,QJsonObject){});if(!save("56-history-gallery-return"))return 2;window.findChild<QToolButton*>("historyListMode")->click();
 // History states of the 2026-10-26 design pass: active filters while collapsed,
 // a multi-selection strip, the remote-deletion confirmation and a search
 // with no match. Each returns the page to its first state.
 {auto* page=window.history();auto* failedOnly=window.findChild<QListWidget*>("historyCategories");if(!page||!failedOnly)return 2;failedOnly->setCurrentRow(5);if(!waitUntil([&]{return list->model()->rowCount()<8&&!page->historyState()["busy"].toBool();})||!save("40-history-filters-active"))return 2;
  list->selectionModel()->select(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(0,0));if(!save("50-history-failure-recovery"))return 2;page->command({{"action","openDetails"}},[](Error,QJsonObject){});if(!save("50a-history-failure-inspector"))return 2;page->command({{"action","closeDetails"}},[](Error,QJsonObject){});window.findChild<QToolButton*>("historyFiltersExpanded")->setChecked(true);if(!save("history-failed-filters-locked"))return 2;window.findChild<QToolButton*>("historyFiltersExpanded")->setChecked(false);
  {HistoryWindow compact(fixture.session);compact.resize(Ui::scaled(Ui::Metric::historySidebar)+Ui::scaled(400)+160,Ui::scaled(480));compact.show();compact.findChild<QListWidget*>("historyCategories")->setCurrentRow(5);auto* rows=compact.findChild<QListView*>("historyList");if(!waitUntil([&]{return rows->model()->rowCount()==1;}))return 2;rows->selectionModel()->select(rows->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);rows->setCurrentIndex(rows->model()->index(0,0));putgd::test::settle(100);const auto image=steadyGrab(compact);if(!image.save(QDir(output).filePath("50b-history-failure-compact.png")))return 2;captures.append(QJsonObject{{"file","50b-history-failure-compact.png"},{"width",image.width()},{"height",image.height()}});compact.command({{"action","openDetails"}},[](Error,QJsonObject){});putgd::test::settle(100);const auto details=steadyGrab(compact);if(!details.save(QDir(output).filePath("50c-history-failure-compact-inspector.png")))return 2;captures.append(QJsonObject{{"file","50c-history-failure-compact-inspector.png"},{"width",details.width()},{"height",details.height()}});}
  failedOnly->setCurrentRow(0);if(!waitUntil([&]{return list->model()->rowCount()==8;}))return 2;
  window.findChild<QToolButton*>("historyFiltersExpanded")->setChecked(true);if(!save("45-history-filters-expanded"))return 2;window.findChild<QAction*>("historyAddFilterremoteStatus")->trigger();{auto* remote=window.findChild<QComboBox*>("historyRemoteState");if(!remote)return 2;auto* scroll=window.findChild<QScrollArea*>("historyFiltersScroll");scroll->ensureWidgetVisible(remote);remote->showPopup();putgd::test::settle(80);auto* popup=remote->view()->window();QPixmap shot=steadyGrab(window);{QPainter painter(&shot);painter.drawPixmap(window.mapFromGlobal(popup->mapToGlobal(QPoint())),steadyGrab(*popup));}if(!shot.save(QDir(output).filePath("history-remote-groups.png")))return 2;captures.append(QJsonObject{{"file","history-remote-groups.png"},{"width",shot.width()},{"height",shot.height()}});remote->hidePopup();scroll->verticalScrollBar()->setValue(0);}for(const auto& key:{"from","to","mime","exactRemoteStatus"})window.findChild<QAction*>(QString("historyAddFilter")+key)->trigger();{auto* scroll=window.findChild<QScrollArea*>("historyFiltersScroll");putgd::test::settle(50);scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());if(!save("62-history-technical-filters"))return 2;scroll->verticalScrollBar()->setValue(0);}window.findChild<QToolButton*>("historyTechnicalFiltersExpanded")->setChecked(false);window.findChild<QToolButton*>("historyFiltersExpanded")->setChecked(false);
  list->selectionModel()->select(QItemSelection(list->model()->index(1,0),list->model()->index(3,0)),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);if(!save("41-history-selection"))return 2;{const auto previous=window.size();window.resize(Ui::scaled(1800),960);if(!save("41a-history-selection-inspector"))return 2;window.resize(previous);}
  // The confirmation for two of the preview's uploads, as delete.prepare answers.
  QJsonArray targets;QHash<QString,QString> names;const auto model=qobject_cast<HistoryModel*>(list->model());
  for(int row:{1,2}){const auto file=model->record(row)["records"].toArray().first().toObject();names[file["id"].toString()]=file["name"].toString();auto receipt=file["receipt"].toObject();receipt["objectReference"]=QJsonObject{{"schemaVersion",1},{"key","screenshots/"+file["name"].toString()},{"versionId",QString("3HL4kqtJlcpXroDTDmJ%1rP0Ae").arg(row)},{"sha256",QString(64,'a')},{"bytes",36000}};
   targets.append(QJsonObject{{"historyId",file["id"]},{"receipt",receipt},{"effect","Deletes exactly this object version. Older versions and other objects in the bucket remain."}});}
  const auto beforeConfirmation=window.size();window.resize(1060,680);QPointer<QDialog> confirmation=HistoryWindow::deletionConfirmation(&window,{{"targets",targets},{"acknowledgement",QString(64,'0')},{"localCopiesPreserved",true},{"cacheNotice","Origin deletion does not invalidate browser or CDN caches"}},names,fixture.session.document()["profiles"].toArray(),1);
  confirmation->show();putgd::test::settle(450);app.processEvents();{if(!saveDialog(output,"42-history-delete-confirmation",*confirmation,captures))return 2;}delete confirmation.data();window.resize(beforeConfirmation);
  // Proton deletion is intentionally unsupported. A separate S3 receipt set
  // exercises the production dialog's affirmative action without deleting it.
  QJsonArray supportedTargets;const auto s3=config["profiles"].toArray().last().toObject();
  const Destination s3Destination{"s3","synthetic-archive",settings.revision(),s3["settings"].toObject(),s3["credentials"].toObject()};
  for(const auto& value:targets){auto target=value.toObject();auto receipt=target["receipt"].toObject();receipt["destination"]=s3Destination.toJson();receipt["directUrl"]=QJsonValue::Null;target["receipt"]=receipt;supportedTargets.append(target);}
  confirmation=HistoryWindow::deletionConfirmation(&window,{{"targets",supportedTargets}},names,config["profiles"].toArray());confirmation->show();putgd::test::settle(100);
  if(!confirmation->findChild<QPushButton*>("confirmActionButton")||!saveDialog(output,"confirm-history-delete-supported",*confirmation,captures))return 2;delete confirmation.data();
  {auto single=supportedTargets.first().toObject();auto receipt=single["receipt"].toObject();receipt["destination"]=QJsonObject{{"providerId","opaque"},{"profileId","synthetic-archive"}};receipt["objectReference"]=QJsonObject{{"opaqueIdentity","synthetic-target-token"}};single["receipt"]=receipt;single["effect"]="Removes the exact recorded file. Local copies and history remain.";
   confirmation=HistoryWindow::deletionConfirmation(&window,{{"targets",QJsonArray{single}}},names,config["profiles"].toArray());confirmation->show();if(!saveDialog(output,"confirm-history-delete-opaque-single",*confirmation,captures))return 2;delete confirmation.data();}
  confirmation=HistoryWindow::deletionConfirmation(&window,{{"targets",targets}},names,fixture.session.document()["profiles"].toArray());confirmation->setMaximumHeight(640);confirmation->resize(confirmation->width(),qMin(640,confirmation->height()));confirmation->findChild<QToolButton*>("deleteConfirmationDetailsExpanded")->setChecked(true);confirmation->show();putgd::test::settle(100);auto* targetScroll=confirmation->findChild<QScrollArea*>("deleteConfirmationTargets");targetScroll->verticalScrollBar()->setValue(targetScroll->verticalScrollBar()->maximum());putgd::test::settle(50);const auto technicalShot=confirmation->findChild<QPlainTextEdit*>("deleteConfirmationDetails")->window()->grab();if(!technicalShot.save(QDir(output).filePath("49-history-delete-details.png")))return 2;captures.append(QJsonObject{{"file","49-history-delete-details.png"},{"width",technicalShot.width()},{"height",technicalShot.height()}});delete confirmation.data();
  list->clearSelection();auto* search=window.findChild<QLineEdit*>("historySearch");fixture.client.replyDelayMs["history.list"]=1200;search->setText("quarterly budget");QMetaObject::invokeMethod(search,"returnPressed");if(!save("47-history-filtering"))return 2;if(!waitUntil([&]{return !page->historyState()["busy"].toBool();}))return 2;fixture.client.replyDelayMs.remove("history.list");search->setText("quarterly budget");QMetaObject::invokeMethod(search,"returnPressed");if(!waitUntil([&]{return list->model()->rowCount()==0&&!page->historyState()["busy"].toBool();})||!save("43-history-no-matches"))return 2;
  window.findChild<QPushButton*>("historyResetView")->click();if(!waitUntil([&]{return list->model()->rowCount()==8;}))return 2;
  fixture.client.failOnce["history.list"]={"synthetic_load","Couldn't read the history database on this computer.",{}};search->setText("Screenshot");QMetaObject::invokeMethod(search,"returnPressed");if(!waitUntil([&]{return window.findChild<QPushButton*>("historyRetryLoading")->isVisible();})||!save("51-history-loading-failure"))return 2;window.findChild<QPushButton*>("historyRetryLoading")->click();if(!waitUntil([&]{return list->model()->rowCount()>0&&!page->historyState()["busy"].toBool();}))return 2;search->clear();QMetaObject::invokeMethod(search,"returnPressed");if(!waitUntil([&]{return list->model()->rowCount()==8&&!page->historyState()["busy"].toBool();}))return 2;
  const auto file=model->record(0)["records"].toArray().first().toObject();QJsonObject deletion{{"id","synthetic-preview-deletion"},{"state","completed"},{"items",QJsonArray{QJsonObject{{"historyId",file["id"]},{"receipt",file["receipt"]},{"status","failed"},{"error",QJsonObject{{"code","synthetic_failure"},{"message","Connection interrupted. Retry deletion when the destination is available."}}}}}}};{HistoryStore ledger(operations.databasePath());if(ledger.saveDeletion(deletion))return 2;}page->refresh();if(!waitUntil([&]{return model->record(0)["deleteOperationId"]=="synthetic-preview-deletion";}))return 2;
  list->selectionModel()->select(list->model()->index(0,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(list->model()->index(0,0));if(!waitUntil([&]{return !window.findChild<QPushButton*>("retryHistoryDeletion")->isHidden();}))return 2;auto* inspector=window.findChild<QScrollArea*>("historyInspectorScroll");inspector->verticalScrollBar()->setValue(0);if(!save("48-history-deletion-recovery"))return 2;
  auto settled=deletion["items"].toArray().first().toObject();settled["status"]="present";settled["error"]=QJsonObject{};deletion["items"]=QJsonArray{settled};{HistoryStore ledger(operations.databasePath());if(ledger.saveDeletion(deletion))return 2;}page->refresh();if(!waitUntil([&]{return window.findChild<QPushButton*>("retryHistoryDeletion")->isHidden();}))return 2;inspector->verticalScrollBar()->setValue(0);
 }window.navigate("destinations");auto* destinations=window.findChild<QListWidget*>("destinationList");if(!destinations)return 2;destinations->setCurrentRow(0);if(!waitUntil([&]{return bool(loadFolders);}))return 2;loadFolders({},{{"accountRootUid","synthetic-root"},{"accountEmail","designer@example.invalid"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","put.gd"}}}}});if(!waitUntil([&]{return window.findChild<QLabel*>("protonAccountStatus")->text().contains("designer@example.invalid");}))return 2;if(!save("03-destinations-proton"))return 2;{const auto size=window.size();auto* scroll=window.findChild<QScrollArea*>("destinationScroll");for(const bool compact:{false,true}){if(compact)window.resize(1060,680);reveal(scroll,window.findChild<QWidget*>("protonKeepPrivate"),40);if(!save(compact?"settings-radio-choices-compact":"settings-radio-choices"))return 2;}window.resize(size);scroll->verticalScrollBar()->setValue(0);}window.findChild<QPushButton*>("inspectProtonAccount")->click();if(!waitUntil([&]{return window.findChild<QLabel*>("protonFolderStatus")->isVisible();})||!save("16-proton-loading-folders"))return 2;loadFolders({},{{"accountRootUid","synthetic-root"},{"folders",QJsonArray{QJsonObject{{"uid","synthetic-folder"},{"name","put.gd"}}}}});if(!waitUntil([&]{return !window.findChild<QLabel*>("protonFolderStatus")->isVisible();}))return 2;destinations->setCurrentRow(1);if(!save("04-destinations-s3"))return 2;
 {const auto size=window.size();auto* action=window.findChild<QPushButton*>("addDestinationCredentials");if(action&&action->isVisible()){window.resize(1060,680);window.activateWindow();action->click();if(!waitUntil([&]{return window.findChild<QLineEdit*>("credentialAccessKey")->hasFocus();})||!save("settings-add-credentials-focus"))return 2;window.resize(size);window.findChild<QScrollArea*>("destinationScroll")->verticalScrollBar()->setValue(0);}}
 window.navigate("settings");auto* tabs=window.findChild<QTabWidget*>("settingsTabs");if(!tabs)return 2;tabs->setCurrentIndex(3);if(!save("05-settings-advanced"))return 2;{auto* support=window.findChild<QToolButton*>("desktopSupportDetailsToggle");support->setChecked(true);auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());putgd::test::settle(50);scroll->ensureWidgetVisible(window.findChild<QWidget*>("desktopSupportDetails"),0,20);if(!save("settings-desktop-support-expanded"))return 2;support->setChecked(false);scroll->verticalScrollBar()->setValue(0);}window.findChild<QToolButton*>("resourceLimitsToggle")->setChecked(true);if(!save("settings-resource-limits-expanded"))return 2;{auto* help=window.findChild<QToolButton*>("historyRetentionChoiceHelp");if(!help)return 2;help->setChecked(true);auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());putgd::test::settle(50);scroll->ensureWidgetVisible(window.findChild<QLabel*>("historyRetentionChoiceHelpText"),0,20);if(!save("settings-retention-help"))return 2;help->setChecked(false);scroll->verticalScrollBar()->setValue(0);}window.findChild<QToolButton*>("resourceLimitsToggle")->setChecked(false);
 {auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());auto* retention=window.findChild<QLineEdit*>("historyRetentionDays");putgd::test::settle(100);scroll->ensureWidgetVisible(retention,0,100);if(!save("settings-forever-retention"))return 2;scroll->verticalScrollBar()->setValue(0);auto* limit=window.findChild<QLineEdit*>("maxFileBytes");const auto before=limit->text();limit->setText("3");if(!save("settings-numeric-limit-unit"))return 2;limit->setText(before);}
 tabs->setCurrentIndex(2);if(!save("06-settings-shortcuts"))return 2;tabs->setCurrentIndex(0);if(!save("07-settings-general"))return 2;tabs->setCurrentIndex(1);if(!save("08-settings-capture"))return 2;window.findChild<QToolButton*>("customFileNamePatternToggle")->setChecked(true);{auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());putgd::test::settle(40);scroll->ensureWidgetVisible(window.findChild<QWidget*>("customFileNamePattern"),0,24);if(!save("settings-custom-name-pattern"))return 2;window.findChild<QToolButton*>("customFileNamePatternToggle")->setChecked(false);scroll->verticalScrollBar()->setValue(0);}
// #984: the Screen recording section's frame rate, below Region selection.
{auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());putgd::test::settle(40);scroll->ensureWidgetVisible(window.findChild<QWidget*>("recordingFrameRate"),0,160);if(!save("settings-recording-frame-rate"))return 2;scroll->verticalScrollBar()->setValue(0);}
 window.navigate("destinations");destinations->setCurrentRow(1);window.findChild<QLineEdit*>("credentialAccessKey")->setText("EXAM-synthetic-preview-access");window.findChild<QLineEdit*>("credentialSecretKey")->setText("synthetic-preview-secret");window.findChild<QPushButton*>("storeCredentials")->click();if(!waitUntil([&]{return window.findChild<QLabel*>("storedCredentialHint")->text().contains("EXAM");}))return 2;auto* destinationScroll=window.findChild<QScrollArea*>("destinationScroll");destinationScroll->verticalScrollBar()->setValue(destinationScroll->verticalScrollBar()->maximum());if(!save("09-stored-credential-hint"))return 2;
 window.resize(1060,680);window.navigate("history");if(!save("10-history-compact")||window.width()!=1060||window.height()!=680)return 2;{window.history()->command({{"action","openDetails"}},[](Error,QJsonObject){});window.activateWindow();putgd::test::settle(30);auto* focusedPreview=window.findChild<QLabel*>("historyPreview");focusedPreview->setFocus(Qt::TabFocusReason);if(!waitUntil([&]{return focusedPreview->hasFocus();}))return 2;if(!save("10a-history-compact-preview-focus"))return 2;window.history()->command({{"action","closeDetails"}},[](Error,QJsonObject){});}window.findChild<QToolButton*>("historyManagementExpanded")->setChecked(true);if(!save("46-history-maintenance-compact"))return 2;{auto* menu=window.findChild<QMenu*>("historyManagementMenu");if(!menu||!menu->isVisible())return 2;const auto image=steadyGrab(*menu);if(!image.save(QDir(output).filePath("57-history-management-menu.png")))return 2;captures.append(QJsonObject{{"file","57-history-management-menu.png"},{"width",image.width()},{"height",image.height()}});}window.findChild<QToolButton*>("historyManagementExpanded")->setChecked(false);
 // Actual SettingsWindow actions build these dialogs; no copied dialog text.
 window.navigate("destinations");destinations->setCurrentRow(1);
 auto captureQuestion=[&](const QString& name,const std::function<void()>& open){
  bool captured=false;QTimer timer;timer.setSingleShot(true);QObject::connect(&timer,&QTimer::timeout,&app,[&]{
   if(auto* dialog=qobject_cast<QDialog*>(QApplication::activeModalWidget())){captured=saveDialog(output,name,*dialog,captures);dialog->reject();}
  });timer.start(100);open();if(!captured)qCritical("Confirmation capture did not open: %s",qPrintable(name));return captured;
 };
 if(!captureQuestion("confirm-make-default-untested",[&]{window.findChild<QPushButton*>("makeDefaultDestination")->click();})||
    !captureQuestion("confirm-remove-destination",[&]{window.findChild<QPushButton*>("removeDestination")->click();})||
    !captureQuestion("confirm-add-destination-name",[&]{auto* add=window.findChild<QPushButton*>("addS3Destination");add->menu()->actions().first()->trigger();}))return 2;
 destinations->setCurrentRow(0);
 if(!captureQuestion("confirm-new-proton-folder",[&]{window.findChild<QPushButton*>("createProtonFolder")->click();}))return 2;
 destinations->setCurrentRow(1);
 window.navigate("settings");tabs->setCurrentIndex(3);
 if(!captureQuestion("confirm-reset-limits",[&]{window.findChild<QPushButton*>("resetAdvancedDefaults")->click();})||
    !captureQuestion("confirm-clear-logs",[&]{window.findChild<QPushButton*>("clearLogs")->click();}))return 2;
 const auto previousUpdate=catalogUpdate;
 catalogUpdate={{"supported",true},{"build",QJsonObject{{"version","26.1029.1"},{"channel","nightly"}}},
                {"install",QJsonObject{{"available",true},{"image","/tmp/synthetic-putgd.AppImage"},
                  {"switch",QJsonObject{{"to","stable"},{"version","26.1028.1"},{"older",true}}}}}};
 fixture.session.refreshSnapshot();
 if(!waitUntil([&]{return fixture.session.diagnostics()["update"].toObject()==catalogUpdate;})||
    !captureQuestion("confirm-switch-older",[&]{window.findChild<QPushButton*>("installUpdate")->click();}))return 2;
 catalogUpdate=previousUpdate;fixture.session.refreshSnapshot();
 window.navigate("history");
 auto* management=window.history()->findChild<QMenu*>("historyManagementMenu");
 if(!management||!captureQuestion("confirm-clear-history",[&]{management->actions()[0]->trigger();})||
    !captureQuestion("confirm-clear-cache",[&]{management->actions()[1]->trigger();}))return 2;
 // Revoke asks before dispatching any request, even in this offline fixture.
 if(!captureQuestion("confirm-revoke-link",[&]{window.findChild<QPushButton*>("revokeHistoryLink")->click();}))return 2;
 // Below the side-by-side breakpoint (150% text) the inspector opens over the list.
 {bool opened=false;window.history()->command({{"action","openDetails"}},[&](Error,QJsonObject state){opened=state["detailsOpen"].toBool();});if(opened&&!save("44-history-compact-details"))return 2;window.history()->command({{"action","closeDetails"}},[](Error,QJsonObject){});}
 window.findChild<QToolButton*>("historyGridMode")->click();if(!save("history-gallery-compact"))return 2;window.history()->command({{"action","openDetails"}},[](Error,QJsonObject){});if(!save("67-history-gallery-compact-details"))return 2;
 {auto* page=window.history();if(!waitUntil([&]{return page->historyState()["canExpandPreview"].toBool();}))return 2;page->command({{"action","expandPreview"}},[](Error,QJsonObject){});auto* dialog=page->findChild<QDialog*>("historyExpandedPreview");if(!dialog)return 2;putgd::test::settle(100);const auto image=steadyGrab(*dialog);if(!image.save(QDir(output).filePath("68-history-expanded-preview.png")))return 2;captures.append(QJsonObject{{"file","68-history-expanded-preview.png"},{"width",image.width()},{"height",image.height()}});page->command({{"action","closePreview"}},[](Error,QJsonObject){});}
 {const auto before=window.size();window.resize(1060,900);if(!save("70-history-taller-details"))return 2;window.resize(before);}
 {const auto before=window.size(),minimum=window.minimumSize();window.setMinimumHeight(480);window.resize(1060,480);if(!save("71-history-thumbnail-details"))return 2;window.setMinimumSize(minimum);window.resize(before);}
 window.history()->command({{"action","closeDetails"}},[](Error,QJsonObject){});window.findChild<QToolButton*>("historyListMode")->click();
 {const auto size=window.size();window.resize(1440,960);auto* page=window.history();auto* model=qobject_cast<HistoryModel*>(list->model());{HistoryStore ledger(operations.databasePath());for(int i=0;i<2;++i){Operation op;op.id=QString("synthetic-history-kind-%1").arg(i);op.created=now.addSecs(i).toString(Qt::ISODateWithMs);op.action=i?"record":"clipboard";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id=op.id+"-file";item.artifact.name=i?"Product walkthrough.mp4":"Release notes.txt";item.artifact.mime=i?"video/mp4":"text/plain";item.artifact.category=i?"videos":"text";item.artifact.bytes=4096;item.local.status="disabled";item.uploadStatus="uploaded";item.shareStatus="revoked";item.receipt.directUrl="https://example.invalid/previous-link";op.items.append(item);if(ledger.saveOperation(op))return 2;}}page->refresh();if(!waitUntil([&]{return model->rowCount()==10;}))return 2;for(int i=0;i<2;++i){int row=-1;for(int r=0;r<model->rowCount();++r)if(model->record(r)["operationId"]==QString("synthetic-history-kind-%1").arg(i))row=r;if(row<0)return 2;if(i)fixture.client.failOnce["history.preview"]={"preview_failed","The recording poster could not be decoded.",{}};list->selectionModel()->select(model->index(row),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(model->index(row));if(page->historyState()["layout"]=="compact")page->command({{"action","openDetails"}},[](Error,QJsonObject){});if(i){auto* retry=window.findChild<QPushButton*>("retryHistoryPreview");if(!waitUntil([&]{return retry->isVisible();})||!save("66-history-preview-failed"))return 2;retry->click();if(!waitUntil([&]{return window.findChild<QLabel*>("historyPreview")->text()=="Local original unavailable";}))return 2;}if(!save(i?"59-history-recording-no-copy":"58-history-text-unavailable-link"))return 2;}window.findChild<QToolButton*>("historyReceiptExpanded")->setChecked(true);auto* inspector=window.findChild<QScrollArea*>("historyInspectorScroll");putgd::test::settle(50);inspector->verticalScrollBar()->setValue(inspector->verticalScrollBar()->maximum());if(!save("60-history-previous-link-details"))return 2;window.findChild<QToolButton*>("historyReceiptExpanded")->setChecked(false);{HistoryStore ledger(operations.databasePath());Error error;auto op=ledger.operation("synthetic-history-kind-1",&error);if(error)return 2;op.items[0].shareStatus="shared";op.items[0].receipt.directUrl="unsupported://synthetic-preview";if(ledger.saveOperation(op))return 2;}page->refresh();auto* open=window.findChild<QPushButton*>("openHistoryLink");if(!waitUntil([&]{return open->isVisible()&&open->isEnabled();}))return 2;open->click();inspector->verticalScrollBar()->setValue(0);if(!save("61-history-browser-failure"))return 2;
  for(const auto& state:QStringList{"present","removed","local_only"}){
   const auto operationId="synthetic-missing-copy-"+state;const auto rows=model->rowCount();
   {HistoryStore ledger(operations.databasePath());Error error;auto op=ledger.operation("synthetic-history-kind-1",&error);if(error)return 2;op.id=operationId;op.created=now.addSecs(rows).toString(Qt::ISODateWithMs);op.items[0].artifact.id=operationId+"-file";op.items[0].remoteStatus=state=="local_only"?"not_requested":state;op.items[0].uploadStatus=state=="local_only"?"local_only":"uploaded";op.items[0].shareStatus=state=="local_only"?"not_requested":"shared";op.items[0].receipt.directUrl=state=="local_only"?QString{}:"https://example.invalid/synthetic-recording";if(ledger.saveOperation(op))return 2;}
   page->refresh();if(!waitUntil([&]{return model->rowCount()==rows+1;}))return 2;int row=-1;for(int r=0;r<model->rowCount();++r)if(model->record(r)["operationId"]==operationId)row=r;if(row<0)return 2;list->selectionModel()->select(model->index(row),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(model->index(row));const auto marker=state=="present"?QString("download the uploaded"):state=="removed"?QString("was deleted from"):QString("No successful upload is recorded");// The preview request starts 400 ms after the selection; wait past it for its outcome.
   auto* missingPreview=window.findChild<QLabel*>("historyPreview");const auto previewSettled=[&]{return window.findChild<QLabel*>("historyCapabilityHint")->text().contains(marker)&&(!missingPreview->isVisible()||missingPreview->text()!=QStringLiteral("Making a preview…"));};if(!waitUntil(previewSettled))return 2;putgd::test::settle(700);if(!waitUntil(previewSettled))return 2;inspector->verticalScrollBar()->setValue(0);if(!save("history-missing-copy-"+state))return 2;
  }
  window.resize(size);}
 {auto* page=window.history();auto* model=qobject_cast<HistoryModel*>(list->model());int row=-1;for(int r=0;r<model->rowCount();++r)if(model->record(r)["operationId"]=="synthetic-preview-operation-0")row=r;if(row<0)return 2;list->selectionModel()->select(model->index(row),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->setCurrentIndex(model->index(row));if(!waitUntil([&]{return window.findChild<QPushButton*>("saveHistoryAs")->isEnabled();}))return 2;
  const auto previousDialog=qgetenv("PUTGD_FILE_DIALOG");qputenv("PUTGD_FILE_DIALOG","fallback");page->saveAsDialogCommand("open",{},[](Error,QJsonObject){});QFileDialog* chooser=nullptr;if(!waitUntil([&]{return (chooser=page->saveAsDialog().fallbackDialog())&&chooser->isVisible();}))return 2;auto copyReleased=std::make_shared<std::atomic_bool>(false);auto releaseCopy=qScopeGuard([copyReleased]{copyReleased->store(true);});fixture.session.background([copyReleased](Error&){while(!copyReleased->load())std::this_thread::sleep_for(std::chrono::milliseconds(1));return QJsonObject{};},page,[](Error,QJsonObject){});chooser->selectFile(data.filePath("Design review copy.png"));QMetaObject::invokeMethod(chooser,"accept");if(!waitUntil([&]{return page->historyState()["copyJob"].toObject()["status"]=="saving";})||!save("69a-history-saving-copy"))return 2;copyReleased->store(true);if(!waitUntil([&]{return !page->historyState()["savedPath"].toString().isEmpty();})||!save("69-history-saved-copy"))return 2;page->command({{"action","dismissSavedCopy"}},[](Error,QJsonObject){});if(!save("69b-history-dismissed-copy"))return 2;if(previousDialog.isNull())qunsetenv("PUTGD_FILE_DIALOG");else qputenv("PUTGD_FILE_DIALOG",previousDialog);
 }

 window.navigate("destinations");{auto* provider=window.findChild<QComboBox*>("storageProvider");provider->setCurrentIndex(provider->findData("backblaze-b2"));putgd::test::settle(100);destinationScroll->verticalScrollBar()->setValue(0);if(!save("settings-b2-compact"))return 2;auto* mode=window.findChild<QComboBox*>("b2LinkMode");mode->setCurrentIndex(mode->findData("authorized"));putgd::test::settle(100);destinationScroll->ensureWidgetVisible(window.findChild<QWidget*>("b2LinkDaysHint"),0,40);if(!save("settings-b2-stepper-1060"))return 2;mode->setCurrentIndex(mode->findData("public"));provider->setCurrentIndex(provider->findData("s3"));}
 window.navigate("settings");tabs->setCurrentIndex(0);if(!save("settings-general-compact"))return 2;window.findChild<QToolButton*>("updatePrivacyDetails")->setChecked(true);if(auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget()))scroll->ensureWidgetVisible(window.findChild<QWidget*>("updatePrivacyFields"),0,40);if(!save("settings-update-privacy"))return 2;window.findChild<QToolButton*>("updatePrivacyDetails")->setChecked(false);tabs->setCurrentIndex(2);if(!save("settings-shortcuts-compact"))return 2;tabs->setCurrentIndex(3);if(!save("11-advanced-compact"))return 2;window.navigate("destinations");destinations->setCurrentRow(0);destinationScroll->verticalScrollBar()->setValue(0);if(!save("12-proton-compact"))return 2;{auto* advanced=window.findChild<QToolButton*>("advancedSettings");advanced->setChecked(true);putgd::test::settle(100);destinationScroll->ensureWidgetVisible(window.findChild<QWidget*>("protonRequestTimeout"),0,40);if(!save("settings-proton-steppers-1060"))return 2;advanced->setChecked(false);}destinations->setCurrentRow(1);{auto* label=window.findChild<QLineEdit*>("destinationLabel");if(!label)return 2;const auto original=label->text();label->selectAll();label->insert(QString("Production screenshots for customer support — ").repeated(3).left(120));destinationScroll->verticalScrollBar()->setValue(0);if(!save("17-destination-long-name-compact"))return 2;destinationScroll->ensureWidgetVisible(window.findChild<QWidget*>("credentialActions"),0,40);if(!save("18-destination-credentials-compact"))return 2;label->selectAll();label->insert(original);destinationScroll->verticalScrollBar()->setValue(0);}
 window.navigate("settings");tabs->setCurrentIndex(1);if(!save("13-capture-compact"))return 2;window.findChild<QToolButton*>("categoryFoldersToggle")->setChecked(true);putgd::test::settle(100);if(auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget()))scroll->ensureWidgetVisible(window.findChild<QWidget*>("folderOther"),0,24);if(!save("settings-category-folders"))return 2;window.findChild<QToolButton*>("categoryFoldersToggle")->setChecked(false);if(auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget()))scroll->verticalScrollBar()->setValue(0);window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);if(!save("14-capture-exceptions-compact"))return 2;{auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());putgd::test::settle(100);scroll->ensureWidgetVisible(window.findChild<QWidget*>("captureRowRegion"),0,80);if(!save("settings-exceptions-compact"))return 2;scroll->verticalScrollBar()->setValue(0);}
 window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(false);auto* destination=window.findChild<QComboBox*>("defaultCaptureDestination");destination->setCurrentIndex(destination->findData(QString{}));window.findChild<QCheckBox*>("keepUploadCopies")->setChecked(false);if(!waitUntil([&]{return !settings.snapshot().archive.keepUploads&&settings.snapshot().actions["region"].destination=="local";}))return 2;if(!save("15-capture-no-output"))return 2;for(auto* button:window.findChildren<QToolButton*>("captureActionButton"))if(button->property("captureAction")=="region")button->click();if(!save("main-unavailable-recovery"))return 2;window.findChild<QToolButton*>("dismissActionRecovery")->click();
 {auto* menu=window.findChild<QMenu*>("editSourceMenu");menu->popup(window.findChild<QWidget*>("commandBand")->mapToGlobal(QPoint(220,50)));putgd::test::settle(100);const auto image=steadyGrab(*menu);if(!image.save(QDir(output).filePath("history-edit-sources-no-output.png")))return 2;captures.append(QJsonObject{{"file","history-edit-sources-no-output.png"},{"width",image.width()},{"height",image.height()}});menu->close();}
 // Locked values: the inherited per-action outputs, scrolled into view.
 window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);if(auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget()))if(auto* region=window.findChild<QComboBox*>("profileRegion")){putgd::test::settle(100);scroll->ensureWidgetVisible(region,0,200);}if(!save("50-capture-exceptions-locked"))return 2;window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(false);
 // Settings review: replacement storage, setup admission and deep-link focus.
 window.navigate("destinations");destinations->setCurrentRow(1);{auto* mode=window.findChild<QComboBox*>("credentialMode");mode->setCurrentIndex(mode->findData("keychain"));if(!waitUntil([&]{return settings.document()["profiles"].toArray()[1].toObject()["credentials"].toObject()["mode"]=="keychain";}))return 2;reveal(destinationScroll,window.findChild<QWidget*>("credentialActions"),40);if(!save("60-credential-storage-replacement"))return 2;
  fixture.client.replyDelayMs["settings.save"]=2500;auto* name=window.findChild<QLineEdit*>("destinationLabel");const auto original=name->text();name->setText("Project archive pending save");window.settings()->flushAutosave();reveal(destinationScroll,window.findChild<QWidget*>("credentialStoreReason"),16);if(!save("61-credential-store-pending"))return 2;if(!waitUntil([&]{return window.findChild<QPushButton*>("storeCredentials")->isEnabled();}))return 2;fixture.client.replyDelayMs.remove("settings.save");name->setText(original);mode->setCurrentIndex(mode->findData("session"));if(!waitUntil([&]{return window.findChild<QPushButton*>("storeCredentials")->isEnabled();}))return 2;}
 window.findChild<QToolButton*>("advancedSettings")->setChecked(true);reveal(destinationScroll,window.findChild<QWidget*>("s3LowSpeedBytesPerSecondControl"),40);if(!save("62-destination-connection-expanded"))return 2;destinationScroll->ensureWidgetVisible(window.findChild<QWidget*>("s3EndpointValue"),0,40);if(!save("settings-endpoint-advanced"))return 2;window.findChild<QToolButton*>("advancedSettings")->setChecked(false);
 window.activateWindow();window.showLocalCopySettings();if(!waitUntil([&]{return window.findChild<QCheckBox*>("keepUploadCopies")->hasFocus();})||!save("63-local-copies-focus"))return 2;
 // A conflict: the file changes under an unsaved draft (a typed, unstored key);
 // Reload from file… sits beside the message on both pages.
 window.navigate("destinations");window.findChild<QLineEdit*>("credentialAccessKey")->setText("EXAM-synthetic-conflict");{auto doc=settings.document();auto prefs=doc["preferences"].toObject();prefs["notifications"]=!prefs["notifications"].toBool();doc["preferences"]=prefs;if(!settings.save(doc,settings.revision()))return 2;}
 if(!waitUntil([&]{return window.settings()->saveViewState()["conflict"].toBool();}))return 2;if(!save("52-destinations-conflict"))return 2;putgd::test::settle(100);destinationScroll->ensureWidgetVisible(window.findChild<QWidget*>("credentialActions")->parentWidget(),0,16);if(!save("64-credential-store-conflict"))return 2;
 window.navigate("settings");tabs->setCurrentIndex(0);if(!save("51-settings-conflict"))return 2;
 {const auto size=window.size();catalogUpdate={{"supported",true},{"result",QJsonObject{{"updateAvailable",true},{"latest","26.1231.99999-nightly"}}}};fixture.session.refreshSnapshot();if(!waitUntil([&]{return window.findChild<QPushButton*>("updateAvailable")->isVisible();}))return 2;window.resize(1060,680);putgd::test::settle(100);
  // A user scroll to the top, which also ends Settings' "reveal Updates" follow. The
  // page header must use its current compact height after the resize (#778).
  qobject_cast<QScrollArea*>(tabs->currentWidget())->verticalScrollBar()->triggerAction(QAbstractSlider::SliderToMinimum);if(!save("51a-settings-conflict-compact-update"))return 2;window.resize(size);catalogUpdate={};fixture.session.refreshSnapshot();if(!waitUntil([&]{return window.findChild<QPushButton*>("updateAvailable")->isHidden();}))return 2;}
 {bool confirmed=false;QTimer::singleShot(500,&app,[&]{for(auto* widget:QApplication::topLevelWidgets())if(auto* box=qobject_cast<QMessageBox*>(widget);box&&box->isVisible()){confirmed=saveDialog(output,"53-reload-confirm",*box,captures);box->button(QMessageBox::Yes)->click();}});
  window.findChild<QPushButton*>("reloadConflict")->click();if(!confirmed||!waitUntil([&]{return !window.settings()->saveViewState()["conflict"].toBool();}))return 2;}
 // The 2026-10-26 settings pass: an overridden action among compact rows,
 // chooser failures beside their actions, the status-band summary's full text
 // and the diagnostics package dialog.
 {window.navigate("settings");tabs->setCurrentIndex(1);auto* override=window.findChild<QCheckBox*>("overrideClipboard");if(!override)return 2;
  // The reload above re-enables and repopulates the page once its reply lands.
  if(!waitUntil([&]{return window.settings()->isEnabled();}))return 2;putgd::test::settle(450);window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(true);override->click();auto* scroll=qobject_cast<QScrollArea*>(tabs->currentWidget());
  if(!waitUntil([&]{return settings.document()["actions"].toObject()["clipboard"].toObject()["destination"]!="default"&&override->isChecked();}))return 2;putgd::test::settle(100);if(scroll)scroll->ensureWidgetVisible(window.findChild<QWidget*>("captureEditorClipboard"),0,40);if(!save("54-capture-exception-override"))return 2;
  auto* regionOverride=window.findChild<QCheckBox*>("overrideRegion");if(!regionOverride->isChecked())regionOverride->click();auto* regionProfile=window.findChild<QComboBox*>("profileRegion");regionProfile->setCurrentIndex(regionProfile->findData("synthetic-proton"));auto* regionSharing=window.findChild<QComboBox*>("publicationRegion");regionSharing->setCurrentIndex(regionSharing->findData("private"));
  if(!waitUntil([&]{return settings.snapshot().actions["record"].publicationMode=="private";}))return 2;if(scroll)scroll->ensureWidgetVisible(window.findChild<QWidget*>("captureRowRecord"),0,40);if(!save("66-recording-region-inheritance"))return 2;
  window.findChild<QCheckBox*>("overrideRecord")->click();auto* recordProfile=window.findChild<QComboBox*>("profileRecord");recordProfile->setCurrentIndex(recordProfile->findData("synthetic-archive"));auto* recordSharing=window.findChild<QComboBox*>("publicationRecord");recordSharing->setCurrentIndex(recordSharing->findData("share"));
  if(!waitUntil([&]{return settings.snapshot().actions["record"].publicationMode=="share"&&settings.snapshot().actions["record"].profileId=="synthetic-archive";}))return 2;if(scroll)scroll->ensureWidgetVisible(window.findChild<QWidget*>("captureEditorRecord"),0,40);if(!save("67-recording-explicit-output"))return 2;
  window.navigate("history");auto* summaryMixed=window.findChild<QToolButton*>("captureOutputSummary");summaryMixed->menu()->popup(summaryMixed->mapToGlobal(QPoint(0,-summaryMixed->menu()->sizeHint().height())));putgd::test::settle(450);
  {QPixmap shot=steadyGrab(window);if(auto* popup=summaryMixed->menu()){QPainter painter(&shot);painter.drawPixmap(window.mapFromGlobal(popup->mapToGlobal(QPoint())),steadyGrab(*popup));}if(!shot.save(QDir(output).filePath("main-mixed-outputs.png")))return 2;captures.append(QJsonObject{{"file","main-mixed-outputs.png"},{"width",shot.width()},{"height",shot.height()}});if(auto* popup=summaryMixed->menu())popup->close();}
  window.findChild<QPushButton*>("chooseCommandOutput")->click();if(!save("main-output-recovery"))return 2;
  window.findChild<QCheckBox*>("overrideRecord")->click();regionProfile->setCurrentIndex(regionProfile->findData(QString{}));regionOverride->click();
  window.findChild<QToolButton*>("captureExceptionsToggle")->setChecked(false);
  const FileDialog::Result failed{FileDialog::Result::Status::Failed,{},"The desktop's file chooser did not answer."},cancelled{};
  window.settings()->folderChooserFinished("outputFolder","images",failed);putgd::test::settle(100);if(scroll)scroll->ensureWidgetVisible(window.findChild<QWidget*>("folderImages"),0,120);if(!save("55-folder-chooser-failed"))return 2;window.settings()->folderChooserFinished("outputFolder","images",cancelled);
  window.imageChooserFinished(failed);if(!save("56-edit-image-chooser-failed"))return 2;window.imageChooserFinished(cancelled);
  // The summary's full text opens from the keyboard in its own popup, over the window.
  window.navigate("history");auto* summary=window.findChild<QToolButton*>("captureOutputSummary");if(!summary)return 2;window.activateWindow();summary->setFocus(Qt::TabFocusReason);summary->menu()->popup(summary->mapToGlobal(QPoint(0,-summary->menu()->sizeHint().height())));putgd::test::settle(450);
  {QPixmap shot=steadyGrab(window);if(auto* popup=summary->menu()){QPainter painter(&shot);painter.drawPixmap(window.mapFromGlobal(popup->mapToGlobal(QPoint())),steadyGrab(*popup));}if(!shot.save(QDir(output).filePath("57-output-summary-full-text.png")))return 2;captures.append(QJsonObject{{"file","57-output-summary-full-text.png"},{"width",shot.width()},{"height",shot.height()}});if(auto* popup=summary->menu())popup->close();}
  bool opened=false;window.settings()->diagnosticsCommand({{"action","openPackage"}},[&](Error,QJsonObject){opened=true;});if(!waitUntil([&]{return opened&&!window.settings()->diagnosticsState()["package"].toObject()["uploadTarget"].toObject()["checking"].toBool();}))return 2;
  if(auto* dialog=window.settings()->findChild<QDialog*>("diagnosticsPackageDialog")){putgd::test::settle(450);app.processEvents();const auto image=steadyGrab(*dialog);if(!image.save(QDir(output).filePath("58-diagnostics-package.png")))return 2;captures.append(QJsonObject{{"file","58-diagnostics-package.png"},{"width",image.width()},{"height",image.height()}});dialog->findChild<QToolButton*>("packageContentsToggle")->setChecked(true);reveal(dialog->findChild<QScrollArea*>("diagnosticsPackageScroll"),dialog->findChild<QLabel*>("packageContents"),20);if(!steadyGrab(*dialog).save(QDir(output).filePath("settings-package-contents-expanded.png")))return 2;dialog->reject();}else return 2;
 }
 window.navigate("destinations");destinationScroll->verticalScrollBar()->setValue(0);window.findChild<QAction*>("renameDestination")->trigger();if(!save("main-rename-destination"))return 2;
 window.navigate("settings");window.settings()->showUpdates();putgd::test::settle(100);auto* browserFeedback=static_cast<Ui::BrowserFeedback*>(window.findChild<QWidget*>("updateBrowserFeedback"));browserFeedback->opening(QUrl("https://example.invalid/synthetic-release"))({{"opened",false}});
 // showUpdates() scrolls the least distance from where the page is, and the feedback
 // changes the layout after it: scroll again from the top once it settles.
 putgd::test::settle(100);qobject_cast<QScrollArea*>(tabs->widget(0))->verticalScrollBar()->setValue(0);window.settings()->showUpdates();if(!save("main-update-browser-failure"))return 2;browserFeedback->hide();
 QJsonObject realSnapshot;fixture.server.dispatch({{"schemaVersion",1},{"requestId","preview-version"},{"session",fixture.server.socketPath()},{"method","ui.snapshot"},{"params",QJsonObject{}}},[&](Error error,QJsonObject data){if(!error)realSnapshot=data;});if(!waitUntil([&]{return !realSnapshot.isEmpty();}))return 2;
 {auto snapshot=realSnapshot;auto build=snapshot["build"].toObject();build["version"]="26.1026.12345-nightly";build["dirty"]=false;snapshot["build"]=build;fixture.session.applySnapshot(snapshot);auto* failure=static_cast<Ui::BrowserFeedback*>(window.findChild<QWidget*>("appVersionBrowserFeedback"));failure->opening(QUrl("https://example.invalid/synthetic-release"))({{"opened",false}});putgd::test::settle(100);qobject_cast<QScrollArea*>(tabs->widget(0))->verticalScrollBar()->setValue(0);window.settings()->showUpdates();if(!save("main-version-browser-failure"))return 2;fixture.session.applySnapshot(realSnapshot);}
 window.navigate("settings");window.settings()->showUpdates();catalogUpdate={{"supported",true},{"check",true},{"checkedAt",QJsonValue::Null},{"nextCheckAt",QJsonValue::Null},{"channelSource","repository"},{"channel","nightly"},{"build",QJsonObject{{"version","26.1026.1-nightly"},{"channel","nightly"},{"supported",true}}},{"action",QJsonObject{{"kind","software-updates"},{"repository","managed"},{"file","/etc/yum.repos.d/putgd.repo"}}}};fixture.session.refreshSnapshot();if(!waitUntil([&]{return Ui::isLocked(window.findChild<QComboBox*>("updateChannel"));})||!save("65-repository-update-channel"))return 2;
 window.showActionFailure("region",{"output_required","No output is enabled. Choose a destination or turn on Keep a local copy in Settings → Capture.",{}});if(!save("history-tray-output-recovery"))return 2;
 window.showActionFailure("region",{"s3_settings_invalid","Invalid S3 settings; choose the correct bucket and endpoint.",{}});if(!save("history-tray-destination-recovery"))return 2;
 window.navigate("history");window.history()->command({{"action","closeDetails"}},[](Error,QJsonObject){});window.findChild<QToolButton*>("historyListMode")->click();
 {HistoryStore ledger(operations.databasePath());for(int i=0;i<24;++i){Operation op;op.id=QString("synthetic-many-active-%1").arg(i);op.created=now.addSecs(i+100).toString(Qt::ISODateWithMs);op.action="clipboard";op.state="running";op.phase="uploading";op.settings=settings.snapshot();if(ledger.saveOperation(op))return 2;}}
 window.history()->refresh();if(!waitUntil([&]{return window.findChild<QPushButton*>("historyMoreActive")->text()=="23 more active";})||!save("history-many-active"))return 2;
 window.history()->command({{"action","cancelAll"}},[](Error,QJsonObject){});{auto* dialog=window.history()->findChild<QDialog*>("historyCancelAllConfirmation");if(!dialog||!dialog->isVisible())return 2;putgd::test::settle(100);if(!saveDialog(output,"history-cancel-all-confirmation",*dialog,captures))return 2;}window.history()->command({{"action","keepUploading"}},[](Error,QJsonObject){});
 }
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
  EditorWindow editor(doc);editor.setDestination("Private upload to Project archive");editor.resize(1180,820);editor.show();
  auto grab=[&](const QString& name){putgd::test::settle(450);app.processEvents();if(!checkSteppers(editor))return false;const auto image=steadyGrab(editor);const auto path=QDir(output).filePath(name+".png");if(!image.save(path))return false;if(!native.step(editor,name)||!writeInventory(output,name,editor))return false;captures.append(QJsonObject{{"file",name+".png"},{"controls",name+".controls.json"},{"width",image.width()},{"height",image.height()}});return true;};
  editor.selectTool(editor::Tool::Redact);if(!grab("20-editor-redact"))return 2;
  editor.selectTool(editor::Tool::Blur);if(!grab("21-editor-blur-warning"))return 2;
  editor.findChild<QWidget*>("blurStrength")->setFocus(Qt::TabFocusReason);if(!grab("21a-editor-blur-focus"))return 2;
  editor.selectTool(editor::Tool::Arrow);if(!grab("22-editor-arrow-style"))return 2;
  // A crop draft over mixed content shows the dual-stroke guides and the crop fields.
  editor.selectTool(editor::Tool::Crop);editor.viewCommand({{"action","geometry"},{"rect",QJsonArray{40,150,640,420}}},code,message);if(!grab("23-editor-crop"))return 2;
  editor.canvas().cancelCrop();editor.resize(900,620);if(!grab("24-editor-compact"))return 2;
  // A selected mark with collapsed geometry, then its expanded fields and Save a copy… feedback.
  editor.resize(1180,820);editor.selectTool(editor::Tool::Select);editor.canvas().select(quint64(created[3].toDouble()));if(!grab("26-editor-selected"))return 2;
  editor.setDestination("Private upload to Production screenshots for customer support, Europe, Middle East and Asia-Pacific teams, release readiness and accessibility reviews");editor.resize(900,620);if(!grab("26g-editor-long-destination"))return 2;
  editor.setDestination("Private upload to Project archive");
  editor.resize(1060,820);if(!grab("26c-editor-1060"))return 2;
  editor.viewCommand({{"action","geometryDetails"}},code,message);if(!grab("26h-editor-geometry-expanded"))return 2;editor.viewCommand({{"action","geometryDetails"}},code,message);
  editor.viewCommand({{"action","keyboardHelp"}},code,message);putgd::test::settle(100);if(!grab("26d-editor-keyboard-help"))return 2;editor.viewCommand({{"action","keyboardHelp"}},code,message);editor.selectTool(editor::Tool::Text);if(!grab("26e-editor-text-stepper-1060"))return 2;editor.selectTool(editor::Tool::Select);editor.canvas().select(quint64(created[3].toDouble()));editor.resize(1180,820);
  {auto* zoom=editor.findChild<QToolButton*>("editorZoom");zoom->menu()->popup(zoom->mapToGlobal(QPoint(0,zoom->height())));putgd::test::settle(100);if(!zoom->menu()->grab().save(QDir(output).filePath("26f-editor-zoom-menu.png")))return 2;zoom->menu()->close();}
  editor.activateWindow();editor.canvas().setFocus(Qt::TabFocusReason);QTest::keyClick(&editor.canvas(),Qt::Key_Tab);if(!grab("26a-editor-properties-focus"))return 2;
  {editor::Shape label;label.kind=editor::Shape::Kind::Text;label.text="Edge label";label.fontPixels=20;const QFontMetrics metrics(editor::textFont({},20));label.rect=QRect(QPoint(800,520),QSize(metrics.horizontalAdvance(label.text)+1,metrics.height()));const auto id=doc.add(label);editor.documentEdited();editor.canvas().select(id);auto* size=editor.findChild<QSpinBox*>("textSize");size->setValue(200);if(doc.find(id)->fontPixels>=200||!grab("26j-editor-text-size-at-edge"))return 2;doc.remove(id);editor.documentEdited();}
  {editor::Shape label;label.kind=editor::Shape::Kind::Text;label.rect=QRect(940,580,20,20);label.text="Edge label";const auto id=doc.add(label);editor.documentEdited();editor.canvas().select(id);editor.canvas().setFocus();QTest::keyClick(&editor.canvas(),Qt::Key_Return);auto* field=editor.canvas().findChild<QLineEdit*>("annotationText");if(!field||!field->isVisible())return 2;field->setText("Long annotation text extends past the image edge");if(!grab("26b-editor-text-edge"))return 2;editor.canvas().finishText(false);QApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);label=*doc.find(id);label.rect.moveTopLeft(QPoint(460,420));doc.replace(label);editor.documentEdited();editor.canvas().select(id);editor.canvas().setFocus();QTest::keyClick(&editor.canvas(),Qt::Key_Return);field=editor.canvas().findChild<QLineEdit*>("annotationText");if(!field||!field->isVisible())return 2;field->setText("Review ready");if(!grab("26i-editor-text-anchor"))return 2;editor.canvas().finishText(false);doc.remove(id);editor.documentEdited();editor.canvas().select(quint64(created[3].toDouble()));}
  {editor::Shape label;label.kind=editor::Shape::Kind::Text;label.rect=QRect(940,360,20,20);label.text="Clipped note";const auto first=doc.add(label);label.rect.moveTopLeft({800,500});const auto second=doc.add(label);editor.documentEdited();editor.canvas().select(0);
   editor.viewCommand({{"action","selectAffectedText"}},code,message);if(!grab("26j-editor-affected-text"))return 2;
   editor.selectTool(editor::Tool::Crop);editor.viewCommand({{"action","geometry"},{"rect",QJsonArray{40,150,640,420}}},code,message);editor.viewCommand({{"action","selectAffectedText"}},code,message);if(!grab("26k-editor-affected-crop"))return 2;
   editor.canvas().cancelCrop();doc.remove(first);doc.remove(second);editor.documentEdited();editor.canvas().select(quint64(created[3].toDouble()));}
  editor.showSaved("/home/synthetic/Pictures/put.gd-edit-2026-10-26-14-03-22.png");if(!grab("27-editor-saved"))return 2;editor.resize(900,620);if(!grab("27b-editor-saved-compact"))return 2;editor.resize(1180,820);
  editor.showSavedCopy();if(!grab("27a-editor-folder-failed"))return 2;
  editor.showSaveFailed("/media/synthetic/readonly/put.gd-edit-2026-10-26-14-03-22.png","Permission denied");if(!grab("28-editor-save-failed"))return 2;editor.resize(900,620);if(!grab("28c-editor-save-failed-compact"))return 2;editor.resize(1180,820);
  {auto* target=editor.findChild<Ui::ElidedLabel*>("editorSaveError");editor.findChild<QToolButton*>("editorSaveDetails")->click();if(!target->fullTextPopup())return 2;if(!grab("28a-editor-save-path"))return 2;const auto popup=target->fullTextPopup()->grab();if(!popup.save(QDir(output).filePath("28b-editor-save-path-popup.png")))return 2;captures.append(QJsonObject{{"file","28b-editor-save-path-popup.png"},{"width",popup.width()},{"height",popup.height()}});target->fullTextPopup()->close();}
  editor.canvas().select(0);editor.resize(900,620);
  // Discard… asks first; the dialog alone, as the desktop shows it over the editor.
  editor.viewCommand({{"action","discard"}},code,message);if(auto* box=editor.findChild<QDialog*>("confirmAction")){putgd::test::settle(450);app.processEvents();if(!saveDialog(output,"19-editor-discard-confirm",*box,captures))return 2;}else return 2;
  editor.viewCommand({{"action","keepEditing"}},code,message);
  {QTemporaryDir copy;const auto target=copy.filePath("preview-copy.png");if(!editor.writeCopy(target))return 2;
   if(!editor.grab().save(QDir(output).filePath("29a-editor-saving.png")))return 2;
   captures.append(QJsonObject{{"file","29a-editor-saving.png"},{"width",editor.width()},{"height",editor.height()}});
   if(!waitUntil([&]{return !editor.savingCopy();}))return 2;}
  editor.setBusy("Preparing the edited image…");if(!grab("25-editor-finalizing"))return 2;
  {editor::Shape label;label.kind=editor::Shape::Kind::Text;label.rect=QRect(doc.imageRect().right()-20,doc.imageRect().bottom()-40,20,20);label.text="Annotation beyond the image edge";const auto id=doc.add(label);editor.documentEdited();if(!grab("25a-editor-finalizing-warning"))return 2;doc.remove(id);editor.documentEdited();}
  // A result over the output's limit: editing resumes with the reason, and a
  // destination too long to sit beside the actions wraps above them.
  editor.setDestination("Upload to Production screenshots for customer support (EU) · Shared link");
  editor.showPreparationFailed("The edited image is 41.3 MB, over the 25 MB limit for this output. Your edits are kept: crop it and try again, or save a copy.");if(!grab("29-editor-preparation-failed"))return 2;
  // Clear state 29's failure and the earlier saved-copy feedback before the swatches.
  editor.viewCommand({{"action","keepEditing"}},code,message);if(!code.isEmpty())return 2;
  editor.viewCommand({{"action","dismissSavedCopy"}},code,message);if(!code.isEmpty())return 2;
  editor.setDestination("Private upload to Project archive");
  const auto clean=editor.viewState();if(!clean["preparationFailure"].isNull()||clean["saveFeedbackVisible"].toBool()||!clean["message"].isNull())return 2;
  editor.selectTool(editor::Tool::Arrow);editor.setRecentColors({"#123456","#abcdef"});if(!grab("29b-editor-recent-colours"))return 2;
  editor.allowClose();editor.close();
 }
 {
  // Dark app content must keep a visible image boundary in either scheme.
  QImage image(960,600,QImage::Format_RGB32);image.fill(QColor("#191919"));
  QPainter painter(&image);painter.fillRect(QRect(0,0,960,44),QColor("#202020"));painter.setFont(Ui::font());painter.setPen(QColor("#F0F0EC"));painter.drawText(QRect(24,0,900,44),Qt::AlignVCenter,"Synthetic dark screenshot");painter.end();
  editor::EditDocument doc(image);EditorWindow editor(doc);editor.setDestination("Private upload to Project archive");editor.resize(1180,820);editor.selectTool(editor::Tool::Select);editor.show();putgd::test::settle(450);
  const auto captured=steadyGrab(editor);if(!captured.save(QDir(output).filePath("26l-editor-dark-content.png")))return 2;
  captures.append(QJsonObject{{"file","26l-editor-dark-content.png"},{"width",captured.width()},{"height",captured.height()}});editor.allowClose();editor.close();
 }
 }
#ifdef PUTGD_CATALOG_REVIEW
 if(!editorOnly){
  // Settings recovery at the supported compact width.
 if(!recordOnly){DataDir temp("pgs");if(!temp.isValid())return 2;SettingsService config(temp.filePath("config.json"),[](auto,auto){return Error{};});auto doc=Settings::developmentTemplate();auto profiles=doc["profiles"].toArray();auto profile=profiles.first().toObject();profile["provider"]="proton-drive";profile["settings"]=ProtonProvider::defaults();profiles[0]=profile;doc["profiles"]=profiles;if(!config.save(doc,{}))return 2;CredentialStore secrets;UiSessionFixture recovery(config,secrets);JsonReply finish;
  recovery.setup.setProviderSetup([&](const QString&,const Destination&,const QJsonObject&,JsonReply done,auto event){finish=done;event({{"event","auth"},{"url","https://account.proton.me/cli?synthetic-preview"}});});host::setTestOpener([](const QUrl&){});MainWindow recover(recovery.session);recover.resize(1060,680);recover.navigate("destinations");recover.show();putgd::test::settle(100);if(!steadyGrab(recover).save(QDir(output).filePath("settings-proton-signed-out.png")))return 2;captures.append(QJsonObject{{"file","settings-proton-signed-out.png"},{"width",recover.width()},{"height",recover.height()}});recover.findChild<QPushButton*>("connectProtonAccount")->click();if(!waitUntil([&]{return recover.settings()->saveViewState()["setupJobs"].toArray().size()==1&&recover.findChild<QLabel*>("protonAccountStatus")->text().contains("browser");}))return 2;const auto job=recover.settings()->saveViewState()["setupJobs"].toArray().first().toObject()["setupId"].toString();recover.settings()->protonBrowserFinished(job,{{"opened",false}});putgd::test::settle(100);if(!steadyGrab(recover).save(QDir(output).filePath("settings-proton-browser-retry.png")))return 2;captures.append(QJsonObject{{"file","settings-proton-browser-retry.png"},{"width",recover.width()},{"height",recover.height()}});finish({"synthetic_cancelled","Synthetic preview finished",{}},{});host::setTestOpener({});
  // The cancelled Proton setup reports first, so the editor failure is the last status.
  if(!waitUntil([&]{return recover.settings()->saveViewState()["setupJobs"].toArray().isEmpty();}))return 2;
  recover.navigate("settings");recover.findChild<QTabWidget*>("settingsTabs")->setCurrentIndex(3);QFile::remove(config.path());recover.findChild<QPushButton*>("openConfigEditor")->click();auto* scroll=qobject_cast<QScrollArea*>(recover.findChild<QTabWidget*>("settingsTabs")->currentWidget());putgd::test::settle(100);scroll->ensureWidgetVisible(recover.findChild<QLabel*>("configEditorFailure"));if(!steadyGrab(recover).save(QDir(output).filePath("settings-config-editor-failure.png")))return 2;captures.append(QJsonObject{{"file","settings-config-editor-failure.png"},{"width",recover.width()},{"height",recover.height()}});
 }
 // The private review and the ready control bar, over a synthetic frame.
  record::ReviewWindow::Options o;o.workingClip=QDir(output).filePath("no-clip.mkv");o.destination="Project archive";o.sharingMode="share";o.uploadSummary="Upload to Project archive · Shared link";o.durationUs=32000000;o.canvas=QSize(960,600);o.formats={"mp4","webm","gif","webp"};o.codecs=QJsonObject{{"mp4",QJsonArray{"h264","h265"}},{"webm",QJsonArray{"vp9"}}};
  RecordingConfig config;config.format="mp4";config.codec="h264";config.fps=30;config.audio.system=true;o.audioRecorded=true;
  // No clip exists: hold the frame refresh so the synthetic frame stays until a state asks for one.
  qputenv("PUTGD_RECORD_REVIEW_DELAY_MS","3600000");
  record::ReviewWindow review(o,config);review.resize(record::ReviewWindow::defaultSize());review.show();
  if(auto* view=review.findChild<record::FrameView*>())view->setFrame(syntheticCapture(2),12000000);
  RecordingEdit edit;edit.revision=1;RecordingMask solid;solid.id="m-solid";solid.kind="solid";solid.rect=QRect(61,194,256,40);solid.start=8000000;solid.end=21000000;RecordingMask blur=solid;blur.id="m-blur";blur.kind="blur";blur.rect=QRect(337,194,256,130);blur.start=3000000;blur.end=12000000;edit.masks={solid,blur};edit.trimStart=2000000;edit.trimEnd=29000000;review.setEdit(edit);
  auto shot=[&](QWidget& w,const QString& name){if(auto* r=qobject_cast<record::ReviewWindow*>(&w))if(auto* view=r->findChild<record::FrameView*>();view&&view->previewError().isEmpty())view->setFrame(syntheticCapture(2),qint64(r->inspect()["playheadUs"].toDouble()));putgd::test::settle(450);app.processEvents();if(auto* r=qobject_cast<record::ReviewWindow*>(&w)){const auto problem=putgd::test::reviewFooterProblem(*r);if(!problem.isEmpty()){qCritical()<<name<<problem;w.grab().save(QDir(output).filePath(name+"-failed.png"));return false;}}if(!checkSteppers(w))return false;const auto image=steadyGrab(w);if(!image.save(QDir(output).filePath(name+".png")))return false;if(!native.step(w,name)||!writeInventory(output,name,w))return false;captures.append(QJsonObject{{"file",name+".png"},{"controls",name+".controls.json"},{"width",image.width()},{"height",image.height()}});return true;};
  // Each review state also keeps the window's own geometry (header, frame share).
  auto inspected=[&](record::ReviewWindow& r,const QString& name){QSaveFile f(QDir(output).filePath(name+".json"));return f.open(QIODevice::WriteOnly)&&f.write(QJsonDocument(r.inspect()).toJson())>0&&f.commit();};
  review.drive({{"action","selectMask"},{"maskId","m-solid"}});
  if(!shot(review,"30-review")||!inspected(review,"30-review"))return 2;
  review.drive({{"action","seek"},{"time",1000000}});if(!shot(review,"30a-review-mask-inactive")||!inspected(review,"30a-review-mask-inactive"))return 2;
  review.drive({{"action","seek"},{"time",25000000}});if(!shot(review,"30b-review-mask-ended")||!inspected(review,"30b-review-mask-ended"))return 2;
  review.drive({{"action","seek"},{"time",10000000}});
  review.drive({{"action","selectMask"},{"maskId","m-blur"}});if(!shot(review,"31-review-blur-selected")||!inspected(review,"31-review-blur-selected"))return 2;
  review.drive({{"action","addMask"},{"kind","blur"}});if(!shot(review,"31j-review-add-blur-warning")||!inspected(review,"31j-review-add-blur-warning"))return 2;
  review.drive({{"action","selectMask"},{"maskId","m-solid"}});if(!shot(review,"31k-review-disabled-blur-warning")||!inspected(review,"31k-review-disabled-blur-warning"))return 2;review.drive({{"action","selectMask"},{"maskId","m-blur"}});review.drive({{"action","dismissBlurTip"}});
  {review.drive({{"action","showMaskOptions"}});if(!shot(*review.findChild<QMenu*>("maskOptionsMenu"),"31i-review-mask-options"))return 2;review.drive({{"action","closeMaskOptions"}});}
  {const auto size=review.size();review.resize(1060,size.height());if(!shot(review,"31b-review-strength-1060"))return 2;review.resize(size);}
  {const auto size=review.size();review.resize(1342,size.height());if(!shot(review,"31m-review-header-wide")||!inspected(review,"31m-review-header-wide"))return 2;review.resize(size);}
  {auto many=edit;for(int i=2;i<8;++i){auto mask=solid;mask.id=QStringLiteral("m-extra-%1").arg(i);many.masks.append(mask);}review.setEdit(many);review.drive({{"action","selectMask"},{"maskId","m-solid"}});if(!shot(review,"31c-review-many-lanes")||!inspected(review,"31c-review-many-lanes"))return 2;review.setEdit(edit);review.drive({{"action","selectMask"},{"maskId","m-blur"}});}
  for(int i=0;i<3;++i){review.resize(review.minimumSizeHint());putgd::test::settle(40);}if(!shot(review,"32-review-minimum")||!inspected(review,"32-review-minimum"))return 2;
  {QEvent inactive(QEvent::WindowDeactivate);QApplication::sendEvent(&review,&inactive);if(!shot(review,"settings-review-inactive-header"))return 2;QEvent active(QEvent::WindowActivate);QApplication::sendEvent(&review,&active);}
  review.drive({{"action","selectMask"},{"maskId",QJsonValue(QJsonValue::Null)}});review.drive({{"action","dismissBlurTip"}});
  if(!shot(review,"32a-review-minimum-no-selection")||!inspected(review,"32a-review-minimum-no-selection"))return 2;
  review.resize(record::ReviewWindow::defaultSize());
  // Keyboard mask editing: the frame focused by Tab, the selected cover's ring.
  review.drive({{"action","selectMask"},{"maskId","m-solid"}});review.activateWindow();putgd::test::waitForWindowActive(&review);if(auto* view=review.findChild<record::FrameView*>())view->setFocus(Qt::TabFocusReason);if(!shot(review,"38-review-keyboard-mask")||!inspected(review,"38-review-keyboard-mask"))return 2;
  // An export failure and a codec warning, each with its actions, in the status row.
  review.drive({{"action","format"},{"format","mp4"},{"codec","h265"}});review.showError("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");if(!shot(review,"39-review-status-errors")||!inspected(review,"39-review-status-errors"))return 2;
  {
   auto constrainedOptions=o;constrainedOptions.availableSize=QSize(1366,680);
   record::ReviewWindow constrained(constrainedOptions,config);constrained.setEdit(edit);constrained.show();
   constrained.drive({{"action","selectMask"},{"maskId","m-blur"}});constrained.drive({{"action","format"},{"format","mp4"},{"codec","h265"}});
   constrained.showError("Export failed: the encoder stopped after 412 frames because the disk holding the private recording is full; free some space and try again");
   if(auto* view=constrained.findChild<record::FrameView*>())view->setFrame(syntheticCapture(2),12000000);
   if(!shot(constrained,"67-review-680-workspace")||!inspected(constrained,"67-review-680-workspace"))return 2;
   {auto* destination=constrained.findChild<QToolButton*>("uploadDestinationMenu");destination->menu()->popup(destination->mapToGlobal(QPoint(0,destination->height())));putgd::test::settle(100);if(!shot(*destination->menu(),"review-destination-full-text"))return 2;destination->menu()->close();}
   constrained.drive({{"action","showAdvice"}});if(auto* dialog=constrained.findChild<QWidget*>("reviewAdvice")){if(!shot(*dialog,"68-review-error-details"))return 2;}else return 2;
  }
  {auto* add=review.findChild<QToolButton*>("addMask");add->menu()->popup(add->mapToGlobal(QPoint(0,add->height())));putgd::test::settle(100);if(!shot(*add->menu(),"31d-review-add-mask-menu"))return 2;add->menu()->close();}
  {QComboBox* formats=nullptr;for(auto* combo:review.findChildren<QComboBox*>())if(combo->accessibleName()=="Output format")formats=combo;if(!formats)return 2;formats->showPopup();putgd::test::settle(100);if(!shot(*formats->view()->window(),"31e-review-format-menu"))return 2;formats->hidePopup();}
  review.drive({{"action","dismissStatus"}});review.drive({{"action","format"},{"format","mp4"},{"codec","h264"}});
  review.drive({{"action","format"},{"format","gif"}});if(!shot(review,"31f-review-gif-audio")||!inspected(review,"31f-review-gif-audio"))return 2;
  review.drive({{"action","format"},{"format","mp4"},{"codec","h264"}});if(!shot(review,"31g-review-restored-audio")||!inspected(review,"31g-review-restored-audio"))return 2;
  {auto silentOptions=o;silentOptions.audioRecorded=false;record::ReviewWindow silent(silentOptions,config);silent.show();if(!shot(silent,"31h-review-no-audio")||!inspected(silent,"31h-review-no-audio"))return 2;}
  // A refresh that fails (there is no clip): the earlier frame is dimmed and named.
  review.drive({{"action","seek"},{"time",16000000}});review.drive({{"action","retryPreview"}});if(!shot(review,"40-review-preview-error")||!inspected(review,"40-review-preview-error"))return 2;
  if(auto* view=review.findChild<record::FrameView*>())view->setFrame(syntheticCapture(2),16000000);
  review.setSavedCopy("/tmp/product-walkthrough.mp4",edit.revision,config);if(!shot(review,"49-review-saved-copy")||!inspected(review,"49-review-saved-copy"))return 2;
  auto later=edit;later.revision++;review.setEdit(later);if(auto* view=review.findChild<record::FrameView*>())view->setFrame(syntheticCapture(2),16000000);if(!shot(review,"50-review-earlier-copy")||!inspected(review,"50-review-earlier-copy"))return 2;
  review.drive({{"action","dismissSavedCopy"}});
  // Discard… asks first; the dialog alone, as the desktop shows it over the review.
  review.drive({{"action","discard"}});if(auto* box=review.findChild<QMessageBox*>("confirmAction")){if(!shot(*box,"41-review-discard-confirm"))return 2;}else return 2;review.drive({{"action","keepReviewing"}});
  qunsetenv("PUTGD_RECORD_REVIEW_DELAY_MS");
  // The review window's own frame under real input (compact header).
  // Alone on the backdrop, so the driver locates and drives only this window.
  window.hide();review.resize(record::ReviewWindow::defaultSize());review.showNormal();if(!native.chrome(review,"91-review-chrome"))return 2;
  review.hide();window.show();
  record::ControlBar::Options bar;bar.targetLabel="Region · 960 × 600";bar.formats=o.formats;bar.codecs=o.codecs;bar.audioSupported=true;
  // A 1366×768 laptop decides one row or two (two at 150% text).
  record::ControlBar ready(bar,config);ready.fitTo(QRect(0,0,1366,768));ready.show();if(!shot(ready,"33-record-ready-bar"))return 2;
  auto* barFormats=ready.findChild<QComboBox*>("format");if(!barFormats)return 2;
  for(int i=0;i<barFormats->count();++i)if(barFormats->itemData(i).toStringList().first()=="gif"){barFormats->setCurrentIndex(i);break;}
  if(!shot(ready,"33a-record-gif-requested-audio"))return 2;
  for(int i=0;i<barFormats->count();++i)if(barFormats->itemData(i).toStringList()==QStringList{"mp4","h264"}){barFormats->setCurrentIndex(i);break;}
  if(!shot(ready,"33b-record-restored-audio"))return 2;
  ready.setState("recording");ready.setRetained(12300000);if(!shot(ready,"34-record-recording-bar"))return 2;
  ready.setState("paused");if(!shot(ready,"35-record-paused-bar"))return 2;
  ready.setState("recording");if(auto* cancel=ready.findChild<QPushButton*>("cancel"))cancel->click();if(!ready.confirming()||!shot(ready,"36-record-discard-confirm"))return 2;
  ready.findChild<QPushButton*>("keepRecording")->click();ready.setState("paused");if(auto* cancel=ready.findChild<QPushButton*>("cancel"))cancel->click();if(!ready.confirming()||!shot(ready,"42-record-paused-discard-confirm"))return 2;
  ready.findChild<QPushButton*>("keepRecording")->click();ready.setState("ready");ready.setRetained(0);ready.fitTo(QRect(0,0,1024,768));if(!shot(ready,"37-record-ready-narrow"))return 2;
  ready.hide();
  qputenv("PUTGD_RECORD_REVIEW_DELAY_MS","60000");
  record::ReviewWindow::Options privateOptions=o;privateOptions.destination="Proton Drive";privateOptions.sharingMode="private";privateOptions.uploadSummary="Private upload to Proton Drive";
  record::ReviewWindow privateReview(privateOptions,config);privateReview.show();if(auto* view=privateReview.findChild<record::FrameView*>())view->setFrame(syntheticCapture(2),0);if(!shot(privateReview,"43-review-private-destination")||!inspected(privateReview,"43-review-private-destination"))return 2;privateReview.hide();
  {auto longOptions=o;longOptions.destination="Project archive for customer support in the European Union";longOptions.uploadSummary="Upload to "+longOptions.destination+" · Shared link";record::ReviewWindow longReview(longOptions,config);longReview.setEdit(edit);longReview.show();if(auto* view=longReview.findChild<record::FrameView*>())view->setFrame(syntheticCapture(2),12000000);if(!shot(longReview,"43a-review-long-destination")||!inspected(longReview,"43a-review-long-destination"))return 2;longReview.resize(1342,longReview.height());if(!shot(longReview,"43b-review-long-destination-header")||!inspected(longReview,"43b-review-long-destination-header"))return 2;longReview.hide();}
  record::ReviewWindow crowded(o,config);RecordingEdit many;many.revision=1;
  for(int i=0;i<8;++i){auto m=solid;m.id=QString("lane-%1").arg(i);m.start=1000000;m.end=29000000;many.masks.append(m);}
  crowded.setEdit(many);crowded.show();if(auto* view=crowded.findChild<record::FrameView*>())view->setFrame(syntheticCapture(2),0);
  if(!shot(crowded,"48-review-many-masks")||!inspected(crowded,"48-review-many-masks"))return 2;
  crowded.drive({{"action","selectMask"},{"maskId","lane-0"}});crowded.drive({{"action","nextMaskLanes"}});
  if(!shot(crowded,"48a-review-next-mask-lanes")||!inspected(crowded,"48a-review-next-mask-lanes"))return 2;
  crowded.hide();qunsetenv("PUTGD_RECORD_REVIEW_DELAY_MS");
  const auto screen=QGuiApplication::primaryScreen()->geometry();
  CaptureFrame selectionFrame{"fixture",screen,screen,1,0,{}};
  RegionOverlay screenshot({selectionFrame},nullptr,"Uploads right away to Team Drive · Shared link");screenshot.start();
  for(auto* w:QApplication::topLevelWidgets())if(w->isVisible()&&w->objectName()=="putgd-region-overlay"){if(!shot(*w,"46-region-entire-display"))return 2;QTest::keyClick(w,Qt::Key_Right);if(!shot(*w,"44-region-keyboard"))return 2;QTest::mousePress(w,Qt::LeftButton,{},QPoint(w->width()/2-200,8));QTest::mouseMove(w,QPoint(w->width()/2+200,220));if(!shot(*w,"44a-region-badge-outside-selection"))return 2;break;}
  screenshot.cancel();
  record::SelectionOverlay selector;selector.start();
  for(auto* w:QApplication::topLevelWidgets())if(w->isVisible()&&w->objectName()=="putgd-record-select"){if(!shot(*w,"47-record-entire-display"))return 2;QTest::keyClick(w,Qt::Key_Right);if(!shot(*w,"45-record-region-keyboard"))return 2;QTest::mousePress(w,Qt::LeftButton,{},QPoint(w->width()/2-200,8));QTest::mouseMove(w,QPoint(w->width()/2+200,220));if(!shot(*w,"45a-record-badge-outside-selection"))return 2;break;}
  selector.cancel();
 }
#endif
 if(!recordOnly&&!editorOnly){
  AboutWindow about(fixture.session.build(),&window);Ui::styleWindow(about);about.show();
  auto grabAbout=[&](const QString& name){app.processEvents();const auto image=steadyGrab(about);if(!image.save(QDir(output).filePath(name+".png"))||!native.step(about,name)||!writeInventory(output,name,about))return false;captures.append(QJsonObject{{"file",name+".png"},{"controls",name+".controls.json"},{"width",image.width()},{"height",image.height()}});return true;};
  if(!grabAbout("about"))return 2;auto* details=about.findChild<QToolButton*>("buildDetails");if(!details)return 2;details->click();if(!grabAbout("about-build-details"))return 2;about.findChild<QPushButton*>("copyBuildDetails")->click();if(!grabAbout("about-build-details-copied"))return 2;about.close();
 }
 // The custom frame under real input: the driver moves, maximizes, restores,
 // resizes and finally closes the main window.
 window.resize(1060,680);window.showNormal();if(!native.chrome(window,"90-chrome"))return 2;
 QSaveFile manifest(QDir(output).filePath("manifest.json"));if(!manifest.open(QIODevice::WriteOnly))return 2;manifest.write(QJsonDocument(QJsonObject{{"kind","synthetic-ui-catalog"},{"build",sourceBuild()},{"qtVersion",QString::fromLatin1(qVersion())},{"desktopAcceptance",false},{"platform",QGuiApplication::platformName()},{"networkActions",0},{"credentialFixture","synthetic session only"},{"sourceReads",source.reads},{"captures",captures},{"skippedCaptures",skippedCaptures},
  // What this run covered, and every image it wrote (not all are captures),
  // so a partial render cannot pass for a complete one.
  {"scope",QJsonObject{{"review",
#ifdef PUTGD_CATALOG_REVIEW
   true
#else
   false
#endif
  },{"recordOnly",recordOnly},{"editorOnly",editorOnly},{"handshake",native.active()}}},
  {"images",QJsonArray::fromStringList(QDir(output).entryList({"*.png"},QDir::Files,QDir::Name))}}).toJson(QJsonDocument::Indented));if(!manifest.commit())return 2;return source.reads==0?0:2;
}
