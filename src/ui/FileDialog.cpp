#include "FileDialog.h"
#include "Theme.h"
#include "Ui.h"
#include <QAbstractItemView>
#include <QApplication>
#include <QFile>
#include <QFileDialog>
#include <QGuiApplication>
#include <QLoggingCategory>
#include <QPalette>
#include <QToolButton>
#include <QTreeView>
#include <QHeaderView>
#include <QScreen>
#include <QLocale>
#include <QDateTime>
#include <QUrl>
#include <QUuid>
#include <QWidget>
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#endif

Q_LOGGING_CATEGORY(lcFileDialog,"putgd.filedialog")

namespace putgd {
namespace {
const QString portalService=QStringLiteral("org.freedesktop.portal.Desktop");
const QString portalPath=QStringLiteral("/org/freedesktop/portal/desktop");
const QString chooserInterface=QStringLiteral("org.freedesktop.portal.FileChooser");
const QString requestInterface=QStringLiteral("org.freedesktop.portal.Request");
// Accepting a request is quick once the portal runs; the chooser itself then
// stays open as long as the person needs. A first use may start the portal.
int portalCallTimeoutMs=8000;
// Response 2 is "ended in some other way" (#781). The GTK and GNOME backends
// answer it when the person presses Escape or closes the chooser's window; the
// portal answers it when a running backend fails (a backend that fails to start
// makes the call itself error). The chooser takes about 300 ms to get focus after
// the request is accepted, so a 2 this soon is a failure, not a person.
int portalFailureWindowMs=250;
QString modeName(FileDialog::Mode mode){return mode==FileDialog::Mode::Save?"save":mode==FileDialog::Mode::Folder?"folder":"open";}
QJsonValue text(const QString& value){return value.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(value);}
// Qt's widget dialog in the Theme tokens: the generic platform theme gives it
// Fusion's light palette, which the windows' own stylesheets only partly cover.

void styleFallback(QFileDialog* dialog){
 const auto scheme=Theme::scheme();const auto& p=Theme::current();
 QPalette palette=dialog->palette();
 for(const auto group:{QPalette::Active,QPalette::Inactive}){
  palette.setColor(group,QPalette::Window,p.bg);palette.setColor(group,QPalette::WindowText,p.text);
  palette.setColor(group,QPalette::Base,p.surface);palette.setColor(group,QPalette::AlternateBase,p.surface);
  palette.setColor(group,QPalette::Text,p.text);palette.setColor(group,QPalette::Button,p.surfaceRaised);palette.setColor(group,QPalette::ButtonText,p.text);
  palette.setColor(group,QPalette::Highlight,p.selected);palette.setColor(group,QPalette::HighlightedText,p.text);
  palette.setColor(group,QPalette::PlaceholderText,p.textMuted);palette.setColor(group,QPalette::Link,p.link);
  palette.setColor(group,QPalette::ToolTipBase,p.surfaceRaised);palette.setColor(group,QPalette::ToolTipText,p.text);
  palette.setColor(group,QPalette::Light,p.borderStrong);palette.setColor(group,QPalette::Midlight,p.border);palette.setColor(group,QPalette::Mid,p.border);
  palette.setColor(group,QPalette::Dark,p.bg);palette.setColor(group,QPalette::Shadow,p.bg);
 }
 palette.setColor(QPalette::Disabled,QPalette::Window,p.bg);palette.setColor(QPalette::Disabled,QPalette::Base,p.surface);palette.setColor(QPalette::Disabled,QPalette::Button,p.surfaceRaised);
 for(const auto role:{QPalette::WindowText,QPalette::Text,QPalette::ButtonText})palette.setColor(QPalette::Disabled,role,p.textDisabled);
 dialog->setPalette(palette);
 dialog->setProperty("themeScheme",Theme::name(scheme));
 // The app style paints the fallback's controls from the tokens; no stylesheet.
 dialog->setStyleSheet({});
 // The toolbar's Fusion pixmaps are dark on dark; IconArt glyphs follow the tokens.
 const QList<std::pair<const char*,const char*>> buttons{{"backButton","back"},{"forwardButton","forward"},{"toParentButton","up"},{"newFolderButton","folder-new"},{"listModeButton","view-list"},{"detailModeButton","view-detail"}};
 for(const auto& [name,glyph]:buttons)if(auto* button=dialog->findChild<QToolButton*>(name))button->setIcon(Ui::icon(glyph));
}
#ifdef Q_OS_LINUX
QString shortName(const QString& name){return name.startsWith(':')?name.mid(1).replace('.','_'):name;}
QList<portal::Filter> portalFilters(const QList<FileDialog::Filter>& filters){
 QList<portal::Filter> result;
 for(const auto& filter:filters){portal::Filter f{filter.label,{}};for(const auto& pattern:filter.patterns)f.rules.append({0,pattern});result.append(f);}
 return result;
}
#endif
}
#ifdef Q_OS_LINUX
namespace portal {
QDBusArgument& operator<<(QDBusArgument& a,const FilterRule& r){a.beginStructure();a<<r.type<<r.pattern;a.endStructure();return a;}
const QDBusArgument& operator>>(const QDBusArgument& a,FilterRule& r){a.beginStructure();a>>r.type>>r.pattern;a.endStructure();return a;}
QDBusArgument& operator<<(QDBusArgument& a,const Filter& f){a.beginStructure();a<<f.label<<f.rules;a.endStructure();return a;}
const QDBusArgument& operator>>(const QDBusArgument& a,Filter& f){a.beginStructure();a>>f.label>>f.rules;a.endStructure();return a;}
void registerTypes(){
 static const bool registered=[]{qDBusRegisterMetaType<FilterRule>();qDBusRegisterMetaType<QList<FilterRule>>();qDBusRegisterMetaType<Filter>();qDBusRegisterMetaType<QList<Filter>>();return true;}();
 Q_UNUSED(registered);
}
}
#endif

FileDialog::FileDialog(QWidget* owner):QObject(owner),owner_(owner){}
FileDialog::~FileDialog(){
 // The owner is going away: close the chooser without answering it.
 done_=nullptr;
 if(phase_==Phase::Portal)abandonPortal();
 if(fallback_){fallback_->disconnect(this);fallback_->deleteLater();}
 block(false);
}
QFileDialog* FileDialog::fallbackDialog()const{return fallback_.data();}
void FileDialog::setPortalCallTimeoutMs(int milliseconds){portalCallTimeoutMs=qMax(1,milliseconds);}
void FileDialog::setPortalFailureWindowMs(int milliseconds){portalFailureWindowMs=qMax(0,milliseconds);}
QString FileDialog::parentWindow(const QWidget* owner){
 if(!owner||QGuiApplication::platformName()!="xcb")return {};
 const auto* window=owner->window();
 return window->internalWinId()?QStringLiteral("x11:%1").arg(quint64(window->internalWinId()),0,16):QString{};
}
bool FileDialog::open(const Options& options,Done done){
 if(isOpen()||!owner_)return false;
 options_=options;done_=std::move(done);backend_.clear();parent_.clear();reason_.clear();handle_.clear();
#ifdef Q_OS_LINUX
 if(qEnvironmentVariable("PUTGD_FILE_DIALOG")=="fallback"){openFallback("disabled");return true;}
 startPortal();
#else
 openFallback("no_portal");
#endif
 return true;
}
void FileDialog::startPortal(){
#ifdef Q_OS_LINUX
 auto bus=QDBusConnection::sessionBus();
 if(!bus.isConnected()){openFallback("no_session_bus");return;}
 phase_=Phase::Portal;backend_="portal";parent_=parentWindow(owner_);block(true);emit stateChanged();
 if(options_.mode!=Mode::Folder){requestPortal();return;}
 // Folders need FileChooser version 3 (its directory option).
 auto get=QDBusMessage::createMethodCall(portalService,portalPath,"org.freedesktop.DBus.Properties","Get");get<<chooserInterface<<QStringLiteral("version");
 auto* watcher=new QDBusPendingCallWatcher(bus.asyncCall(get,portalCallTimeoutMs),this);
 connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher,attempt=attempt_]{
  watcher->deleteLater();
  if(attempt!=attempt_)return;
  const QDBusPendingReply<QDBusVariant> reply=*watcher;
  if(reply.isError()){openFallback(reply.error().name());return;}
  if(reply.value().variant().toUInt()<3){openFallback("portal_version");return;}
  requestPortal();
 });
