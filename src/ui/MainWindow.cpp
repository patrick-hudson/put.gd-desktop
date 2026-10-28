#include "MainWindow.h"
#include <QDir>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QClipboard>
#include <QApplication>
#include <QStandardPaths>
#include "FileDialog.h"
#include <QMenu>
#include <QWidgetAction>
#include <QDropEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QStyle>
#include <QUrl>
#include "SettingsWindow.h"
#include "HistoryWindow.h"
#include "UiSession.h"
#include "VisualStyle.h"
#include "RedesignTheme.h"
#include "Graphite.h"
#include <QPainter>
#include <QMenu>
#include <QHBoxLayout>
#include <QGridLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QJsonArray>
#include <QKeySequence>
#include <QToolButton>
#include <QTabWidget>
#include <QStatusBar>
#include <QTimer>
#include "platform/HostOpen.h"
#include "core/ReleaseLink.h"
namespace putgd {
using Graphite::Space;
using Graphite::space;
namespace {
void orderNavigation(Graphite::WindowHeader* header,QWidget* tabs_,const QList<QToolButton*>& actionButtons_,QWidget* band_){
 QWidget* last=header->menuButton();QWidget::setTabOrder(last,tabs_);last=tabs_;for(auto* button:actionButtons_){QWidget::setTabOrder(last,button);last=button;}
 // Preserve each page's internal order, after the persistent commands; chrome is last.
 QList<QWidget*> content;auto* next=last->nextInFocusChain();while(next!=last){if(next!=header->menuButton()&&next!=tabs_&&!header->isAncestorOf(next)&&!band_->isAncestorOf(next)&&next->focusPolicy()!=Qt::NoFocus)content.append(next);next=next->nextInFocusChain();}
 for(auto* control:content){QWidget::setTabOrder(last,control);last=control;}header->chromeAfter(last);
}
class OutputButton final:public QToolButton {
public:
 explicit OutputButton(QWidget* parent):QToolButton(parent){setMinimumHeight(qMax(32,fontMetrics().height()+4));}
 // The status band keeps a 32 px target and the full body font without form padding.
 QSize sizeHint()const override{auto size=QToolButton::sizeHint();size.setHeight(qMax(32,fontMetrics().height()+4));return size;}
 QSize minimumSizeHint()const override{return {Graphite::scaled(120),sizeHint().height()};}
protected:
 void changeEvent(QEvent* event)override{QToolButton::changeEvent(event);if(event->type()==QEvent::FontChange)setMinimumHeight(qMax(32,fontMetrics().height()+4));}
 void keyPressEvent(QKeyEvent* event)override{if(menu()&&(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)){menu()->popup(mapToGlobal(QPoint(0,-menu()->sizeHint().height())));event->accept();return;}QToolButton::keyPressEvent(event);}
};
class EditSourceButton final:public QToolButton {
public:using QToolButton::QToolButton;
 // Toolbar targets retain six history rows with the 56 px status band.
 QSize sizeHint()const override{auto size=QToolButton::sizeHint();if(window()->height()<Graphite::scaled(720))size.setHeight(qMax(Graphite::Metric::iconTarget,qMax(iconSize().height(),fontMetrics().height()+space(Space::s))));return size;}
 QSize minimumSizeHint()const override{return sizeHint();}
protected:
 void keyPressEvent(QKeyEvent* event)override{if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){click();event->accept();return;}QToolButton::keyPressEvent(event);}
};
QString unavailableOutput(const QJsonObject& document,const QString& action){
 const auto route=Settings::effectiveAction(document,action);const bool local=document["archive"].toObject()["keepUploads"].toBool();
 if(route["destination"]!="upload"||route["profileId"].toString().isEmpty())return local?QString{}:QString("New captures have no output. Choose a destination or turn on Keep a local copy.");
 QJsonObject profile;for(const auto& value:document["profiles"].toArray())if(value.toObject()["id"]==route["profileId"])profile=value.toObject();
 if(profile.isEmpty())return "The upload destination is unavailable. Choose an output in Settings → Capture.";
 if(route["publicationMode"]=="private"&&profile["provider"]!="proton-drive")return "This destination doesn't support private uploads. Choose sharing in Settings → Capture.";
 return {};
}
// Only compact output and build metadata belong in the status band.
class StatusBand final:public QStatusBar {
public:
 explicit StatusBand(QWidget* parent):QStatusBar(parent){setSizeGripEnabled(false);setObjectName("statusBand");setMinimumHeight(Graphite::scaled(Graphite::Metric::statusBand));setContentsMargins(space(Space::l),space(Space::s),space(Space::l),space(Space::s));}
protected:
 bool event(QEvent* event)override{
  const bool handled=QStatusBar::event(event);
  if(event->type()==QEvent::LayoutRequest||event->type()==QEvent::Show){int height=Graphite::scaled(32);for(auto* child:findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(!child->isWindow()&&child->isVisibleTo(this))height=qMax(height,qMax(child->minimumHeight(),child->sizeHint().height()));setMinimumHeight(qMax(Graphite::scaled(Graphite::Metric::statusBand),height+2*space(Space::s)));}
  return handled;
 }
 void paintEvent(QPaintEvent* event)override{{QPainter p(this);p.fillRect(rect(),Theme::current().surface);p.setPen(Theme::current().border);p.drawLine(0,0,width(),0);}QStatusBar::paintEvent(event);}
};
}
MainWindow::MainWindow(UiSession& session,QWidget* parent):QMainWindow(parent),session_(session){
 chooser_=new FileDialog(this);
 setObjectName("putgdMainWindow");setWindowTitle("put.gd");setWindowIcon(VisualStyle::icon("brand"));resize(1320,880);setMinimumSize(1060,680);
 const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Upload clipboard"},{"edit","Capture region & edit"},{"record","Record screen"}};
 const QHash<QString,QString> glyphs{{"region","region"},{"window","window"},{"desktop","desktop"},{"clipboard","clipboard"},{"edit","edit"},{"record","record"}};
 // The application menu holds only what the window does not already show:
 // help, updates, About and Quit. Capture commands live in the command band
 // and the pages in the header's navigation, once each.
 auto* menu=new QMenu(this);menu->setObjectName("applicationMenuPopup");
 auto* helpAction=menu->addAction("Capture actions");helpAction->setObjectName("captureActionsHelp");
 auto* updatesAction=menu->addAction("Check for updates");updatesAction->setObjectName("checkForUpdates");
 // Opens General → Updates and starts its Check now, where checks are
 // supported; the result shows there.
 connect(updatesAction,&QAction::triggered,this,[this]{navigate("settings");settings_->showUpdates();if(auto* now=settings_->findChild<QPushButton*>("checkForUpdatesNow");now&&now->isEnabled())now->click();});
 connect(menu->addAction("About put.gd"),&QAction::triggered,this,&MainWindow::aboutRequested);
 menu->addSeparator();
 auto* quitAction=menu->addAction("Quit put.gd");quitAction->setObjectName("quitApplication");connect(quitAction,&QAction::triggered,this,&MainWindow::quitRequested);
 auto* header=Graphite::WindowHeader::install(this,nullptr,menu);
 tabs_=new Graphite::Segmented(header);tabs_->setObjectName("mainNavigation");tabs_->setAccessibleName("Main navigation");for(const auto* label:{"History","Destinations","Settings"})tabs_->addTab(label);header->setCentre(tabs_);
 auto* center=new QWidget;setCentralWidget(center);auto* layout=new QVBoxLayout(center);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 // The command band: capture and upload actions, lead first.
 band_=new Graphite::Surface("bg",{},center);band_->setObjectName("commandBand");band_->setAccessibleName("Capture and upload actions");band_->installEventFilter(this);auto* bandRow=new QGridLayout(band_);bandRow->setContentsMargins(space(Space::m),space(Space::s),space(Space::m),space(Space::s));bandRow->setSpacing(space(Space::s));layout->addWidget(band_);
 const QHash<QString,QString> explanations{{"region","Drag to select an area of your screen. Press Escape to cancel."},{"window","Capture the focused external app window, including its frame where supported."},{"desktop","Capture all enabled monitors, including desktop panels."},{"clipboard","Upload copied local files, an image, or plain text, in that order. Text URLs are uploaded as text."},{"edit","Select a region, then redact, blur, crop or annotate it. Nothing uploads until you choose Upload.\n\nUse its menu to edit the clipboard image or an image file instead, or drop a PNG or JPEG on this button."},{"record","Drag to select an area, then use the floating bar to record, pause and stop. Stop opens a review window; nothing uploads until you choose Upload there."}};
 const auto actionHelp=[&]{QStringList help;for(const auto& action:Settings::presentationOrder())help<<labels.value(action)+": "+explanations.value(action);help<<"Each button starts that action after finished settings edits are saved. Screen captures close this window to keep it out of the capture; clipboard uploads keep it open. Store or clear unfinished credentials before a screen capture. Settings → Capture selects the upload destination, and Keep a local copy independently enables local saving. Both can be enabled. With neither enabled, capture actions are unavailable. Shared uploads copy a link; private uploads create no link. Select the output summary in the status bar to see each action's output behavior. Results and recovery actions appear in History.";return help.join("\n\n");}();
 // Three groups, each action once: the screenshots (Capture region is the
 // lead), then editing and recording, and at the far end the clipboard
 // upload, which captures nothing. Hairlines separate the groups.
 const auto bandOrder=Settings::presentationOrder();
 for(const auto& action:bandOrder){
  if(action=="edit"){auto* divider=Graphite::rule(band_,Qt::Vertical);divider->setObjectName("commandBandDivider");divider->setFixedHeight(Graphite::scaled(20));bandRow->addWidget(divider,0,3,Qt::AlignCenter);}
  if(action=="clipboard")bandRow->setColumnStretch(6,1);
  auto* button=new EditSourceButton(band_);button->setText(QString(labels.value(action)).replace('&',QStringLiteral("&&")));button->setFocusPolicy(Qt::StrongFocus);button->setIconSize(QSize(Graphite::scaled(Graphite::Metric::icon),Graphite::scaled(Graphite::Metric::icon)));button->setIcon(VisualStyle::icon(glyphs.value(action)));button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);VisualStyle::role(button,action=="region"?"lead":"quiet");button->setObjectName("captureActionButton");button->setProperty("captureAction",action);button->setAccessibleName(labels.value(action));
  if(action=="edit"){
   // The button captures a region to edit; its menu edits an image that
   // already exists instead and never starts a capture. A right-click or a
   // dropped PNG or JPEG does the same.
   editButton_=button;button->setContextMenuPolicy(Qt::CustomContextMenu);button->setAcceptDrops(true);button->installEventFilter(this);
   auto* sources=new QMenu(this);sources->setObjectName("editSourceMenu");
   connect(sources->addAction(VisualStyle::icon("clipboard"),"Edit clipboard image"),&QAction::triggered,this,[this]{emit editRequested("clipboard",{});});
   connect(sources->addAction(VisualStyle::icon("images"),"Edit an image file…"),&QAction::triggered,this,&MainWindow::chooseImageToEdit);
   button->setMenu(sources);button->setPopupMode(QToolButton::MenuButtonPopup);
   connect(button,&QWidget::customContextMenuRequested,sources,[sources,button](const QPoint& at){sources->popup(button->mapToGlobal(at));});
  }
  auto update=[this,button,action,explanations,glyphs]{
   const auto saved=Settings::effectiveAction(session_.document(),action);const auto key=QKeySequence(saved["shortcut"].toString()).toString(QKeySequence::NativeText);
   const auto unavailable=unavailableOutput(session_.document(),action);button->setEnabled(true);const auto defaults=session_.document()["captureDefaults"].toObject();button->setProperty("captureUnavailable",!unavailable.isEmpty());button->setProperty("captureBadge",!unavailable.isEmpty()&&!defaults.isEmpty()&&(saved["destination"]!=defaults["destination"]||saved["profileId"]!=defaults["profileId"]||saved["publicationMode"]!=defaults["publicationMode"]));if(action=="region")VisualStyle::role(button,"lead");button->setIcon(VisualStyle::icon(glyphs.value(action)));
   button->setAccessibleDescription(unavailable.isEmpty()?explanations.value(action):(action=="edit"?"Region capture unavailable: "+unavailable+" Existing images can still be edited.":"Unavailable: "+unavailable));
   button->setToolTip(button->accessibleName()+(key.isEmpty()?QString{}:QString(" · ")+key)+(unavailable.isEmpty()?QString{}:QString("\n")+unavailable));
  };
  connect(&session_,&UiSession::accepted,button,update);update();connect(button,&QToolButton::clicked,this,[this,action]{const auto reason=unavailableOutput(session_.document(),action);if(reason.isEmpty())emit actionRequested(action);else showActionFailure(action,{"output_required",reason,{}});});bandRow->addWidget(button,0,action=="region"?0:action=="window"?1:action=="desktop"?2:action=="edit"?4:action=="record"?5:7,Qt::AlignLeft);actionButtons_<<button;
 }
 connect(helpAction,&QAction::triggered,this,[this,actionHelp]{VisualStyle::showHelp("Capture actions",actionHelp,this);});
 outputWarning_=new Graphite::Surface("surface",Qt::BottomEdge,center);outputWarning_->setObjectName("commandOutputWarning");auto* warningRow=new QHBoxLayout(outputWarning_);warningRow->setContentsMargins(space(Space::l),space(Space::s),space(Space::m),space(Space::s));warningRow->setSpacing(space(Space::m));outputWarningText_=new QLabel(outputWarning_);outputWarningText_->setObjectName("commandOutputWarningText");outputWarningText_->setTextFormat(Qt::PlainText);outputWarningText_->setWordWrap(true);VisualStyle::role(outputWarningText_,"warning");warningRow->addWidget(outputWarningText_,1);auto* chooseOutput=new QPushButton("Change output",outputWarning_);chooseOutput->setObjectName("chooseCommandOutput");chooseOutput->setToolTip("Open Settings → Capture to choose the output");warningRow->addWidget(chooseOutput);connect(chooseOutput,&QPushButton::clicked,this,[this]{navigate("settings");QStringList affected;for(const auto& action:Settings::presentationOrder())if(!unavailableOutput(session_.document(),action).isEmpty())affected<<action;settings_->showCaptureSettings(affected);});layout->addWidget(outputWarning_);connect(&session_,&UiSession::accepted,this,&MainWindow::updateAvailability);updateAvailability();
 // Edit an image file… whose chooser could not open: the reason under the band
 // that holds the action, with Try again. A cancel shows nothing.
 chooserFailure_=new Graphite::Surface("surface",Qt::BottomEdge,center);chooserFailure_->setObjectName("editImageChooserFailure");{auto* row=new QHBoxLayout(chooserFailure_);row->setContentsMargins(space(Space::l),space(Space::s),space(Space::m),space(Space::s));row->setSpacing(space(Space::m));
  chooserError_=new QLabel(chooserFailure_);chooserError_->setObjectName("editImageChooserError");chooserError_->setTextFormat(Qt::PlainText);chooserError_->setWordWrap(true);row->addWidget(chooserError_,1);
  auto* retry=new QPushButton("Try again",chooserFailure_);retry->setObjectName("retryEditImageChooser");row->addWidget(retry);connect(retry,&QPushButton::clicked,this,[this]{chooseImageToEdit();});
  auto* dismiss=Graphite::toolButton({},{},"quiet",chooserFailure_);dismiss->setObjectName("dismissEditImageChooserFailure");dismiss->setIcon(Graphite::chromeIcon("close"));dismiss->setAccessibleName("Dismiss");dismiss->setToolTip("Dismiss");row->addWidget(dismiss);connect(dismiss,&QToolButton::clicked,chooserFailure_,&QWidget::hide);}
 layout->addWidget(chooserFailure_);chooserFailure_->hide();
 pages_=new QStackedWidget;pages_->setObjectName("mainPages");layout->addWidget(pages_,1);history_=new HistoryWindow(session_,pages_);pages_->addWidget(history_);
 // Keep the navigation indexes stable without constructing hidden Settings and
 // destination controls for a History-only visit (#558).
 pages_->addWidget(new QWidget(pages_));pages_->addWidget(new QWidget(pages_));
 connect(tabs_,&QTabBar::currentChanged,this,[this](int row){if(settings_)settings_->clearCompletedFeedback();if(row>0)settings();pages_->setCurrentIndex(row);if(row==1)settings_->activateDestinations();updateAvailability();emit viewStateChanged();});
 connect(history_,&HistoryWindow::viewStateChanged,this,&MainWindow::viewStateChanged);connect(history_,&HistoryWindow::editRequested,this,&MainWindow::editRequested);connect(history_,&HistoryWindow::chooseImageRequested,this,&MainWindow::chooseImageToEdit);
 setStatusBar(new StatusBand(this));statusBar()->installEventFilter(this);
 // One output control; the popup owns the per-action details and navigation.
 auto* outputGroup=outputGroup_=new QWidget;outputGroup->setObjectName("captureOutputGroup");auto* outputRow=new QHBoxLayout(outputGroup);outputRow->setContentsMargins(0,0,0,0);outputRow->setSpacing(0);
 destination_=new OutputButton(outputGroup);destination_->setFocusPolicy(Qt::StrongFocus);VisualStyle::role(destination_,"quiet");destination_->setObjectName("captureOutputSummary");destination_->setToolButtonStyle(Qt::ToolButtonTextOnly);destination_->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);destination_->setMinimumWidth(Graphite::scaled(120));destination_->setPopupMode(QToolButton::InstantPopup);outputRow->addWidget(destination_,1);outputRow->addStretch(0);
 auto* outputMenu=new QMenu(destination_);outputMenu->setObjectName("captureOutputMenu");destination_->setMenu(outputMenu);const auto info=[outputMenu](const QString& name){auto* entry=new QWidgetAction(outputMenu);entry->setObjectName(name);auto* label=new QLabel;label->setTextFormat(Qt::PlainText);label->setWordWrap(true);label->setMinimumWidth(Graphite::scaled(280));label->setMaximumWidth(Graphite::scaled(480));label->setContentsMargins(space(Space::m),space(Space::s),space(Space::m),space(Space::s));label->setFocusPolicy(Qt::NoFocus);VisualStyle::role(label,"muted");entry->setDefaultWidget(label);connect(entry,&QAction::changed,label,[entry,label]{label->setText(entry->text());});outputMenu->addAction(entry);};info("captureOutputDefault");for(const auto& action:Settings::presentationOrder())info("captureOutputDetail"+action);outputMenu->addSeparator();auto* changeOutput=outputMenu->addAction("Change output");changeOutput->setObjectName("changeCaptureOutput");
 statusBar()->addWidget(outputGroup,1);updateAvailability();
 // The running build, from the resident identity About also shows. Stable and
 // nightly builds link to their GitHub release; development builds are text.
 version_=new QLabel;version_->setObjectName("appVersion");version_->setTextFormat(Qt::PlainText);VisualStyle::role(version_,"meta");
 versionLink_=new QPushButton;versionLink_->setObjectName("appVersionLink");VisualStyle::role(versionLink_,"version");versionLink_->setCursor(Qt::PointingHandCursor);
 auto* versionGroup=new QWidget;versionGroup->setObjectName("appVersionGroup");auto* versionLayout=new QHBoxLayout(versionGroup);versionLayout->setContentsMargins(0,0,0,0);versionLayout->addWidget(versionLink_);
 auto* versionNotice=new Graphite::Surface("surface",Qt::BottomEdge,center);versionNotice->setObjectName("appVersionNotification");auto* noticeRow=new QHBoxLayout(versionNotice);noticeRow->setContentsMargins(space(Space::l),space(Space::s),space(Space::m),space(Space::s));noticeRow->setSpacing(space(Space::m));
 auto* versionBrowser=new Graphite::BrowserFeedback(versionNotice);versionBrowser->setObjectName("appVersionBrowserFeedback");auto* feedbackLayout=versionBrowser->layout();delete feedbackLayout;auto* feedbackRow=new QHBoxLayout(versionBrowser);feedbackRow->setContentsMargins(0,0,0,0);feedbackRow->setSpacing(space(Space::m));feedbackRow->addWidget(versionBrowser->findChild<QLabel*>(),1);auto* copyRelease=versionBrowser->findChild<QPushButton*>();copyRelease->setText("Copy release link");copyRelease->setObjectName("copyReleaseLink");feedbackRow->addWidget(copyRelease);noticeRow->addWidget(versionBrowser,1);
 auto* dismissVersion=Graphite::toolButton({},{},"quiet",versionNotice);dismissVersion->setObjectName("dismissVersionNotification");dismissVersion->setIcon(Graphite::chromeIcon("close"));dismissVersion->setAccessibleName("Dismiss release link notification");dismissVersion->setToolTip("Dismiss");noticeRow->addWidget(dismissVersion);connect(dismissVersion,&QToolButton::clicked,versionBrowser,&Graphite::BrowserFeedback::clear);versionBrowser->installEventFilter(this);versionNotice->hide();layout->insertWidget(layout->indexOf(pages_),versionNotice);
 connect(versionLink_,&QPushButton::clicked,this,[this,versionBrowser]{if(const auto url=releaseLink(session_.build()).url;url.isValid())host::openLink(url,versionBrowser->opening(url));});
 auto updateVersion=[this,versionGroup,versionBrowser]{
  const auto link=releaseLink(session_.build());const auto name="put.gd version "+link.version;const auto page="Open the v"+link.version+" release on GitHub";
  version_->setText(link.version);version_->setAccessibleName(name);version_->setToolTip(name);version_->setVisible(!link.version.isEmpty()&&link.url.isEmpty());
  versionLink_->setText(link.version);versionLink_->setAccessibleName(name);versionLink_->setAccessibleDescription(page);versionLink_->setToolTip(page);versionLink_->setVisible(!link.url.isEmpty());versionGroup->setVisible(!link.url.isEmpty());if(versionLink_->property("browserUrl").toUrl()!=link.url){versionBrowser->clear();versionLink_->setProperty("browserUrl",link.url);}
 };
 connect(&session_,&UiSession::statusChanged,this,updateVersion);updateVersion();
 // A newer version, from the resident's daily check (#293): it opens
 // General → Updates, which has the download.
 updateLink_=new QPushButton(VisualStyle::icon("download"),"Update available");updateLink_->setObjectName("updateAvailable");VisualStyle::role(updateLink_,"version");updateLink_->setCursor(Qt::PointingHandCursor);updateLink_->hide();
 connect(updateLink_,&QPushButton::clicked,this,[this]{navigate("settings");settings_->showUpdates();});
 auto updateAvailable=[this]{
  const auto update=session_.diagnostics()["update"].toObject();const auto result=update["result"].toObject();
  const bool available=update["supported"].toBool()&&result["updateAvailable"].toBool();
  updateLink_->setToolTip("put.gd "+result["latest"].toString()+" is available. Open Settings → General → Updates.");
  updateLink_->setAccessibleName("Update available: put.gd "+result["latest"].toString());updateLink_->setVisible(available);updateSaveStatus();
 };
 connect(&session_,&UiSession::statusChanged,this,updateAvailable);updateAvailable();
 auto updateDestination=[this]{
  const auto document=session_.document();QString label;
  auto outputName=[&](const QJsonObject& output){const bool local=document["archive"].toObject()["keepUploads"].toBool();if(output["destination"]=="local"||output["profileId"].toString().isEmpty())return local?QString("Local only"):QString("Nothing is saved or uploaded");for(const auto& value:document["profiles"].toArray()){const auto profile=value.toObject();if(profile["id"]!=output["profileId"])continue;const auto mode=output["publicationMode"].toString("default");const bool privateUpload=mode=="private"||(mode=="default"&&profile["provider"]=="proton-drive"&&!profile["settings"].toObject()["shareByDefault"].toBool(true));return (privateUpload?(profile["provider"]=="proton-drive"?QString("Private upload"):QString("Private upload unavailable")):QString("Shared link"))+" · "+profile["label"].toString()+(local?QString(" + local copy"):QString{});}return QString("Choose a destination");};
  const auto defaults=document.contains("captureDefaults")?document["captureDefaults"].toObject():Settings::effectiveAction(document,"region");
  const auto defaultName=outputName(defaults);QStringList details{"Default output: "+defaultName+(!document["archive"].toObject()["keepUploads"].toBool()&&defaults["destination"]=="upload"&&!defaults["profileId"].toString().isEmpty()?QString(" · No local copy"):QString{})};bool mixed=false;
  const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"edit","Capture region & edit"},{"clipboard","Upload clipboard"},{"record","Record screen"}};
  for(const auto& action:Settings::presentationOrder()){
   const auto route=Settings::effectiveAction(document,action);const auto output=outputName(route);
   const bool differs=output!=defaultName||route["profileId"].toString()!=defaults["profileId"].toString();mixed|=differs;
   auto* entry=destination_->menu()->findChild<QAction*>("captureOutputDetail"+action);entry->setVisible(true);
   const bool upload=route["destination"]=="upload"&&!route["profileId"].toString().isEmpty();const auto timing=action=="edit"||action=="record"?(upload?QString("Upload when you choose"):QString("Save when you choose")):upload?QString("Uploads right away"):QString("Saves locally");const auto detail=labels.value(action,action)+": "+(output=="Nothing is saved or uploaded"?output:timing+" · "+output+(!document["archive"].toObject()["keepUploads"].toBool()&&upload?QString(" · No local copy"):QString{}));entry->setText(detail);details<<detail;
   for(auto* button:actionButtons_)if(button->property("captureAction")==action){button->setProperty("outputSuffix",QString{});button->setProperty("commandLabel",labels.value(action));const auto reason=unavailableOutput(document,action);const bool ownOutput=route["destination"]!=defaults["destination"]||route["profileId"]!=defaults["profileId"]||route["publicationMode"]!=defaults["publicationMode"];button->setProperty("captureUnavailable",!reason.isEmpty());button->setProperty("captureBadge",!reason.isEmpty()&&ownOutput);button->setToolTip(button->toolTip().section('\n',0,0)+"\n"+detail+(reason.isEmpty()?QString{}:QString("\n")+reason));button->setAccessibleDescription(button->accessibleDescription().section("\nOutput: ",0,0)+"\nOutput: "+output);}


  }
  if(!mixed)details[0].replace(0,QString("Default output").size(),"All actions");
  label=mixed?QString("Mixed outputs"):defaultName=="Local only"?QString("Captures save locally"):defaultName=="Nothing is saved or uploaded"?QString("Captures have no output"):"Captures upload right away · "+defaultName;
  fitBand();destination_->setProperty("fullOutputSummary",label);updateOutputSummary();destination_->menu()->findChild<QAction*>("captureOutputDefault")->setText(details.first());destination_->setAccessibleName(label);destination_->setAccessibleDescription(details.join("\n"));destination_->setToolTip(details.join("\n"));updateSaveStatus();
 };
 connect(changeOutput,&QAction::triggered,this,[this]{navigate("settings");settings_->showCaptureSettings();});
 connect(history_,&HistoryWindow::captureSettingsRequested,changeOutput,&QAction::trigger);
 connect(history_,&HistoryWindow::localCopySettingsRequested,this,&MainWindow::showLocalCopySettings);
 explanation_=new QLabel("Settings save automatically");explanation_->setObjectName("autosaveExplanation");VisualStyle::role(explanation_,"meta");statusBar()->addPermanentWidget(explanation_);
 // Autosave describes the Destinations and Settings pages, not History.
 connect(tabs_,&QTabBar::currentChanged,this,[this]{updateSaveStatus();});
 saveStatus_=new QLabel;saveStatus_->setObjectName("autosaveStatus");saveStatus_->setAccessibleName("Settings save status");saveStatus_->setTextFormat(Qt::PlainText);VisualStyle::role(saveStatus_,"meta");statusBar()->addPermanentWidget(saveStatus_);
 statusBar()->addPermanentWidget(updateLink_);statusBar()->addPermanentWidget(version_);statusBar()->addPermanentWidget(versionGroup);
 connect(&session_,&UiSession::accepted,this,[this,updateDestination]{updateDestination();updateSaveStatus();});
 updateDestination();updateSaveStatus();tabs_->setCurrentIndex(0);RedesignTheme::apply(*this);
 orderNavigation(header,tabs_,actionButtons_,band_);
 connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,[this]{QTimer::singleShot(0,this,[this]{fitBand();});});installEventFilter(this);
}
SettingsWindow* MainWindow::settings(){
 if(settings_)return settings_;
 settings_=new SettingsWindow(session_,pages_);
 auto* destinations=pages_->widget(1);auto* preferences=pages_->widget(2);
 pages_->removeWidget(destinations);pages_->removeWidget(preferences);delete destinations;delete preferences;
 pages_->addWidget(settings_->destinationsPage());pages_->addWidget(settings_);
 orderNavigation(findChild<Graphite::WindowHeader*>("windowHeader"),tabs_,actionButtons_,band_);
 // New combo popup containers need the same themed delegate/margin refresh
 // that the original eager tree received when the main style was applied.
 RedesignTheme::apply(*this);
 connect(settings_,&SettingsWindow::viewStateChanged,this,[this]{updateSaveStatus();updateAvailability();emit viewStateChanged();});
 connect(settings_,&SettingsWindow::actionRequested,this,&MainWindow::actionRequested);
 connect(settings_,&SettingsWindow::quitRequested,this,&MainWindow::quitRequested);
 connect(settings_,&SettingsWindow::destinationsRequested,this,[this]{navigate("destinations");});
 emit settingsCreated(settings_);
 updateSaveStatus();return settings_;
}
void MainWindow::updateOutputSummary(){
 if(!destination_)return;destination_->setText(destination_->property("fullOutputSummary").toString().replace('&',QStringLiteral("&&")));
}
// One bounded line: a conflict or a refused save reads "Changes not saved" (the
// page's banner explains it and offers Reload or Retry), and the band never
// says settings save automatically while they don't.
void MainWindow::updateSaveStatus(){
 if(!saveStatus_||!explanation_)return;
 const auto text=settings_?settings_->autosaveText():QString();const bool unsaved=settings_&&settings_->changesUnsaved();
 VisualStyle::feedback(saveStatus_,unsaved?QString("Changes not saved"):text,unsaved);saveStatus_->setToolTip(unsaved?text:QString());saveStatus_->setAccessibleDescription(unsaved?text:QString());
 // "Settings save automatically" is the band's least important line: it gives
 // way before the output summary has to elide (narrow windows, large text).
 const int gap=qMax(statusBar()->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing,nullptr,statusBar()),6);
 const auto link=releaseLink(session_.build());auto* versionGroup=findChild<QWidget*>("appVersionGroup");
 int needed=(outputGroup_->isHidden()?0:destination_->sizeHint().width()+gap)+24;
 for(auto* widget:statusBar()->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(widget!=outputGroup_&&widget!=explanation_&&widget!=version_&&widget!=versionGroup&&!widget->isWindow()&&widget->isVisibleTo(statusBar()))needed+=widget->sizeHint().width()+gap;
 const int versionWidth=link.url.isEmpty()?version_->sizeHint().width():versionGroup->sizeHint().width();
 const bool versionRoom=needed+versionWidth+gap<=statusBar()->width();
 version_->setVisible(versionRoom&&!link.version.isEmpty()&&link.url.isEmpty());versionGroup->setVisible(versionRoom&&!link.url.isEmpty());
 explanation_->setVisible(tabs_->currentIndex()>0&&!unsaved&&needed+(versionRoom?versionWidth+gap:0)+explanation_->sizeHint().width()+gap<=statusBar()->width());
}
// Compact commands keep their task labels; output details stay in the status menu.
void MainWindow::fitBand(){
 if(!band_||fittingBand_)return;fittingBand_=true;
 // A short window keeps one 4 px inset around the measured navigation track.
 if(auto* header=findChild<Graphite::WindowHeader*>("windowHeader"))header->setFixedHeight(height()<Graphite::scaled(720)?qMax(Graphite::scaled(48),tabs_->sizeHint().height()+2*space(Space::xs)):Graphite::scaled(Graphite::Metric::header));
 auto* grid=static_cast<QGridLayout*>(band_->layout());
 auto apply=[this,grid](int stage){grid->setHorizontalSpacing(space(stage>=2?Space::xs:Space::s));grid->setContentsMargins(space(Space::m),space(Space::s),space(Space::m),space(Space::s));for(auto* button:actionButtons_){button->setMinimumWidth(0);}for(auto* button:actionButtons_){const auto action=button->property("captureAction").toString();button->setToolButtonStyle(stage==3&&(action=="window"||action=="desktop")?Qt::ToolButtonIconOnly:Qt::ToolButtonTextBesideIcon);const auto label=stage&&(action=="window"||action=="desktop")?(action=="window"?QString("Window"):QString("Desktop")):button->property("commandLabel").toString();if(!label.isEmpty())button->setText(QString(label).replace('&',QStringLiteral("&&")));}grid->invalidate();grid->activate();};
 int stage=0;for(;stage<4;++stage){apply(stage);if(grid->sizeHint().width()<=band_->width()||stage==3)break;}
 for(auto* button:actionButtons_)button->setMinimumWidth(button->sizeHint().width());bandStage_=stage;fittingBand_=false;
}
void MainWindow::updateAvailability(){
 QStringList unavailable,reasons;for(auto* button:actionButtons_){const auto reason=unavailableOutput(session_.document(),button->property("captureAction").toString());if(reason.isEmpty())continue;unavailable<<button->accessibleName();if(!reasons.contains(reason))reasons<<reason;}
 const auto message=unavailable.isEmpty()?QString{}:unavailable.size()==actionButtons_.size()?reasons.join(" "):"Choose an output for: "+unavailable.join(", ")+".";outputWarningText_->setText(message);outputWarningText_->setAccessibleDescription(message+" Opens Settings → Capture.");const auto* recovery=findChild<QWidget*>("actionRecoveryNotification");const bool capture=tabs_&&tabs_->currentIndex()==2&&settings_&&settings_->findChild<QTabWidget*>("settingsTabs")->currentIndex()==1;outputWarning_->setVisible(!(settings_&&settings_->changesUnsaved())&&!capture&&!unavailable.isEmpty()&&!(recovery&&!recovery->isHidden()&&recovery->property("outputRecovery").toBool()));if(outputGroup_)outputGroup_->setVisible(!capture);
}
void MainWindow::showActionFailure(const QString& action,const Error& error){
 const bool output=error.code=="output_required",local=error.code=="archive_failed";
 if(output)showCaptureSettings({action});else if(local)showLocalCopySettings();else {navigate("destinations");const auto route=Settings::effectiveAction(session_.document(),action);auto* profiles=findChild<QListWidget*>("destinationList");const auto destinations=session_.document()["profiles"].toArray();QString provider;for(int i=0;i<destinations.size();++i)if(destinations[i].toObject()["id"]==route["profileId"]){if(profiles)profiles->setCurrentRow(i);provider=destinations[i].toObject()["provider"].toString();}const auto target=(provider=="s3"||provider=="backblaze-b2")?QString("s3Bucket"):provider=="proton-drive"?QString("protonFolder"):QString("destinationLabel");QTimer::singleShot(0,this,[this,target]{auto* control=findChild<QWidget*>(target);if(!control||!control->isVisible())control=findChild<QWidget*>("destinationLabel");if(control){if(auto* scroll=findChild<QScrollArea*>("destinationScroll"))scroll->ensureWidgetVisible(control,24,24);control->setFocus(Qt::OtherFocusReason);control->setAttribute(Qt::WA_KeyboardFocusChange);}});}
 auto* notice=findChild<QWidget*>("actionRecoveryNotification");if(!notice){notice=new Graphite::Surface("surface",Qt::BottomEdge,centralWidget());notice->setObjectName("actionRecoveryNotification");auto* row=new QHBoxLayout(notice);row->setContentsMargins(space(Space::l),space(Space::s),space(Space::m),space(Space::s));row->setSpacing(space(Space::m));auto* text=new QLabel(notice);text->setObjectName("actionRecoveryError");text->setTextFormat(Qt::PlainText);text->setWordWrap(true);VisualStyle::role(text,"danger");row->addWidget(text,1);auto* retry=new QPushButton(notice);retry->setObjectName("actionRecoveryLink");row->addWidget(retry);connect(retry,&QPushButton::clicked,this,[this,retry]{const auto action=retry->property("action").toString(),page=retry->property("page").toString();if(page=="capture")showCaptureSettings({action});else if(page=="local")showLocalCopySettings();else {const Error error{retry->property("code").toString(),retry->property("message").toString(),{}};showActionFailure(action,error);}});auto* dismiss=Graphite::toolButton({},{},"quiet",notice);dismiss->setIcon(Graphite::chromeIcon("close"));dismiss->setObjectName("dismissActionRecovery");dismiss->setAccessibleName("Dismiss action error");row->addWidget(dismiss);connect(dismiss,&QToolButton::clicked,this,[this,notice]{notice->hide();updateAvailability();});auto* box=static_cast<QVBoxLayout*>(centralWidget()->layout());box->insertWidget(box->indexOf(pages_),notice);connect(tabs_,&QTabBar::currentChanged,this,[this,notice]{notice->hide();updateAvailability();});}
 notice->findChild<QLabel*>("actionRecoveryError")->setText(error.message);auto* retry=notice->findChild<QPushButton*>("actionRecoveryLink");retry->setText(output?"Change output":local?"Keep local copies":"Review destination");retry->setProperty("action",action);retry->setProperty("code",error.code);retry->setProperty("message",error.message);retry->setProperty("page",output?"capture":local?"local":"destinations");notice->setProperty("outputRecovery",output);notice->show();updateAvailability();
}
void MainWindow::showCaptureSettings(const QStringList& actions){navigate("settings");settings_->showCaptureSettings(actions);}
void MainWindow::showLocalCopySettings(){navigate("settings");settings_->showLocalCopySettings();}
MainWindow::~MainWindow(){
 if(settings_)disconnect(settings_,nullptr,this,nullptr);disconnect(history_,nullptr,this,nullptr);disconnect(tabs_,nullptr,this,nullptr);disconnect(&session_,nullptr,this,nullptr);
}
bool MainWindow::chooseImageToEdit(){
 auto folder=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);if(folder.isEmpty())folder=QDir::homePath();
 return chooser_->open({FileDialog::Mode::Open,"Edit an image file",folder,{},{{"Images",{"*.png","*.jpg","*.jpeg"}}}},[this](const FileDialog::Result& result){imageChooserFinished(result);});
}
void MainWindow::imageChooserFinished(const FileDialog::Result& result){
 chooserFailure_->setVisible(result.failed());
 if(result.failed())VisualStyle::feedback(chooserError_,"Edit an image file: the file chooser couldn't open"+(result.error.isEmpty()?QString("."):": "+result.error.left(1).toLower()+result.error.mid(1)+(result.error.endsWith('.')?"":"."))+" Nothing was opened.",true);
 if(result.chosen())emit editRequested("file",result.path);
}
QString MainWindow::droppedImage(const QMimeData* data){
 if(!data||data->urls().size()!=1||!data->urls().first().isLocalFile())return {};
 const QFileInfo file(data->urls().first().toLocalFile());
 return file.isFile()&&QStringList{"png","jpg","jpeg"}.contains(file.suffix().toLower())?file.absoluteFilePath():QString{};
}
bool MainWindow::eventFilter(QObject* watched,QEvent* event){
 if(watched->objectName()=="appVersionBrowserFeedback"&&(event->type()==QEvent::ShowToParent||event->type()==QEvent::HideToParent))static_cast<QWidget*>(watched)->parentWidget()->setVisible(event->type()==QEvent::ShowToParent);
 if((watched==this||watched==band_)&&event->type()==QEvent::Resize)fitBand();
 if(watched==statusBar()&&event->type()==QEvent::Resize){updateOutputSummary();updateSaveStatus();}
 if(watched!=editButton_)return QMainWindow::eventFilter(watched,event);
 auto highlight=[this](bool on){editButton_->setProperty("dropTarget",on);editButton_->style()->unpolish(editButton_);editButton_->style()->polish(editButton_);};
 switch(event->type()){
 case QEvent::DragEnter:case QEvent::DragMove:{auto* drag=static_cast<QDragMoveEvent*>(event);const bool image=!droppedImage(drag->mimeData()).isEmpty();if(image)drag->acceptProposedAction();else drag->ignore();if(event->type()==QEvent::DragEnter)highlight(image);return true;}
 case QEvent::DragLeave:highlight(false);return true;
 case QEvent::Drop:{auto* drop=static_cast<QDropEvent*>(event);highlight(false);const auto path=droppedImage(drop->mimeData());if(path.isEmpty()){drop->ignore();return true;}drop->acceptProposedAction();emit editRequested("file",path);return true;}
 default:return QMainWindow::eventFilter(watched,event);
 }
}
void MainWindow::navigate(const QString& name){tabs_->setCurrentIndex(name=="settings"?2:name=="destinations"?1:0);if(name=="activity"||name=="history")history_->refresh();}
QJsonObject MainWindow::saveViewState()const{return {{"page",tabs_->currentIndex()},{"geometry",QJsonArray{x(),y(),width(),height()}}};}
bool MainWindow::restoreViewState(const QJsonObject& state,bool restorePage){auto geometry=state["geometry"].toArray();if(geometry.size()!=4)return false;setGeometry(geometry[0].toInt(),geometry[1].toInt(),geometry[2].toInt(),geometry[3].toInt());if(restorePage)tabs_->setCurrentIndex(qBound(0,state["page"].toInt(),2));return true;}
}
