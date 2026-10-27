#include "MainWindow.h"
#include <QDir>
#include <QStandardPaths>
#include "FileDialog.h"
#include <QMenu>
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
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QScrollArea>
#include <QStackedWidget>
#include <QJsonArray>
#include <QKeySequence>
#include <QToolButton>
#include <QStatusBar>
#include <QTimer>
#include "platform/HostOpen.h"
#include "core/ReleaseLink.h"
namespace putgd {
namespace {
QString unavailableOutput(const QJsonObject& document,const QString& action){
 const auto route=Settings::effectiveAction(document,action);const bool local=document["archive"].toObject()["keepUploads"].toBool();
 if(route["destination"]!="upload"||route["profileId"].toString().isEmpty())return local?QString{}:QString("No output is enabled. Choose a destination or turn on Keep a local copy in Settings → Capture.");
 QJsonObject profile;for(const auto& value:document["profiles"].toArray())if(value.toObject()["id"]==route["profileId"])profile=value.toObject();
 if(profile.isEmpty())return "The upload destination is unavailable. Choose an output in Settings → Capture.";
 if(route["publicationMode"]=="private"&&profile["provider"]!="proton-drive")return "This destination does not support private uploads. Choose sharing in Settings → Capture.";
 return {};
}
// The status band grows with its output summary and metadata.
class StatusBand final:public QStatusBar {
public:
 explicit StatusBand(QWidget* parent):QStatusBar(parent){setSizeGripEnabled(false);setObjectName("statusBand");setMinimumHeight(Graphite::scaled(Graphite::Metric::statusBand));setContentsMargins(8,0,8,0);}
protected:
 bool event(QEvent* event)override{
  const bool handled=QStatusBar::event(event);
  if(event->type()==QEvent::LayoutRequest||event->type()==QEvent::Show){int height=32;for(auto* child:findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(!child->isWindow()&&child->isVisibleTo(this))height=qMax(height,qMax(child->minimumHeight(),child->sizeHint().height()));setMinimumHeight(height+4);}
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
 auto* helpAction=menu->addAction(VisualStyle::icon("info"),"Capture and clipboard actions…");helpAction->setObjectName("captureActionsHelp");
 auto* updatesAction=menu->addAction(VisualStyle::icon("download"),"Check for updates");updatesAction->setObjectName("checkForUpdates");
 // Opens General → Updates and starts its Check now, where checks are
 // supported; the result shows there.
 connect(updatesAction,&QAction::triggered,this,[this]{navigate("settings");settings_->showUpdates();if(auto* now=settings_->findChild<QPushButton*>("checkForUpdatesNow");now&&now->isEnabled())now->click();});
 connect(menu->addAction("About put.gd"),&QAction::triggered,this,&MainWindow::aboutRequested);
 menu->addSeparator();
 auto* quitAction=menu->addAction("Quit put.gd");quitAction->setObjectName("quitApplication");connect(quitAction,&QAction::triggered,this,&MainWindow::quitRequested);
 auto* header=Graphite::WindowHeader::install(this,nullptr,menu);
 tabs_=new Graphite::Segmented(header);tabs_->setObjectName("mainNavigation");tabs_->setAccessibleName("Main navigation");for(const auto* label:{"History","Destinations","Settings"})tabs_->addTab(label);header->setCentre(tabs_);
 auto* center=new QWidget;setCentralWidget(center);auto* layout=new QVBoxLayout(center);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 // The command band: capture and upload actions, primary first.
 band_=new Graphite::Surface("bg",{},center);band_->setObjectName("commandBand");band_->setAccessibleName("Capture and upload actions");band_->installEventFilter(this);auto* bandRow=new QHBoxLayout(band_);bandRow->setContentsMargins(12,10,12,11);bandRow->setSpacing(6);layout->addWidget(band_);
 const QString actionHelp="Capture region: drag to select a screen area; Escape cancels.\n\nCapture window: capture the focused external app window, including its frame where supported.\n\nCapture desktop: capture every enabled monitor, including desktop panels.\n\nUpload clipboard: upload copied local files, an image, or plain text, in that order. URLs copied as text are uploaded as text.\n\nCapture region & edit: select a region, then redact, blur, crop or annotate it in the editor. Nothing uploads until you choose Upload. Its menu edits the clipboard image or an image file instead; you can also drop a PNG or JPEG on it.\n\nEach button starts that action after finished settings edits are saved. Screen captures close this window to keep it out of the capture; clipboard uploads keep it open. Store or clear unfinished credentials before a screen capture. Settings → Capture selects the upload destination, and its Keep a local copy independently enables local saving. Both can be enabled. With neither enabled, the action fails before capturing. Shared uploads copy a link; private uploads create no link. Hover a button for its current output behavior. Results and recovery actions appear in History.";
 const QHash<QString,QString> explanations{{"region","Drag to select an area of your screen. Press Escape to cancel."},{"window","Capture the focused external app window, including its frame where supported."},{"desktop","Capture all enabled monitors, including desktop panels."},{"clipboard","Upload copied local files, an image, or plain text, in that order. Text URLs are uploaded as text."},{"edit","Select a region, then redact, blur, crop or annotate it. Nothing uploads until you choose Upload.\n\nUse its menu to edit the clipboard image or an image file instead, or drop a PNG or JPEG on this button."},{"record","Drag to select an area, then use the floating bar to record, pause and stop. Stop opens a review window; nothing uploads until you choose Upload there."}};
 // Three groups, each action once: the screenshots (Capture region is the
 // primary), then editing and recording, and at the far end the clipboard
 // upload, which captures nothing. Hairlines separate the groups.
 const QList<QStringList> groups{{"region","window","desktop"},{"edit","record"},{"clipboard"}};QStringList bandOrder;for(const auto& group:groups)bandOrder<<group;
 for(const auto& action:Settings::invocableActionNames())if(!bandOrder.contains(action))bandOrder<<action;bandOrder.removeIf([](const QString& a){return !Settings::invocableActionNames().contains(a);});
 for(const auto& action:bandOrder){
  if(action=="edit"){auto* divider=Graphite::rule(band_,Qt::Vertical);divider->setObjectName("commandBandDivider");divider->setFixedHeight(Graphite::scaled(20));bandRow->addSpacing(6);bandRow->addWidget(divider,0,Qt::AlignVCenter);bandRow->addSpacing(6);}
  if(action=="clipboard")bandRow->addStretch();
  auto* button=Graphite::toolButton(labels.value(action),glyphs.value(action),action=="region"?"primary":"quiet",band_);button->setObjectName("captureActionButton");button->setProperty("captureAction",action);button->setAccessibleName(labels.value(action));
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
  auto update=[this,button,action,explanations]{
   const auto document=session_.document();const auto saved=Settings::effectiveAction(document,action);const auto key=QKeySequence(saved["shortcut"].toString()).toString(QKeySequence::NativeText);QString output;
   const bool local=document["archive"].toObject()["keepUploads"].toBool();
   if(saved["destination"]=="local"||saved["profileId"].toString().isEmpty())output=local?"Save locally using your output folders, then copy the result to the clipboard when eligible. Uploads are off.":"This action will fail: no destination and local copies are off. Choose a destination or turn on Keep a local copy in Settings → Capture.";
   else{
    QJsonObject profile;for(const auto& value:document["profiles"].toArray())if(value.toObject()["id"]==saved["profileId"])profile=value.toObject();
    if(profile.isEmpty())output="Choose an upload destination in Settings → Capture before using this action.";
    else{const auto mode=saved["publicationMode"].toString("default");const bool privateUpload=mode=="private"||(mode=="default"&&profile["provider"]=="proton-drive"&&!profile["settings"].toObject()["shareByDefault"].toBool(true));if(privateUpload&&profile["provider"]!="proton-drive")output="This destination does not support private uploads. Choose sharing in Settings → Capture before using this action.";else output=privateUpload?"Upload privately to "+profile["label"].toString()+". No share link is created and the clipboard is left unchanged.":"Upload to "+profile["label"].toString()+" and copy its share link to the clipboard when eligible.";}
   }
   if(local&&saved["destination"]=="upload"&&!saved["profileId"].toString().isEmpty())output+=" Also keep a local copy in your folders.";
   if(saved["destination"]=="local"||saved["profileId"].toString().isEmpty()){if(local)output="Local only. "+output;}else{QJsonObject profile;for(const auto& v:document["profiles"].toArray())if(v.toObject()["id"]==saved["profileId"])profile=v.toObject();const auto mode=saved["publicationMode"].toString("default");const bool privateUpload=mode=="private"||(mode=="default"&&profile["provider"]=="proton-drive"&&!profile["settings"].toObject()["shareByDefault"].toBool(true));output=(privateUpload?QString("Private upload. "):QString("Shared link. "))+output;}
   const auto description=explanations.value(action)+"\n\n"+output+"\n\nUses saved settings. Review results in History."+(key.isEmpty()?"\nNo global shortcut assigned.":"\nShortcut: "+key);
   const auto unavailable=unavailableOutput(document,action);button->setEnabled(unavailable.isEmpty());if(action=="region")VisualStyle::role(button,unavailable.isEmpty()?"primary":"quiet");button->setAccessibleDescription(unavailable.isEmpty()?description:"Unavailable: "+unavailable+"\n\n"+description);auto tooltip=button->accessibleDescription().toHtmlEscaped();tooltip.replace('\n',"<br>");button->setToolTip("<qt>"+tooltip+"</qt>");
  };
  connect(&session_,&UiSession::accepted,button,update);update();connect(button,&QToolButton::clicked,this,[this,action]{emit actionRequested(action);});bandRow->addWidget(button);actionButtons_<<button;
 }
 connect(helpAction,&QAction::triggered,this,[this,actionHelp]{VisualStyle::showHelp("Capture and clipboard actions",actionHelp,this);});
 outputWarning_=new Graphite::Surface("surface",Qt::BottomEdge,center);outputWarning_->setObjectName("commandOutputWarning");auto* warningRow=new QHBoxLayout(outputWarning_);warningRow->setContentsMargins(16,6,12,8);warningRow->setSpacing(12);outputWarningText_=new QLabel(outputWarning_);outputWarningText_->setObjectName("commandOutputWarningText");outputWarningText_->setTextFormat(Qt::PlainText);outputWarningText_->setWordWrap(true);VisualStyle::role(outputWarningText_,"warning");warningRow->addWidget(outputWarningText_,1);auto* chooseOutput=new QPushButton("Choose output…",outputWarning_);chooseOutput->setObjectName("chooseCommandOutput");chooseOutput->setToolTip("Open Settings → Capture to choose the output");warningRow->addWidget(chooseOutput);connect(chooseOutput,&QPushButton::clicked,this,[this]{navigate("settings");QStringList affected;for(const auto& action:Settings::invocableActionNames())if(!unavailableOutput(session_.document(),action).isEmpty())affected<<action;settings_->showCaptureSettings(affected);});layout->addWidget(outputWarning_);connect(&session_,&UiSession::accepted,this,&MainWindow::updateAvailability);updateAvailability();
 // Edit an image file… whose chooser could not open: the reason under the band
 // that holds the action, with Try again. A cancel shows nothing.
 chooserFailure_=new Graphite::Surface("surface",Qt::BottomEdge,center);chooserFailure_->setObjectName("editImageChooserFailure");{auto* row=new QHBoxLayout(chooserFailure_);row->setContentsMargins(16,6,12,6);row->setSpacing(12);
  chooserError_=new QLabel(chooserFailure_);chooserError_->setObjectName("editImageChooserError");chooserError_->setTextFormat(Qt::PlainText);chooserError_->setWordWrap(true);row->addWidget(chooserError_,1);
  auto* retry=new QPushButton("Try again",chooserFailure_);retry->setObjectName("retryEditImageChooser");row->addWidget(retry);connect(retry,&QPushButton::clicked,this,[this]{chooseImageToEdit();});
  auto* dismiss=Graphite::toolButton({},{},"quiet",chooserFailure_);dismiss->setObjectName("dismissEditImageChooserFailure");dismiss->setIcon(Graphite::chromeIcon("close"));dismiss->setAccessibleName("Dismiss");dismiss->setToolTip("Dismiss");row->addWidget(dismiss);connect(dismiss,&QToolButton::clicked,chooserFailure_,&QWidget::hide);}
 layout->addWidget(chooserFailure_);chooserFailure_->hide();
 pages_=new QStackedWidget;pages_->setObjectName("mainPages");layout->addWidget(pages_,1);history_=new HistoryWindow(session_,pages_);pages_->addWidget(history_);
 // Keep the navigation indexes stable without constructing hidden Settings and
 // destination controls for a History-only visit (#558).
 pages_->addWidget(new QWidget(pages_));pages_->addWidget(new QWidget(pages_));
 connect(tabs_,&QTabBar::currentChanged,this,[this](int row){if(settings_)settings_->clearCompletedFeedback();if(row>0)settings();pages_->setCurrentIndex(row);if(row==1)settings_->activateDestinations();emit viewStateChanged();});
 connect(history_,&HistoryWindow::viewStateChanged,this,&MainWindow::viewStateChanged);connect(history_,&HistoryWindow::editRequested,this,&MainWindow::editRequested);connect(history_,&HistoryWindow::chooseImageRequested,this,&MainWindow::chooseImageToEdit);
 setStatusBar(new StatusBand(this));statusBar()->installEventFilter(this);
 // Future capture output, on every page: one line at the left of the status
 // band (where the eye checks state), with the way to change it beside it.
 auto* outputGroup=outputGroup_=new QWidget;outputGroup->setObjectName("captureOutputGroup");auto* outputRow=new QHBoxLayout(outputGroup);outputRow->setContentsMargins(0,0,0,0);outputRow->setSpacing(10);
 // The summary takes the room the band has left and elides only beyond it; it
 // is a tab stop whose Enter opens the full text.
 auto* summary=new Graphite::ElidedLabel(outputGroup);destination_=summary;destination_->setObjectName("captureOutputSummary");destination_->setTextFormat(Qt::PlainText);VisualStyle::role(destination_,"secondary");summary->setRevealable(true);summary->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);outputRow->addWidget(destination_,1);
 auto* changeOutput=new QPushButton("Change output…",outputGroup);changeOutput->setObjectName("changeCaptureOutput");VisualStyle::role(changeOutput,"link");changeOutput->setCursor(Qt::PointingHandCursor);outputRow->addWidget(changeOutput);outputRow->addStretch(0);
 statusBar()->addWidget(outputGroup,1);
 // The running build, from the resident identity About also shows. Stable and
 // nightly builds link to their GitHub release; development builds are text.
 version_=new QLabel;version_->setObjectName("appVersion");version_->setTextFormat(Qt::PlainText);VisualStyle::role(version_,"meta");
 versionLink_=new QPushButton;versionLink_->setObjectName("appVersionLink");VisualStyle::role(versionLink_,"link");versionLink_->setFont(Graphite::font(Graphite::Type::metadata));versionLink_->setCursor(Qt::PointingHandCursor);
 auto* versionGroup=new QWidget;auto* versionLayout=new QVBoxLayout(versionGroup);versionLayout->setContentsMargins(0,0,0,0);versionLayout->setSpacing(0);versionLayout->addWidget(versionLink_,0,Qt::AlignRight);auto* versionBrowser=new Graphite::BrowserFeedback(versionGroup);versionBrowser->setObjectName("appVersionBrowserFeedback");versionLayout->addWidget(versionBrowser);
 connect(versionLink_,&QPushButton::clicked,this,[this,versionBrowser]{if(const auto url=releaseLink(session_.build()).url;url.isValid())host::openLink(url,versionBrowser->opening(url));});
 auto updateVersion=[this,versionGroup,versionBrowser]{
  const auto link=releaseLink(session_.build());const auto name="put.gd version "+link.version;const auto page="Open the v"+link.version+" release on GitHub";
  version_->setText(link.version);version_->setAccessibleName(name);version_->setToolTip(name);version_->setVisible(!link.version.isEmpty()&&link.url.isEmpty());
  versionLink_->setText(link.version);versionLink_->setAccessibleName(name);versionLink_->setAccessibleDescription(page);versionLink_->setToolTip(page);versionLink_->setVisible(!link.url.isEmpty());versionGroup->setVisible(!link.url.isEmpty());if(versionLink_->property("browserUrl").toUrl()!=link.url){versionBrowser->clear();versionLink_->setProperty("browserUrl",link.url);}
 };
 connect(&session_,&UiSession::statusChanged,this,updateVersion);updateVersion();
 // A newer version, from the resident's daily check (#293): it opens
 // General → Updates, which has the download.
 updateLink_=new QPushButton(VisualStyle::icon("download"),"Update available");updateLink_->setObjectName("updateAvailable");VisualStyle::role(updateLink_,"link");updateLink_->setCursor(Qt::PointingHandCursor);updateLink_->hide();
 connect(updateLink_,&QPushButton::clicked,this,[this]{navigate("settings");settings_->showUpdates();});
 auto updateAvailable=[this]{
  const auto update=session_.diagnostics()["update"].toObject();const auto result=update["result"].toObject();
  const bool available=update["supported"].toBool()&&result["updateAvailable"].toBool();
  updateLink_->setToolTip("put.gd "+result["latest"].toString()+" is available. Open Settings → General → Updates.");
  updateLink_->setAccessibleName("Update available: put.gd "+result["latest"].toString());updateLink_->setVisible(available);
 };
 connect(&session_,&UiSession::statusChanged,this,updateAvailable);updateAvailable();
 auto updateDestination=[this]{
  const auto document=session_.document();QString label;
  auto outputName=[&](const QJsonObject& output){const bool local=document["archive"].toObject()["keepUploads"].toBool();if(output["destination"]=="local"||output["profileId"].toString().isEmpty())return local?QString("Local only"):QString("No output enabled");for(const auto& value:document["profiles"].toArray()){const auto profile=value.toObject();if(profile["id"]!=output["profileId"])continue;const auto mode=output["publicationMode"].toString("default");const bool privateUpload=mode=="private"||(mode=="default"&&profile["provider"]=="proton-drive"&&!profile["settings"].toObject()["shareByDefault"].toBool(true));return (privateUpload?(profile["provider"]=="proton-drive"?QString("Private upload"):QString("Private upload unavailable")):QString("Shared link"))+" · "+profile["label"].toString()+(local?QString(" + local copy"):QString{});}return QString("Choose a destination");};
  const auto defaults=document.contains("captureDefaults")?document["captureDefaults"].toObject():Settings::effectiveAction(document,"region");
  const auto defaultName=outputName(defaults);QStringList details{"Default output: "+defaultName};bool mixed=false;
  const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"edit","Capture region & edit"},{"clipboard","Upload clipboard"},{"record","Record screen"}};
  for(const auto& action:Settings::invocableActionNames()){
   const auto route=Settings::effectiveAction(document,action);const auto output=outputName(route);
   if(output!=defaultName||route["profileId"].toString()!=defaults["profileId"].toString())mixed=true;
   details<<labels.value(action,action)+": "+output+(document["archive"].toObject()["keepUploads"].toBool()?QString{}:QString(" · No local copy"));
  }
  label=mixed?QString("Outputs vary by action…"):"Default output: "+defaultName;
  destination_->setText(label);static_cast<Graphite::ElidedLabel*>(destination_)->setRevealText(details.join("\n\n"));destination_->setAccessibleName(label);destination_->setAccessibleDescription("Press Enter to show each action's destination, sharing and local copy behavior.");destination_->setToolTip(details.join("\n"));updateSaveStatus();
 };
 connect(changeOutput,&QPushButton::clicked,this,[this]{navigate("settings");settings_->showCaptureSettings();});
 connect(history_,&HistoryWindow::captureSettingsRequested,changeOutput,&QPushButton::click);
 connect(history_,&HistoryWindow::localCopySettingsRequested,this,&MainWindow::showLocalCopySettings);
 explanation_=new QLabel("Settings save automatically");explanation_->setObjectName("autosaveExplanation");VisualStyle::role(explanation_,"meta");statusBar()->addPermanentWidget(explanation_);
 // Autosave describes the Destinations and Settings pages, not History.
 connect(tabs_,&QTabBar::currentChanged,this,[this]{updateSaveStatus();});
 saveStatus_=new QLabel;saveStatus_->setObjectName("autosaveStatus");saveStatus_->setAccessibleName("Settings save status");saveStatus_->setTextFormat(Qt::PlainText);VisualStyle::role(saveStatus_,"meta");statusBar()->addPermanentWidget(saveStatus_);
 statusBar()->addPermanentWidget(updateLink_);statusBar()->addPermanentWidget(version_);statusBar()->addPermanentWidget(versionGroup);
 connect(&session_,&UiSession::accepted,this,[this,updateDestination]{updateDestination();updateSaveStatus();});
 updateDestination();updateSaveStatus();tabs_->setCurrentIndex(0);RedesignTheme::apply(*this);
 connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,[this]{QTimer::singleShot(0,this,[this]{fitBand();});});
}
SettingsWindow* MainWindow::settings(){
 if(settings_)return settings_;
 settings_=new SettingsWindow(session_,pages_);
 auto* destinations=pages_->widget(1);auto* preferences=pages_->widget(2);
 pages_->removeWidget(destinations);pages_->removeWidget(preferences);delete destinations;delete preferences;
 pages_->addWidget(settings_->destinationsPage());pages_->addWidget(settings_);
 // New combo popup containers need the same themed delegate/margin refresh
 // that the original eager tree received when the main style was applied.
 RedesignTheme::apply(*this);
 connect(settings_,&SettingsWindow::viewStateChanged,this,[this]{updateSaveStatus();emit viewStateChanged();});
 connect(settings_,&SettingsWindow::actionRequested,this,&MainWindow::actionRequested);
 connect(settings_,&SettingsWindow::quitRequested,this,&MainWindow::quitRequested);
 connect(settings_,&SettingsWindow::destinationsRequested,this,[this]{navigate("destinations");});
 emit settingsCreated(settings_);
 updateSaveStatus();return settings_;
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
 bool room=true;
 if(tabs_->currentIndex()>0&&!unsaved){
  const int gap=statusBar()->style()->pixelMetric(QStyle::PM_LayoutHorizontalSpacing,nullptr,statusBar());int needed=outputGroup_->sizeHint().width()+explanation_->sizeHint().width()+qMax(gap,6)+24;
  for(auto* widget:statusBar()->findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(widget!=outputGroup_&&widget!=explanation_&&!widget->isWindow()&&widget->isVisibleTo(statusBar()))needed+=widget->sizeHint().width()+qMax(gap,6);
  room=needed<=statusBar()->width();
 }
 explanation_->setVisible(tabs_->currentIndex()>0&&!unsaved&&room);
}
// A band narrower than its buttons first shows Capture window and Capture
// desktop as icons, then every quiet action; names stay in tooltips and
// accessible names. Every action keeps its own button at every width.
void MainWindow::fitBand(){
 if(!band_||fittingBand_)return;fittingBand_=true;
 auto apply=[this](int stage){for(auto* button:actionButtons_){const auto action=button->property("captureAction").toString();const bool compact=action!="region"&&(stage>=2||(stage==1&&(action=="window"||action=="desktop")));button->setToolButtonStyle(compact?Qt::ToolButtonIconOnly:Qt::ToolButtonTextBesideIcon);}band_->layout()->invalidate();band_->layout()->activate();};
 int stage=0;for(;stage<3;++stage){apply(stage);if(band_->layout()->minimumSize().width()<=band_->width()||stage==2)break;}
 bandStage_=stage;fittingBand_=false;
}
void MainWindow::updateAvailability(){
 QStringList unavailable,reasons;for(auto* button:actionButtons_){const auto reason=unavailableOutput(session_.document(),button->property("captureAction").toString());if(reason.isEmpty())continue;unavailable<<button->accessibleName();if(!reasons.contains(reason))reasons<<reason;}
 const auto message=unavailable.isEmpty()?QString{}:unavailable.size()==actionButtons_.size()?(reasons.size()==1&&reasons.first().startsWith("No output")?QString("No output is enabled. Choose a destination or enable local copies."):reasons.join(" ")):"Choose an output for: "+unavailable.join(", ")+".";outputWarningText_->setText(message);outputWarningText_->setAccessibleDescription(message+" Opens Settings → Capture.");outputWarning_->setVisible(!unavailable.isEmpty());
}
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
 if(watched==band_&&event->type()==QEvent::Resize)fitBand();
 if(watched==statusBar()&&event->type()==QEvent::Resize)updateSaveStatus();
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