#endif
}
void FileDialog::requestPortal(){
#ifdef Q_OS_LINUX
 portal::registerTypes();
 auto bus=QDBusConnection::sessionBus();
 token_="putgd"+QUuid::createUuid().toString(QUuid::Id128).left(16);
 // Subscribe before asking, at the handle the portal makes from the token, so
 // an early Response is not missed (an old portal's reply may name another).
 const auto expected=portalPath+"/request/"+shortName(bus.baseService())+"/"+token_;
 subscribe(expected);
 QVariantMap options{{"handle_token",token_},{"modal",true}};
 if(options_.mode==Mode::Folder)options["directory"]=true;
 if(!options_.filters.isEmpty()){const auto filters=portalFilters(options_.filters);options["filters"]=QVariant::fromValue(filters);options["current_filter"]=QVariant::fromValue(filters.first());}
 if(!options_.folder.isEmpty()){auto folder=QFile::encodeName(options_.folder);folder.append('\0');options["current_folder"]=folder;}
 if(options_.mode==Mode::Save&&!options_.name.isEmpty())options["current_name"]=options_.name;
 auto call=QDBusMessage::createMethodCall(portalService,portalPath,chooserInterface,options_.mode==Mode::Save?"SaveFile":"OpenFile");
 call<<parent_<<options_.title<<options;
 qCInfo(lcFileDialog).noquote()<<"portal"<<call.member()<<"parent"<<(parent_.isEmpty()?QStringLiteral("none"):parent_);
 asked_.start();
 auto* watcher=new QDBusPendingCallWatcher(bus.asyncCall(call,portalCallTimeoutMs),this);
 connect(watcher,&QDBusPendingCallWatcher::finished,this,[this,watcher,attempt=attempt_,expected]{
  watcher->deleteLater();
  const QDBusPendingReply<QDBusObjectPath> reply=*watcher;
  const auto handle=reply.isError()?QString{}:reply.value().path();
  // Cancelled or given up on before the portal accepted: the token's handle
  // was closed then; close one an old portal named differently too.
  if(attempt!=attempt_){if(!handle.isEmpty()&&handle!=expected)closeRequest(handle);return;}
  if(reply.isError()){openFallback(reply.error().name());return;}
  // The chooser can only show once the request is accepted.
  if(phase_==Phase::Portal)asked_.start();
  if(handle!=handle_){unsubscribe();subscribe(handle);}
 });
 // A portal that goes away while its chooser is open never answers.
 delete gone_;
 auto* gone=new QDBusServiceWatcher(portalService,bus,QDBusServiceWatcher::WatchForUnregistration,this);gone_=gone;
 connect(gone,&QDBusServiceWatcher::serviceUnregistered,this,[this,attempt=attempt_]{if(attempt==attempt_&&phase_==Phase::Portal)openFallback("portal_exited");});
#endif
}
void FileDialog::abandonPortal(){
 // Every pending portal callback belongs to the attempt this ends.
 ++attempt_;unsubscribe();
 if(!handle_.isEmpty())closeRequest(handle_);
 handle_.clear();
 if(gone_){gone_->deleteLater();gone_=nullptr;}
}
void FileDialog::portalResponse(uint response,const QVariantMap& results){
 if(phase_!=Phase::Portal)return;
 ++attempt_;unsubscribe();handle_.clear();
 if(gone_){gone_->deleteLater();gone_=nullptr;}
 const auto elapsed=asked_.isValid()?asked_.elapsed():0;
 qCInfo(lcFileDialog).noquote()<<"portal response"<<response<<"after"<<elapsed<<"ms since accepted"
     <<(response==2?(elapsed>=portalFailureWindowMs?"(closed by the person, or the backend failed late)":"(backend failed early)"):"");
 if(response==1||(response==2&&elapsed>=portalFailureWindowMs)){finish("cancelled",{});return;}
 if(response!=0){openFallback("portal_failed");return;}
 const auto uris=results.value("uris").toStringList();
 const QUrl url(uris.value(0));
 if(uris.isEmpty()||!url.isLocalFile()){finish("failed",{},"The chosen location is not a local file.");return;}
 finish("chosen",url.toLocalFile());
}
void FileDialog::subscribe(const QString& handle){
#ifdef Q_OS_LINUX
 handle_=handle;
 QDBusConnection::sessionBus().connect(portalService,handle,requestInterface,"Response",this,SLOT(portalResponse(uint,QVariantMap)));
#else
 Q_UNUSED(handle);
#endif
}
void FileDialog::unsubscribe(){
#ifdef Q_OS_LINUX
 if(!handle_.isEmpty())QDBusConnection::sessionBus().disconnect(portalService,handle_,requestInterface,"Response",this,SLOT(portalResponse(uint,QVariantMap)));
#endif
}
void FileDialog::closeRequest(const QString& handle){
#ifdef Q_OS_LINUX
 QDBusConnection::sessionBus().call(QDBusMessage::createMethodCall(portalService,handle,requestInterface,"Close"),QDBus::NoBlock);
#else
 Q_UNUSED(handle);
#endif
}
void FileDialog::openFallback(const QString& reason){
 if(phase_==Phase::Portal)abandonPortal();
 block(false);
 if(!owner_){finish("failed",{},"The window closed.");return;}
 phase_=Phase::Fallback;backend_="fallback";reason_=reason;
 qCInfo(lcFileDialog).noquote()<<"fallback dialog, reason"<<reason;
 auto* dialog=new QFileDialog(owner_,options_.title,options_.folder);
 dialog->setObjectName("putgdFileDialog");dialog->setAttribute(Qt::WA_DeleteOnClose);dialog->setWindowModality(Qt::ApplicationModal);
#ifdef Q_OS_LINUX
 // Qt's own widget dialog, never the platform theme's (#863). putgd-ui keeps the
 // desktop's theme, whose native helper is GTK's chooser on GNOME, which never
 // maps on GNOME 50 Wayland and leaves only an invisible modal blocker, or KDE's.
 dialog->setOption(QFileDialog::DontUseNativeDialog);
#endif
 if(options_.mode==Mode::Save){dialog->setAcceptMode(QFileDialog::AcceptSave);dialog->setFileMode(QFileDialog::AnyFile);if(!options_.name.isEmpty())dialog->selectFile(options_.name);}
 else if(options_.mode==Mode::Folder){dialog->setFileMode(QFileDialog::Directory);dialog->setOption(QFileDialog::ShowDirsOnly);}
 else dialog->setFileMode(QFileDialog::ExistingFile);
 QStringList filters;for(const auto& filter:options_.filters)filters.append(QStringLiteral("%1 (%2)").arg(filter.label,filter.patterns.join(' ')));
 if(!filters.isEmpty())dialog->setNameFilters(filters);
 theme(dialog);
 // Qt's default leaves the detail columns too narrow for their own values.
 QSize size(Ui::scaled(820),Ui::scaled(520));
 if(const auto* screen=owner_->window()->screen())size=size.boundedTo(screen->availableGeometry().size()*0.9);
 dialog->resize(size.expandedTo(dialog->minimumSizeHint()));
 if(auto* tree=dialog->findChild<QTreeView*>("treeView");tree&&tree->header()->count()>=4){
  const auto metrics=tree->fontMetrics();const int padding=32;
  tree->header()->resizeSection(1,metrics.horizontalAdvance(QStringLiteral("1023.9 KB"))+padding);
  tree->header()->resizeSection(2,metrics.horizontalAdvance(QStringLiteral("MP4 video file"))+padding);
  tree->header()->resizeSection(3,metrics.horizontalAdvance(QLocale().toString(QDateTime(QDate(2026,12,30),QTime(23,59)),QLocale::ShortFormat))+padding);
 }
 fallback_=dialog;
 connect(dialog,&QFileDialog::finished,this,[this,dialog](int result){
  if(fallback_!=dialog)return;
  fallback_.clear();
  const auto files=dialog->selectedFiles();
  if(result==QDialog::Accepted&&!files.isEmpty())finish("chosen",files.first());else finish("cancelled",{});
 });
 emit stateChanged();
 dialog->open();
}
void FileDialog::theme(QFileDialog* dialog){
 styleFallback(dialog);
 connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,dialog,[dialog]{styleFallback(dialog);});
}
void FileDialog::cancel(){
 if(phase_==Phase::Fallback&&fallback_){fallback_->reject();return;}
 if(phase_!=Phase::Portal)return;
 abandonPortal();finish("cancelled",{});
}
void FileDialog::finish(const QString& status,const QString& path,const QString& error){
 block(false);
 last_={{"status",status},{"backend",text(backend_)},{"mode",modeName(options_.mode)},{"path",text(path)},{"fallbackReason",text(reason_)},{"error",text(error)}};
 qCInfo(lcFileDialog).noquote()<<"file dialog"<<status<<"via"<<backend_;
 phase_=Phase::Idle;
 auto done=std::exchange(done_,nullptr);
 const Result result{status=="chosen"?Result::Status::Chosen:status=="failed"?Result::Status::Failed:Result::Status::Cancelled,path,error};
 QPointer<FileDialog> guard(this);
 emit stateChanged();
 if(guard&&done)done(result);
}
void FileDialog::block(bool on){
 // Modal like the dialog it replaces: the window takes no input while the
 // portal's chooser is open. Qt does the same for its own native dialogs: a
 // modal widget that is never mapped.
 if(!on){if(blocker_){blocker_->hide();blocker_->deleteLater();blocker_.clear();}return;}
 if(blocker_||!owner_)return;
 auto* blocker=new QWidget(owner_->window(),Qt::Dialog);
 blocker->setObjectName("putgdPortalDialogModal");blocker->setAttribute(Qt::WA_DontShowOnScreen);blocker->setWindowModality(Qt::ApplicationModal);
 blocker->setWindowTitle(options_.title);blocker->show();blocker_=blocker;
}
QJsonObject FileDialog::state()const{
 const bool open=phase_!=Phase::Idle;
 return {{"open",open},{"mode",open?QJsonValue(modeName(options_.mode)):QJsonValue(QJsonValue::Null)},{"title",open?text(options_.title):QJsonValue(QJsonValue::Null)},
  {"backend",open?text(backend_):QJsonValue(QJsonValue::Null)},{"parentWindow",open&&phase_==Phase::Portal?QJsonValue(parent_):QJsonValue(QJsonValue::Null)},
  {"fallbackReason",open?text(reason_):QJsonValue(QJsonValue::Null)},{"last",last_.isEmpty()?QJsonValue(QJsonValue::Null):QJsonValue(last_)}};
}
}
