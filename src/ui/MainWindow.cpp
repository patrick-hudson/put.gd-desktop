#include "MainWindow.h"
#include <QDir>
#include <QStandardPaths>
#include <QFileDialog>
#include <QMenu>
#include "SettingsWindow.h"
#include "HistoryWindow.h"
#include "UiSession.h"
#include "VisualStyle.h"
#include "RedesignTheme.h"
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QListWidget>
#include <QStackedWidget>
#include <QJsonArray>
#include <QKeySequence>
#include <QToolButton>
#include <QStatusBar>
namespace putgd {
MainWindow::MainWindow(UiSession& session,QWidget* parent):QMainWindow(parent),session_(session){
 setObjectName("putgdMainWindow");setWindowTitle("put.gd");setWindowIcon(VisualStyle::icon("brand"));resize(1320,880);setMinimumSize(1060,680);
 auto* center=new QWidget;setCentralWidget(center);auto* layout=new QHBoxLayout(center);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 auto* rail=new QWidget;rail->setObjectName("mainRail");rail->setFixedWidth(210);layout->addWidget(rail);auto* railBox=new QVBoxLayout(rail);railBox->setContentsMargins(14,20,14,14);railBox->setSpacing(6);
 auto* brand=new QLabel("put.gd");VisualStyle::role(brand,"title");railBox->addWidget(brand);railBox->addSpacing(14);
 auto* captureHeading=new QHBoxLayout;captureHeading->setContentsMargins(0,0,0,0);auto* captureLabel=new QLabel("CAPTURE & IMPORT");captureLabel->setObjectName("captureActionsHeading");VisualStyle::role(captureLabel,"railHeading");captureHeading->addWidget(captureLabel);captureHeading->addStretch();
 const QString actionHelp="Capture region: drag to select a screen area; Escape cancels.\n\nCapture window: capture the focused external app window, including its frame where supported.\n\nCapture desktop: capture every enabled monitor, including desktop panels.\n\nImport clipboard: import copied local files, an image, or plain text, in that order. URLs copied as text are imported as text.\n\nCapture region & edit: select a region, then redact, blur, crop or annotate it in the editor. Nothing uploads until you choose Upload.\n\nEach button starts that action after finished settings edits are saved. Screen captures close this window to keep it out of the capture; clipboard imports keep it open. Store or clear unfinished credentials before a screen capture. Settings → Capture selects the upload destination. Keep a local copy in Settings → General independently enables local saving. Both can be enabled. With neither enabled, the action fails before capturing. Shared uploads copy a link; private uploads create no link. Hover a button for its current output behavior. Results and recovery actions appear in History.";
 auto* captureHelp=VisualStyle::helpButton("Capture and clipboard actions",actionHelp,captureLabel,rail);captureHeading->addWidget(captureHelp);railBox->addLayout(captureHeading);railBox->addSpacing(4);
 const QHash<QString,QString> labels{{"region","Capture region"},{"window","Capture window"},{"desktop","Capture desktop"},{"clipboard","Import clipboard"},{"edit","Capture region & edit"}};
 const QHash<QString,QString> explanations{{"region","Drag to select an area of your screen. Press Escape to cancel."},{"window","Capture the focused external app window, including its frame where supported."},{"desktop","Capture all enabled monitors, including desktop panels."},{"clipboard","Import copied local files, an image, or plain text, in that order. Text URLs are imported as text."},{"edit","Select a region, then redact, blur, crop or annotate it. Nothing uploads until you choose Upload."}};
 for(const auto& action:Settings::actionNames()){
  auto* button=new QPushButton;button->setObjectName("captureRailButton");button->setProperty("captureAction",action);button->setAccessibleName(labels.value(action));button->setMinimumHeight(52);
  auto* box=new QVBoxLayout(button);box->setContentsMargins(10,6,10,6);box->setSpacing(2);auto* line=new QHBoxLayout;line->setContentsMargins(0,0,0,0);line->setSpacing(8);auto* icon=new QLabel;icon->setPixmap(VisualStyle::icon(action).pixmap(19,19));line->addWidget(icon);auto* name=new QLabel(labels.value(action));line->addWidget(name);line->addStretch();
  box->addLayout(line);auto* shortcut=new QLabel;shortcut->setProperty("visualRole","mono");
  if(action=="edit"){
   // Other editor sources sit behind one small arrow on the shortcut line, so
   // the rail does not grow and the title keeps its full width.
   auto* lower=new QHBoxLayout;lower->setContentsMargins(0,0,0,0);lower->setSpacing(4);lower->addWidget(shortcut);lower->addStretch();
   auto* more=new QToolButton(button);more->setObjectName("editSourceMenu");more->setAutoRaise(true);more->setArrowType(Qt::DownArrow);more->setPopupMode(QToolButton::InstantPopup);
   more->setFixedSize(22,16);more->setStyleSheet("QToolButton { border: none; background: transparent; padding: 0; } QToolButton:hover { background: #2b303b; border-radius: 4px; } QToolButton::menu-indicator { image: none; width: 0; }");
   more->setToolTip("Edit the clipboard image or an image file");more->setAccessibleName("More ways to edit");
   auto* menu=new QMenu(more);
   connect(menu->addAction(VisualStyle::icon("clipboard"),"Edit clipboard image"),&QAction::triggered,this,[this]{emit editRequested("clipboard",{});});
   connect(menu->addAction(VisualStyle::icon("images"),"Edit an image file…"),&QAction::triggered,this,&MainWindow::chooseImageToEdit);
   more->setMenu(menu);lower->addWidget(more);box->addLayout(lower);
  }else box->addWidget(shortcut);for(auto* label:{name,icon,shortcut})label->setAttribute(Qt::WA_TransparentForMouseEvents);
  auto update=[this,shortcut,button,action,explanations]{
   const auto document=session_.document();const auto saved=Settings::effectiveAction(document,action);const auto key=QKeySequence(saved["shortcut"].toString()).toString(QKeySequence::NativeText);shortcut->setText(key);shortcut->setVisible(!key.isEmpty());QString output;
   const bool local=document["archive"].toObject()["keepUploads"].toBool();
   if(saved["destination"]=="local"||saved["profileId"].toString().isEmpty())output=local?"Save locally using your output folders, then copy the result to the clipboard when eligible. Uploads are off.":"This action will fail: no destination and local copies are off. Choose a destination in Settings → Capture or enable Keep a local copy in Settings → General.";
   else{
    QJsonObject profile;for(const auto& value:document["profiles"].toArray())if(value.toObject()["id"]==saved["profileId"])profile=value.toObject();
    if(profile.isEmpty())output="Choose an upload destination in Settings → Capture before using this action.";
    else{const auto mode=saved["publicationMode"].toString("default");const bool privateUpload=mode=="private"||(mode=="default"&&profile["provider"]=="proton-drive"&&!profile["settings"].toObject()["shareByDefault"].toBool(true));if(privateUpload&&profile["provider"]!="proton-drive")output="This destination does not support private uploads. Choose sharing in Settings → Capture before using this action.";else output=privateUpload?"Upload privately to "+profile["label"].toString()+". No share link is created and the clipboard is left unchanged.":"Upload to "+profile["label"].toString()+" and copy its share link to the clipboard when eligible.";}
   }
   if(local&&saved["destination"]=="upload"&&!saved["profileId"].toString().isEmpty())output+=" Also save a local copy using your folders in General.";
   const auto description=explanations.value(action)+"\n\n"+output+"\n\nUses saved settings. Review results in History."+(key.isEmpty()?"\nNo global shortcut assigned.":"\nShortcut: "+key);
   button->setAccessibleDescription(description);auto tooltip=description.toHtmlEscaped();tooltip.replace('\n',"<br>");button->setToolTip("<qt>"+tooltip+"</qt>");
  };
  connect(&session_,&UiSession::accepted,button,update);update();connect(button,&QPushButton::clicked,this,[this,action]{emit actionRequested(action);});railBox->addWidget(button);
 }
 railBox->addSpacing(16);navigation_=new QListWidget;navigation_->setObjectName("mainNavigation");navigation_->setAccessibleName("Main navigation");navigation_->setIconSize(QSize(20,20));
 for(const auto& entry:QList<QPair<QString,QString>>{{"history","History"},{"destinations","Destinations"},{"preferences","Settings"}})new QListWidgetItem(VisualStyle::icon(entry.first),entry.second,navigation_);
 navigation_->setFixedHeight(168);railBox->addWidget(navigation_);railBox->addStretch();destination_=new QLabel;destination_->setObjectName("captureOutputSummary");destination_->setTextFormat(Qt::PlainText);destination_->setWordWrap(true);VisualStyle::role(destination_,"muted");railBox->addWidget(destination_);
 auto* changeOutput=new QPushButton("Change capture output…");changeOutput->setObjectName("changeCaptureOutput");VisualStyle::role(changeOutput,"link");railBox->addWidget(changeOutput);
 pages_=new QStackedWidget;pages_->setObjectName("mainPages");layout->addWidget(pages_,1);settings_=new SettingsWindow(session_,pages_);history_=new HistoryWindow(session_,pages_);pages_->addWidget(history_);pages_->addWidget(settings_->destinationsPage());pages_->addWidget(settings_);
 connect(navigation_,&QListWidget::currentRowChanged,pages_,&QStackedWidget::setCurrentIndex);connect(navigation_,&QListWidget::currentRowChanged,this,[this](int row){if(row==1)settings_->activateDestinations();emit viewStateChanged();});connect(settings_,&SettingsWindow::viewStateChanged,this,&MainWindow::viewStateChanged);connect(history_,&HistoryWindow::viewStateChanged,this,&MainWindow::viewStateChanged);connect(settings_,&SettingsWindow::actionRequested,this,&MainWindow::actionRequested);connect(settings_,&SettingsWindow::quitRequested,this,&MainWindow::quitRequested);
 auto updateDestination=[this]{
  const auto document=session_.document();QString label;int exceptions=0;
  auto outputName=[&](const QJsonObject& output){const bool local=document["archive"].toObject()["keepUploads"].toBool();if(output["destination"]=="local"||output["profileId"].toString().isEmpty())return local?QString("Local files only"):QString("No output enabled");for(const auto& value:document["profiles"].toArray())if(value.toObject()["id"]==output["profileId"])return value.toObject()["label"].toString()+(local?QString(" + local copy"):QString{});return QString("Choose a destination");};
  if(document.contains("captureDefaults")){
   label="Default output\n"+outputName(document["captureDefaults"].toObject());
   for(const auto& name:Settings::actionNames())if(document["actions"].toObject()[name].toObject()["destination"]!="default")++exceptions;
   if(exceptions)label+="\n"+QString::number(exceptions)+(exceptions==1?" action exception":" action exceptions");
  }else{
   const auto first=Settings::effectiveAction(document,"region");bool same=true;for(const auto& name:Settings::actionNames()){const auto action=Settings::effectiveAction(document,name);if(action["destination"]!=first["destination"]||action["profileId"]!=first["profileId"]||action["publicationMode"].toString("default")!=first["publicationMode"].toString("default"))same=false;}
   label=same?"Capture output\n"+outputName(first):QString("Capture output\nDifferent for each action");
  }
  destination_->setText(label);destination_->setToolTip("Output for future captures, using your saved settings. This is not an active upload.");
 };
 connect(changeOutput,&QPushButton::clicked,this,[this]{navigate("settings");settings_->showCaptureSettings();});
 auto* explanation=new QLabel("Settings save automatically");explanation->setObjectName("autosaveExplanation");statusBar()->setSizeGripEnabled(false);statusBar()->addWidget(explanation);
 saveStatus_=new QLabel;saveStatus_->setObjectName("autosaveStatus");saveStatus_->setAccessibleName("Settings save status");saveStatus_->setTextFormat(Qt::PlainText);statusBar()->addPermanentWidget(saveStatus_);
 auto updateSaveStatus=[this]{const auto text=settings_->autosaveText();saveStatus_->setVisible(!text.isEmpty());VisualStyle::feedback(saveStatus_,text,!text.isEmpty()&&text!="Use Store to save credentials");};
 connect(settings_,&SettingsWindow::viewStateChanged,this,updateSaveStatus);connect(&session_,&UiSession::accepted,this,[updateDestination,updateSaveStatus]{updateDestination();updateSaveStatus();});
 updateDestination();updateSaveStatus();navigation_->setCurrentRow(0);RedesignTheme::apply(*this);
}
MainWindow::~MainWindow(){
 disconnect(settings_,nullptr,this,nullptr);disconnect(history_,nullptr,this,nullptr);disconnect(navigation_,nullptr,this,nullptr);disconnect(&session_,nullptr,this,nullptr);
}
void MainWindow::chooseImageToEdit(){
 auto folder=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);if(folder.isEmpty())folder=QDir::homePath();
 const auto path=QFileDialog::getOpenFileName(this,"Edit an image file",folder,"Images (*.png *.jpg *.jpeg)");
 if(!path.isEmpty())emit editRequested("file",path);
}
void MainWindow::navigate(const QString& name){navigation_->setCurrentRow(name=="settings"?2:name=="destinations"?1:0);if(name=="activity"||name=="history")history_->refresh();}
QJsonObject MainWindow::saveViewState()const{return {{"page",navigation_->currentRow()},{"geometry",QJsonArray{x(),y(),width(),height()}}};}
bool MainWindow::restoreViewState(const QJsonObject& state){auto geometry=state["geometry"].toArray();if(geometry.size()!=4)return false;setGeometry(geometry[0].toInt(),geometry[1].toInt(),geometry[2].toInt(),geometry[3].toInt());navigation_->setCurrentRow(qBound(0,state["page"].toInt(),2));return true;}
}
