#include "MainWindow.h"
#include <QDir>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QClipboard>
#include <QApplication>
#include <QStandardPaths>
#include "FileDialog.h"
#include <QMenu>
#include <QActionGroup>
#include <QWidgetAction>
#include <QTextDocument>
#include <QTextBlock>
#include <QTextLayout>
#include <QAbstractTextDocumentLayout>
#include <QDropEvent>
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QMimeData>
#include <QStyle>
#include <QUrl>
#include "SettingsWindow.h"
#include "HistoryWindow.h"
#include "UiSession.h"
#include "Ui.h"
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
#include <QtMath>
#include "platform/HostOpen.h"
#include "core/ReleaseLink.h"
#include "core/CommandText.h"
namespace putgd {
using Ui::Space;
using Ui::space;
namespace {
void orderNavigation(Ui::WindowHeader* header,QWidget* tabs_,const QList<QToolButton*>& actionButtons_,QWidget* band_){
 QWidget* last=header->menuButton();QWidget::setTabOrder(last,tabs_);last=tabs_;for(auto* button:actionButtons_){QWidget::setTabOrder(last,button);last=button;}
 // Preserve each page's internal order, after the persistent commands; chrome is last.
 QList<QWidget*> content;auto* next=last->nextInFocusChain();while(next!=last){if(next!=header->menuButton()&&next!=tabs_&&!header->isAncestorOf(next)&&!band_->isAncestorOf(next)&&next->focusPolicy()!=Qt::NoFocus)content.append(next);next=next->nextInFocusChain();}
 for(auto* control:content){QWidget::setTabOrder(last,control);last=control;}header->chromeAfter(last);
}
QString outputSpan(const QString& text,const QFont& font,const QColor& color){return "<span style=\"font-family:'"+font.family()+"';font-size:"+QString::number(font.pixelSize())+"px;font-weight:"+QString::number(font.weight())+";color:"+color.name()+"\">"+text+"</span>";}
// One document owns both the popup's measurements and its painted text.
class OutputLabel final:public QLabel {
public:
 explicit OutputLabel(QWidget* parent):QLabel(parent){document_.setDocumentMargin(0);document_.setDefaultFont(Ui::font());setFocusPolicy(Qt::NoFocus);}
 int measure(const QString& html){document_.setHtml(html);document_.setTextWidth(-1);return qCeil(document_.idealWidth());}
 QString wrap(const QStringList& segments,const QFont& font,const QColor& color,int width){QStringList lines;QString line;for(const auto& segment:segments){const auto joined=line.isEmpty()?segment:line+" · "+segment;if(!line.isEmpty()&&measure(outputSpan(joined,font,color))>width){lines.append(line);line.clear();}if(!line.isEmpty())line+=" · ";line+=segment;}if(!line.isEmpty())lines.append(line);return outputSpan(lines.join("<br>"),font,color);}
 void fit(const QString& html,int width){setText(html);document_.setHtml(html);document_.setTextWidth(width);setFixedSize(width,qCeil(document_.size().height()));int lines=0;for(auto block=document_.begin();block.isValid();block=block.next())lines+=block.layout()->lineCount();setProperty("renderedLines",lines);}
 int heightForWidth(int)const override{return height();}
protected:
 void paintEvent(QPaintEvent*)override{QPainter painter(this);document_.drawContents(&painter);}
private:QTextDocument document_;
};
// One measured grid keeps the default and its exceptions together.
void populateOutputRows(QMenu* menu){
 auto* action=menu->findChild<QWidgetAction*>("captureOutputRows");auto* body=action->defaultWidget();auto* grid=qobject_cast<QGridLayout*>(body->layout());while(auto* item=grid->takeAt(0)){delete item->widget();delete item;}
 const auto rows=action->property("outputRows").toStringList();const int exceptions=action->property("outputExceptions").toInt();
 const auto bodyFont=Ui::font(),metaFont=Ui::font(Ui::Type::technical);QList<OutputLabel*> labels;int natural=0;
 for(const auto& text:rows){auto* label=new OutputLabel(body);labels.append(label);natural=qMax(natural,label->measure(outputSpan(text.section(": ",0,0).toHtmlEscaped(),bodyFont,Theme::current().text)+"<br>"+outputSpan(text.section(": ",1).toHtmlEscaped(),metaFont,Theme::current().textSecondary)));}
 const int inset=space(Space::m),width=qBound(Ui::scaled(280),natural+2*inset,Ui::scaled(480)),room=width-2*inset;int row=0;
 for(int index=0;index<rows.size();++index){const auto& text=rows[index];if(exceptions&&index==rows.size()-exceptions){auto* caption=Ui::label("Exceptions","meta",body);caption->setObjectName("captureOutputExceptions");caption->setContentsMargins(0,space(Space::s),0,0);grid->addWidget(caption,row++,0);}
  auto* label=labels[index];label->setObjectName(QStringLiteral("captureOutputRow%1Text").arg(index));QStringList segments;for(const auto& segment:text.section(": ",1).split(" · "))segments.append(segment.toHtmlEscaped());const auto names=text.section(": ",0,0);const auto html=label->wrap({names.toHtmlEscaped()},bodyFont,Theme::current().text,room)+"<br>"+label->wrap(segments,metaFont,Theme::current().textSecondary,room);label->fit(html,room);label->setAccessibleName(text);grid->addWidget(label,row++,0);
 }body->setFixedWidth(width);grid->activate();
}
class OutputButton final:public QToolButton {
public:
 explicit OutputButton(QWidget* parent):QToolButton(parent){setMinimumHeight(qMax(32,fontMetrics().height()+4));}
 // Status controls retain a 32 px target without form padding.
 QSize sizeHint()const override{auto size=QToolButton::sizeHint();size.setHeight(qMax(32,fontMetrics().height()+4));return size;}
 QSize minimumSizeHint()const override{return {Ui::scaled(120),sizeHint().height()};}
protected:
 void changeEvent(QEvent* event)override{QToolButton::changeEvent(event);if(event->type()==QEvent::FontChange)setMinimumHeight(qMax(32,fontMetrics().height()+4));}
 void keyPressEvent(QKeyEvent* event)override{if(menu()&&(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter)){menu()->popup(mapToGlobal(QPoint(0,-menu()->sizeHint().height())));event->accept();return;}QToolButton::keyPressEvent(event);}
};
class StatusLink final:public QPushButton {
public:
 using QPushButton::QPushButton;
 // The style's version links paint 4 px insets and an 8 px icon/text gap.
 QSize sizeHint()const override{auto size=QPushButton::sizeHint();size.setWidth(qMax(size.width(),qCeil(QFontMetricsF(font()).horizontalAdvance(text()))+(icon().isNull()?0:iconSize().width()+8)+8));return size;}
 QSize minimumSizeHint()const override{return sizeHint();}
};
// An action-bar button. A split button (Capture region, Capture region & edit)
// opens its ▾ menu from the keyboard (Alt+Down, F4, the Menu key or Shift+F10)
// with its first row focused, and on a right-click; no other button has a
// context menu.
class CommandButton final:public QToolButton {
public:using QToolButton::QToolButton;
 // Compact toolbar targets retain the supported History row count.
 QSize sizeHint()const override{auto size=QToolButton::sizeHint();if(window()->height()<Ui::scaled(720))size.setHeight(qMax(Ui::Metric::iconTarget,qMax(iconSize().height(),fontMetrics().height()+space(Space::s))));return size;}
 QSize minimumSizeHint()const override{return sizeHint();}
 void openMenu(const QPoint& at,bool keyboard){if(!menu())return;menu()->popup(at);if(keyboard&&!menu()->actions().isEmpty())for(auto* row:menu()->actions())if(row->isVisible()&&row->isEnabled()&&!row->isSeparator()){menu()->setActiveAction(row);break;}}
protected:
 void keyPressEvent(QKeyEvent* event)override{
  const bool menuKey=event->key()==Qt::Key_Menu||event->key()==Qt::Key_F4||(event->key()==Qt::Key_F10&&event->modifiers()==Qt::ShiftModifier)||(event->key()==Qt::Key_Down&&(event->modifiers()&Qt::AltModifier));
  if(menu()&&menuKey){openMenu(mapToGlobal(rect().bottomLeft()),true);event->accept();return;}
  if(event->key()==Qt::Key_Return||event->key()==Qt::Key_Enter){click();event->accept();return;}QToolButton::keyPressEvent(event);}
 void contextMenuEvent(QContextMenuEvent* event)override{if(!menu()){event->ignore();return;}openMenu(event->reason()==QContextMenuEvent::Mouse?event->globalPos():mapToGlobal(rect().bottomLeft()),event->reason()!=QContextMenuEvent::Mouse);event->accept();}
};
QString unavailableOutput(const QJsonObject& document,const QString& action){
 const auto route=Settings::effectiveAction(document,action);const bool local=document["archive"].toObject()["keepUploads"].toBool();
 if(route["destination"]!="upload"||route["profileId"].toString().isEmpty())return local?QString{}:QString("New captures have no output. Choose a destination or turn on Keep a local copy.");
 QJsonObject profile;for(const auto& value:document["profiles"].toArray())if(value.toObject()["id"]==route["profileId"])profile=value.toObject();
 if(profile.isEmpty())return "The upload destination is unavailable. Choose an output in Settings → Capture.";
 if(route["publicationMode"]=="private"&&profile["provider"]!="proton-drive")return "This destination doesn't support private uploads. Change Sharing or Destination in Settings → Capture.";
 return {};
}
// Why the desktop can't run this action's capture, from the adapter's capabilities (#1295); empty when it can or nothing is reported.
QString unavailableSource(const QJsonObject& diagnostics,const QString& action){
 // An absent optional helper follows Record: invocation reports the installation gap.
 // The badge describes desktop support, rather than optional build contents.
 if(action=="scroll"){const auto c=diagnostics["capabilities"].toObject()["scroll"].toObject();if(c.contains("available")&&!c["available"].toBool()&&!QStringList{"recording_helper_missing","scroll_not_probed","scroll_probe_pending","scroll_probe_failed"}.contains(c["reason"].toString()))return "Scrolling capture is unavailable: "+c["reason"].toString();if(c["available"].toBool()&&c["selection"]=="none")return "Scrolling capture is unavailable: region_unavailable";return {};}
 // Copy text follows the region source and the text engine: the resident folds both into text.sources.region.
 if(action==Settings::textActionName()){const auto region=diagnostics["capabilities"].toObject()["text"].toObject()["sources"].toObject()["region"].toObject();if(!region.contains("available")||region["available"].toBool())return {};const auto message=region["error"].toObject()["message"].toString();return "Copy text is unavailable"+(message.isEmpty()?QString("."):QString(": ")+message);}
 const auto kind=action=="edit"?QString("region"):action;if(!QStringList{"region","window","desktop","monitor"}.contains(kind))return {};
 // capture.<kind> where the adapter reports it, else the older top-level source.<kind> (as Settings reads it).
 const auto source=diagnostics.value("capabilities").toObject().value("source").toObject();const auto capture=source.value("capture").toObject();
 const QJsonValue value=capture.contains(kind)?capture.value(kind):source.value(kind);const auto status=value.toObject();
 if(!value.isObject()||status["available"].toBool(true))return {};
 const auto error=status["error"];const auto message=error.isObject()?error.toObject()["message"].toString():error.toString();
 const QHash<QString,QString> names{{"region","Region capture"},{"window","Window capture"},{"desktop","Desktop capture"},{"monitor","Monitor capture"}};
 return names.value(kind)+" is unavailable on this desktop"+(message.isEmpty()?QString("."):QString(": ")+message);
}
// Only compact output and build metadata belong in the status band.
class StatusBand final:public QStatusBar {
public:
 explicit StatusBand(QWidget* parent):QStatusBar(parent){setSizeGripEnabled(false);setObjectName("statusBand");setMinimumHeight(Ui::scaled(Ui::Metric::statusBand));setContentsMargins(space(Space::l),0,space(Space::l),0);}
protected:
 bool event(QEvent* event)override{
  const bool handled=QStatusBar::event(event);
  if(event->type()==QEvent::LayoutRequest||event->type()==QEvent::Show){int height=Ui::scaled(32);for(auto* child:findChildren<QWidget*>(Qt::FindDirectChildrenOnly))if(!child->isWindow()&&child->isVisibleTo(this))height=qMax(height,qMax(child->minimumHeight(),child->sizeHint().height()));setMinimumHeight(qMax(Ui::scaled(Ui::Metric::statusBand),height+2*space(Space::xs)));}
  return handled;
 }
 void paintEvent(QPaintEvent* event)override{{QPainter p(this);p.fillRect(rect(),Theme::current().surface);p.setPen(Theme::current().border);p.drawLine(0,0,width(),0);}QStatusBar::paintEvent(event);}
};
}
MainWindow::MainWindow(UiSession& session,QWidget* parent):QMainWindow(parent),session_(session){
 chooser_=new FileDialog(this);
 setObjectName("putgdMainWindow");setWindowTitle("put.gd");setWindowIcon(Ui::icon("brand"));
 // 1,440 px holds the command band in one row at the default text size (it
 // needs 1,413 px); narrower, it wraps (fitBand).
 resize(1440,880);setMinimumSize(1060,680);
 const QHash<QString,QString> glyphs{{"region","region"},{"window","window"},{"monitor","monitor"},{"desktop","desktop"},{"clipboard","clipboard"},{"edit","edit"},{"scroll","scroll"},{"record","record"},{"text","copy-text"}};
 // The application menu holds only what the window does not already show:
 // help, updates, About and Quit. Capture commands live in the command band
 // and the pages in the header's navigation, once each.
 auto* menu=new QMenu(this);menu->setObjectName("applicationMenuPopup");
 auto* helpAction=menu->addAction("How capture actions work");helpAction->setObjectName("captureActionsHelp");
 auto* updatesAction=menu->addAction("Check for updates");updatesAction->setObjectName("checkForUpdates");
 // Opens General → Updates and starts its Check now, where checks are
 // supported; the result shows there.
 connect(updatesAction,&QAction::triggered,this,[this]{navigate("settings");settings_->showUpdates();if(auto* now=settings_->findChild<QPushButton*>("checkForUpdatesNow");now&&now->isEnabled())now->click();});
 connect(menu->addAction("About put.gd"),&QAction::triggered,this,&MainWindow::aboutRequested);
 menu->addSeparator();
 auto* quitAction=menu->addAction("Quit put.gd");quitAction->setObjectName("quitApplication");connect(quitAction,&QAction::triggered,this,&MainWindow::quitRequested);
 auto* center=new QWidget;setCentralWidget(center);auto* layout=new QVBoxLayout(center);layout->setContentsMargins(0,0,0,0);layout->setSpacing(0);
 auto* chrome=new Ui::Surface("surface",Qt::BottomEdge,center);chrome->setObjectName("mainChrome");auto* chromeBox=new QVBoxLayout(chrome);chromeBox->setContentsMargins(0,0,0,0);chromeBox->setSpacing(0);layout->addWidget(chrome);
 auto* header=Ui::WindowHeader::install(this,chromeBox,menu);header->setProperty("uiChromePlane",true);
 tabs_=new Ui::Segmented(header);tabs_->setObjectName("mainNavigation");tabs_->setAccessibleName("Main navigation");for(const auto* label:{"History","Destinations","Settings"})tabs_->addTab(label);header->setCentre(tabs_);
 // The command band: one label set at every width (council 2026-11-14). The
 // screenshots, led by Capture region, then a divider, editing and recording,
 // and at the far end the two that keep no picture. One row when it fits; else
 // the second group wraps under the first with the same labels in the same order.
 band_=new QWidget(chrome);band_->setObjectName("commandBand");band_->setAccessibleName("Capture and upload actions");band_->installEventFilter(this);
 bandBox_=new QBoxLayout(QBoxLayout::LeftToRight,band_);bandBox_->setContentsMargins(space(Space::m),space(Space::s),space(Space::m),space(Space::s));bandBox_->setSpacing(space(Space::s));chromeBox->addWidget(band_);
 firstRow_=new QHBoxLayout;firstRow_->setContentsMargins(0,0,0,0);firstRow_->setSpacing(space(Space::s));secondRow_=new QHBoxLayout;secondRow_->setContentsMargins(0,0,0,0);secondRow_->setSpacing(space(Space::s));
 divider_=Ui::rule(band_,Qt::Vertical);divider_->setObjectName("commandBandDivider");divider_->setFixedHeight(Ui::scaled(20));
 bandBox_->addLayout(firstRow_);bandBox_->addWidget(divider_,0,Qt::AlignVCenter);bandBox_->addLayout(secondRow_,1);
 const QHash<QString,QString> explanations{{"region","Drag to select a screen region (Esc to cancel). Open its menu for Repeat last region."},{"window","Capture the active app window, including its frame where supported."},{"monitor","Capture the monitor the pointer is on, at its native size."},{"desktop","Capture all enabled monitors, including desktop panels."},{"clipboard","Upload copied local files, an image, or plain text, in that order. Text URLs are uploaded as text."},{"edit","Select a screen region to redact, blur, crop, or annotate before uploading. Open its menu, or drop a PNG or JPEG here, to edit an existing image."},{"scroll","Select a scrollable area. Auto-scroll stops at the bottom. Select Done to finish manually."},{"record","Select a screen region to record. Stopping opens the review window before uploading."},{"text","Select a screen region and copy the text in it"}};
 explanations_=explanations;
 const auto actionHelp=[&]{QStringList help;for(const auto& command:Settings::actionTable())if(!command.barLabel.isEmpty())help<<command.label+": "+explanations.value(command.action);help<<QStringList{"• Each button saves finished Settings edits, then starts its action.","• Screen captures hide this window. Clipboard uploads keep it open.","• Store or clear unfinished credentials before a screen capture.","• Capture delay in the status bar applies to every capture except scrolling capture and recording.","• Settings → Capture sets the destination and Keep a local copy. With neither on, capture actions are unavailable.","• Shared uploads copy a link. Private uploads create none.","• Select the output summary in the status bar to see where each action goes. Results and recovery steps are in History."}.join("\n");return help.join("\n\n");}();
 int group=0;
 for(const auto& command:Settings::actionTable()){
  if(command.barLabel.isEmpty())continue;const auto action=command.action;
  // The far end of the second group: Copy text and Upload clipboard keep no picture.
  if(command.group==2&&group!=2)secondRow_->addStretch(1);group=command.group;
  auto* button=new CommandButton(band_);button->setText(QString(command.barLabel).replace('&',QStringLiteral("&&")));button->setFocusPolicy(Qt::StrongFocus);button->setIconSize(QSize(Ui::scaled(Ui::Metric::icon),Ui::scaled(Ui::Metric::icon)));button->setIcon(Ui::icon(glyphs.value(action)));button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);Ui::role(button,action=="region"?"lead":"quiet");button->setObjectName("captureActionButton");button->setProperty("captureAction",action);button->setProperty("glyph",glyphs.value(action));
  // The scope labels (Window, Monitor…) read as the full action to a screen reader.
  button->setAccessibleName(command.label);
  if(action=="region"){
   // The lead's ▾: Repeat last region, on the region route (#1363).
   auto* options=new QMenu(this);options->setObjectName("captureRegionMenu");
   auto* repeat=options->addAction(Ui::icon("refresh"),Settings::actionLabel(Settings::repeatActionName()));repeat->setObjectName("repeatLastRegion");connect(repeat,&QAction::triggered,this,[this]{const auto reason=unavailableOutput(session_.document(),"region");if(reason.isEmpty())emit actionRequested(Settings::repeatActionName());else showActionFailure("region",{"output_required",reason,{}});});
   button->setMenu(options);button->setPopupMode(QToolButton::MenuButtonPopup);
  }
  if(action=="edit"){
   // The button captures a region to edit; its menu edits an image that
   // already exists instead and never starts a capture, so it stays usable
   // when region capture is not. A dropped PNG or JPEG does the same.
   editButton_=button;button->setAcceptDrops(true);button->installEventFilter(this);
   auto* sources=new QMenu(this);sources->setObjectName("editSourceMenu");
   connect(sources->addAction(Ui::icon("clipboard"),"Edit clipboard image"),&QAction::triggered,this,[this]{emit editRequested("clipboard",{});});
   connect(sources->addAction(Ui::icon("images"),"Edit an image file…"),&QAction::triggered,this,&MainWindow::chooseImageToEdit);
   button->setMenu(sources);button->setPopupMode(QToolButton::MenuButtonPopup);
  }
  connect(button,&QToolButton::clicked,this,[this,action]{const auto reason=action==Settings::textActionName()?QString{}:unavailableOutput(session_.document(),action);if(reason.isEmpty())emit actionRequested(action);else showActionFailure(action,{"output_required",reason,{}});});
  (command.group==0?firstRow_:secondRow_)->addWidget(button);actionButtons_<<button;
 }
 firstRowEnd_=new QSpacerItem(0,0,QSizePolicy::Fixed,QSizePolicy::Minimum);firstRow_->addItem(firstRowEnd_);
 connect(&session_,&UiSession::accepted,this,&MainWindow::refreshCommands);
 connect(helpAction,&QAction::triggered,this,[this,actionHelp]{Ui::showHelp("How capture actions work",actionHelp,this);});
 outputWarning_=new Ui::Surface("surface",Qt::BottomEdge,center);outputWarning_->setObjectName("commandOutputWarning");auto* warningRow=new QHBoxLayout(outputWarning_);warningRow->setContentsMargins(space(Space::l),space(Space::s),space(Space::m),space(Space::s));warningRow->setSpacing(space(Space::m));outputWarningText_=new QLabel(outputWarning_);outputWarningText_->setObjectName("commandOutputWarningText");outputWarningText_->setTextFormat(Qt::PlainText);outputWarningText_->setWordWrap(true);outputWarningText_->installEventFilter(this);Ui::role(outputWarningText_,"body");auto* warningGlyph=new QLabel(outputWarning_);warningGlyph->setObjectName("commandOutputWarningGlyph");auto refreshGlyph=[warningGlyph]{const int size=Ui::scaled(18);warningGlyph->setFixedSize(size,size);warningGlyph->setPixmap(Ui::icon("alert",Theme::current().warning).pixmap(size,size));};connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,warningGlyph,refreshGlyph);refreshGlyph();warningRow->addSpacing(warningGlyph->width()+space(Space::m));warningRow->addWidget(outputWarningText_,1,Qt::AlignVCenter);auto* chooseOutput=new QPushButton("Change output",outputWarning_);chooseOutput->setObjectName("chooseCommandOutput");chooseOutput->setToolTip("Open Settings → Capture to change the output");warningRow->addWidget(chooseOutput);connect(chooseOutput,&QPushButton::clicked,this,[this]{navigate("settings");QStringList affected;for(const auto& action:Settings::presentationOrder())if(!unavailableOutput(session_.document(),action).isEmpty())affected<<action;settings_->showCaptureSettings(affected);});layout->addWidget(outputWarning_);connect(&session_,&UiSession::accepted,this,&MainWindow::updateAvailability);updateAvailability();
 // Edit an image file… whose chooser could not open: the reason under the band
 // that holds the action, with Try again. A cancel shows nothing.
 chooserFailure_=new Ui::Surface("surface",Qt::BottomEdge,center);chooserFailure_->setObjectName("editImageChooserFailure");{auto* row=new QHBoxLayout(chooserFailure_);row->setContentsMargins(space(Space::l),space(Space::s),space(Space::m),space(Space::s));row->setSpacing(space(Space::m));
  chooserError_=new QLabel(chooserFailure_);chooserError_->setObjectName("editImageChooserError");chooserError_->setTextFormat(Qt::PlainText);chooserError_->setWordWrap(true);row->addWidget(chooserError_,1);
  auto* retry=new QPushButton("Try again",chooserFailure_);retry->setObjectName("retryEditImageChooser");row->addWidget(retry);connect(retry,&QPushButton::clicked,this,[this]{chooseImageToEdit();});
  auto* dismiss=Ui::toolButton({},{},"quiet",chooserFailure_);dismiss->setObjectName("dismissEditImageChooserFailure");dismiss->setIcon(Ui::chromeIcon("close"));dismiss->setAccessibleName("Dismiss");dismiss->setToolTip("Dismiss");row->addWidget(dismiss);connect(dismiss,&QToolButton::clicked,chooserFailure_,&QWidget::hide);}
 layout->addWidget(chooserFailure_);chooserFailure_->hide();
 pages_=new QStackedWidget;pages_->setObjectName("mainPages");layout->addWidget(pages_,1);history_=new HistoryWindow(session_,pages_);pages_->addWidget(history_);
 // Keep the navigation indexes stable without constructing hidden Settings and
 // destination controls for a History-only visit (#558).
 pages_->addWidget(new QWidget(pages_));pages_->addWidget(new QWidget(pages_));
 connect(tabs_,&QTabBar::currentChanged,this,[this](int row){if(settings_)settings_->clearCompletedFeedback();if(row>0)settings();pages_->setCurrentIndex(row);if(row==1)settings_->activateDestinations();updateAvailability();emit viewStateChanged();});
 connect(history_,&HistoryWindow::viewStateChanged,this,&MainWindow::viewStateChanged);connect(history_,&HistoryWindow::activityChanged,this,&MainWindow::refreshCommands);connect(history_,&HistoryWindow::editRequested,this,&MainWindow::editRequested);connect(history_,&HistoryWindow::chooseImageRequested,this,&MainWindow::chooseImageToEdit);
 setStatusBar(new StatusBand(this));
 auto* statusContent=new QWidget;statusContent->setObjectName("statusContent");statusContent->installEventFilter(this);auto* statusRow=new QHBoxLayout(statusContent);statusRow->setContentsMargins(0,0,0,0);statusRow->setSpacing(space(Space::s));statusBar()->addWidget(statusContent,1);
 // One output control; the popup owns the per-action details and navigation.
 auto* outputGroup=outputGroup_=new QWidget;outputGroup->setObjectName("captureOutputGroup");auto* outputRow=new QHBoxLayout(outputGroup);outputRow->setContentsMargins(0,0,0,0);outputRow->setSpacing(0);
 destination_=new OutputButton(outputGroup);destination_->setFocusPolicy(Qt::StrongFocus);Ui::role(destination_,"quiet");destination_->setObjectName("captureOutputSummary");destination_->setToolButtonStyle(Qt::ToolButtonTextOnly);destination_->setSizePolicy(QSizePolicy::Maximum,QSizePolicy::Preferred);destination_->setMinimumWidth(Ui::scaled(120));destination_->setPopupMode(QToolButton::InstantPopup);outputRow->addWidget(destination_,1);outputRow->addStretch(0);
 auto* outputMenu=new QMenu(destination_);outputMenu->setObjectName("captureOutputMenu");destination_->setMenu(outputMenu);
 auto* outputRows=new QWidgetAction(outputMenu);outputRows->setObjectName("captureOutputRows");auto* outputBody=new QWidget;outputBody->setFocusPolicy(Qt::NoFocus);auto* outputGrid=new QGridLayout(outputBody);outputGrid->setContentsMargins(space(Space::m),space(Space::m),space(Space::m),space(Space::m));outputGrid->setSpacing(space(Space::s));outputRows->setDefaultWidget(outputBody);outputMenu->addAction(outputRows);connect(outputMenu,&QMenu::aboutToShow,outputMenu,[outputMenu]{populateOutputRows(outputMenu);});outputMenu->addSeparator();auto* changeOutput=outputMenu->addAction("Change output");changeOutput->setObjectName("changeCaptureOutput");
 statusRow->addWidget(outputGroup,1);
 // The capture delay (#1363), on every page: the radio group the tray's Capture
 // delay submenu shows. It never elides; accented while a delay is set.
 delay_=new OutputButton(statusContent);delay_->setObjectName("captureDelayControl");delay_->setFocusPolicy(Qt::StrongFocus);delay_->setToolButtonStyle(Qt::ToolButtonTextOnly);delay_->setPopupMode(QToolButton::InstantPopup);delay_->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Preferred);
 {auto* delays=new QMenu(delay_);delays->setObjectName("captureDelayMenu");delay_->setMenu(delays);auto* group=new QActionGroup(delays);
  for(const int seconds:Settings::captureDelayChoices()){
   auto* choice=delays->addAction(commandtext::delayChoice(seconds));choice->setCheckable(true);choice->setData(seconds);choice->setObjectName(QStringLiteral("captureDelay%1").arg(seconds));group->addAction(choice);
   connect(choice,&QAction::triggered,this,[this,seconds]{
    auto document=session_.document();auto preferences=document["preferences"].toObject();
    if(preferences["captureDelaySeconds"].toInt()==seconds)return;
    preferences["captureDelaySeconds"]=seconds;document["preferences"]=preferences;
    session_.saveAsync(document,session_.revision(),[this](Result result){updateDelay();if(!result)showActionFailure("region",result.error);});
   });
  }
  auto* hotkeys=delays->addAction(commandtext::hotkeysImmediate());hotkeys->setObjectName("captureDelayHotkeys");hotkeys->setEnabled(false);hotkeys->setProperty("visualRole","information");}
 statusRow->addWidget(delay_);
 // When the output summary is hidden (Settings → Capture), the remaining status
 // items stay together at the right instead of spreading across the band.
 statusRow->addStretch(0);updateAvailability();
 // The running build, from the resident identity About also shows. Stable and
 // nightly builds link to their GitHub release; development builds are text.
 version_=new QLabel;version_->setObjectName("appVersion");version_->setTextFormat(Qt::PlainText);Ui::role(version_,"meta");
 versionLink_=new StatusLink;versionLink_->setObjectName("appVersionLink");Ui::role(versionLink_,"version");versionLink_->setCursor(Qt::PointingHandCursor);
 auto* versionGroup=new QWidget;versionGroup->setObjectName("appVersionGroup");auto* versionLayout=new QHBoxLayout(versionGroup);versionLayout->setContentsMargins(0,0,0,0);versionLayout->addWidget(versionLink_);
 auto* versionNotice=new Ui::Surface("surface",Qt::BottomEdge,center);versionNotice->setObjectName("appVersionNotification");auto* noticeRow=new QHBoxLayout(versionNotice);noticeRow->setContentsMargins(space(Space::l),space(Space::s),space(Space::m),space(Space::s));noticeRow->setSpacing(space(Space::m));
 auto* versionBrowser=new Ui::BrowserFeedback(versionNotice);versionBrowser->setObjectName("appVersionBrowserFeedback");auto* feedbackLayout=versionBrowser->layout();delete feedbackLayout;auto* feedbackRow=new QHBoxLayout(versionBrowser);feedbackRow->setContentsMargins(0,0,0,0);feedbackRow->setSpacing(space(Space::m));feedbackRow->addWidget(versionBrowser->findChild<QLabel*>(),1);auto* copyRelease=versionBrowser->findChild<QPushButton*>();copyRelease->setText("Copy release link");copyRelease->setObjectName("copyReleaseLink");feedbackRow->addWidget(copyRelease);noticeRow->addWidget(versionBrowser,1);
 auto* dismissVersion=Ui::toolButton({},{},"quiet",versionNotice);dismissVersion->setObjectName("dismissVersionNotification");dismissVersion->setIcon(Ui::chromeIcon("close"));dismissVersion->setAccessibleName("Dismiss release link notification");dismissVersion->setToolTip("Dismiss");noticeRow->addWidget(dismissVersion);connect(dismissVersion,&QToolButton::clicked,versionBrowser,&Ui::BrowserFeedback::clear);versionBrowser->installEventFilter(this);versionNotice->hide();layout->insertWidget(layout->indexOf(pages_),versionNotice);
 connect(versionLink_,&QPushButton::clicked,this,[this,versionBrowser]{if(const auto url=releaseLink(session_.build()).url;url.isValid())host::openLink(url,versionBrowser->opening(url));});
 auto updateVersion=[this,versionBrowser]{
  const auto link=releaseLink(session_.build());const auto name="put.gd version "+link.version;const auto page="Open the v"+link.version+" release on GitHub";
  version_->setText(link.version);version_->setAccessibleName(name);version_->setToolTip(name);
  versionLink_->setText(link.version);versionLink_->setAccessibleName(name);versionLink_->setAccessibleDescription(page);versionLink_->setToolTip(page);versionLink_->setVisible(!link.url.isEmpty());if(versionLink_->property("browserUrl").toUrl()!=link.url){versionBrowser->clear();versionLink_->setProperty("browserUrl",link.url);}updateSaveStatus();
 };
 connect(&session_,&UiSession::statusChanged,this,updateVersion);updateVersion();
 // A newer version, from the resident's daily check (#293): it opens
 // General → Updates, which has the download.
 updateLink_=new StatusLink(Ui::icon("download"),"Update available");updateLink_->setObjectName("updateAvailable");Ui::role(updateLink_,"version");updateLink_->setCursor(Qt::PointingHandCursor);updateLink_->hide();
 connect(updateLink_,&QPushButton::clicked,this,[this]{navigate("settings");settings_->showUpdates();});
 auto updateAvailable=[this]{
  const auto update=session_.diagnostics()["update"].toObject();const auto result=update["result"].toObject();
  const bool available=update["supported"].toBool()&&result["updateAvailable"].toBool();
  updateLink_->setToolTip("Open Settings → General → Updates to view put.gd "+result["latest"].toString());
  updateLink_->setAccessibleName("Update available: put.gd "+result["latest"].toString());updateLink_->setVisible(available);updateSaveStatus();
 };
 connect(&session_,&UiSession::statusChanged,this,updateAvailable);updateAvailable();
 auto updateDestination=[this]{
  // The words the tray's Capture output submenu uses (commandtext::outputSummary).
  const auto summary=commandtext::outputSummary(session_.document());
  fitBand();destination_->setProperty("fullOutputSummary",summary.band);updateOutputSummary();
  auto* rows=destination_->menu()->findChild<QAction*>("captureOutputRows");rows->setProperty("outputRows",summary.rows);rows->setProperty("outputExceptions",summary.exceptions);rows->setText(summary.rows.join("\n"));populateOutputRows(destination_->menu());
  destination_->setAccessibleName(summary.band);destination_->setAccessibleDescription(summary.rows.join("\n"));destination_->setToolTip(summary.rows.join("\n"));updateDelay();updateSaveStatus();
 };
 connect(changeOutput,&QAction::triggered,this,[this]{navigate("settings");settings_->showCaptureSettings();});
 connect(history_,&HistoryWindow::captureSettingsRequested,changeOutput,&QAction::trigger);
 connect(history_,&HistoryWindow::localCopySettingsRequested,this,&MainWindow::showLocalCopySettings);
 // History's Open error log, and its Open destination settings for a file's destination (#1753).
 connect(history_,&HistoryWindow::errorLogRequested,this,&MainWindow::showErrorLog);
 connect(history_,&HistoryWindow::destinationSettingsRequested,this,&MainWindow::showDestination);
 explanation_=new QLabel("Settings save automatically");explanation_->setObjectName("autosaveExplanation");Ui::role(explanation_,"meta");statusRow->addWidget(explanation_);
 // Autosave describes the Destinations and Settings pages, not History.
 connect(tabs_,&QTabBar::currentChanged,this,[this]{updateSaveStatus();});
 saveStatus_=new QLabel;saveStatus_->setObjectName("autosaveStatus");saveStatus_->setAccessibleName("Settings save status");saveStatus_->setTextFormat(Qt::PlainText);Ui::role(saveStatus_,"meta");statusRow->addWidget(saveStatus_);
 for(auto* widget:QWidgetList{explanation_,saveStatus_,updateLink_,version_,versionGroup})widget->setSizePolicy(QSizePolicy::Fixed,QSizePolicy::Preferred);
 statusRow->addWidget(updateLink_);statusRow->addWidget(version_);statusRow->addWidget(versionGroup);
 for(auto* control:QList<QWidget*>{destination_,delay_,version_,versionLink_,updateLink_,explanation_,saveStatus_}){control->setProperty("uiStatus",true);Ui::role(control,control->property("visualRole").toByteArray().constData());}
 connect(&session_,&UiSession::accepted,this,[this,updateDestination]{updateDestination();updateSaveStatus();});
 const auto currentGaps=[this]{QString gaps;auto actions=Settings::presentationOrder();actions<<Settings::textActionName();for(const auto& action:std::as_const(actions))gaps+=unavailableSource(session_.diagnostics(),action)+'\n';return gaps;};
 // Seeded now: the first snapshot is not a change, and rebuilding an open popup for it re-lays out its labels (#1303).
 sourceGaps_=currentGaps();
 connect(&session_,&UiSession::statusChanged,this,[this,updateDestination,currentGaps]{
  // Every resident snapshot (progress, history) fires statusChanged; rebuild the output popup only when a capture gap changed.
  const auto gaps=currentGaps();
  // The buttons first (badge, tooltip, description), then the per-action output lines on top.
  if(gaps==sourceGaps_)return;sourceGaps_=gaps;refreshCommands();updateDestination();});
 updateDestination();refreshCommands();updateSaveStatus();tabs_->setCurrentIndex(0);Ui::styleWindow(*this);
 orderNavigation(header,tabs_,actionButtons_,band_);
 connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,[this,updateDestination]{QTimer::singleShot(0,this,[this,updateDestination]{updateDestination();refreshCommands();});});installEventFilter(this);
}
SettingsWindow* MainWindow::settings(){
 if(settings_)return settings_;
 settings_=new SettingsWindow(session_,pages_);
 auto* destinations=pages_->widget(1);auto* preferences=pages_->widget(2);
 pages_->removeWidget(destinations);pages_->removeWidget(preferences);delete destinations;delete preferences;
 pages_->addWidget(settings_->destinationsPage());pages_->addWidget(settings_);
 orderNavigation(findChild<Ui::WindowHeader*>("windowHeader"),tabs_,actionButtons_,band_);
 // New combo popup containers need the same themed delegate/margin refresh
 // that the original eager tree received when the main style was applied.
 Ui::styleWindow(*this);
 connect(settings_,&SettingsWindow::viewStateChanged,this,[this]{updateSaveStatus();updateAvailability();emit viewStateChanged();});
 connect(settings_,&SettingsWindow::actionRequested,this,&MainWindow::actionRequested);
 connect(settings_,&SettingsWindow::quitRequested,this,&MainWindow::quitRequested);
 connect(settings_,&SettingsWindow::destinationsRequested,this,[this]{navigate("destinations");});
 connect(settings_,&SettingsWindow::destinationSettingsRequested,this,&MainWindow::showDestination);
 connect(settings_,&SettingsWindow::historyItemRequested,this,[this](const QString& historyId){navigate("history");history_->showItem(historyId);});
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
 const auto feedback=unsaved?QString("Changes not saved"):text;
 // Council 22: the page's banner carries the warning; this line stays in the band's secondary ink.
 if(saveStatus_->text()!=feedback||saveStatus_->property("feedbackTone").toString()!="neutral")Ui::feedback(saveStatus_,feedback);
 saveStatus_->setToolTip(unsaved?text:QString());saveStatus_->setAccessibleDescription(unsaved?text:QString());
 if(updateLink_){const auto update=session_.diagnostics()["update"].toObject();updateLink_->setVisible(update["supported"].toBool()&&update["result"].toObject()["updateAvailable"].toBool());}
 // "Settings save automatically" is the band's least important line: it gives
 // way before the output summary has to elide (narrow windows, large text).
 const auto* content=findChild<QWidget*>("statusContent");auto* row=content->layout();const int gap=row->spacing();
 const auto link=releaseLink(session_.build());auto* versionGroup=findChild<QWidget*>("appVersionGroup");
 // Nested layouts can require more than their text advances, especially after
 // a font change. Use Qt's effective item sizes and leave one grid gap spare.
 int needed=row->contentsMargins().left()+row->contentsMargins().right(),count=0;
 for(int i=0;i<row->count();++i){auto* item=row->itemAt(i);auto* widget=item->widget();if(!widget||widget->isHidden()||widget==explanation_||widget==version_||widget==versionGroup)continue;needed+=item->sizeHint().expandedTo(item->minimumSize()).width()+(count++?gap:0);}
 const auto width=[](QWidget* widget){return widget->sizeHint().expandedTo(widget->minimumSizeHint()).expandedTo(widget->minimumSize()).width();};
 const int versionWidth=width(link.url.isEmpty()?static_cast<QWidget*>(version_):versionGroup),available=content->contentsRect().width()-space(Space::s);
 const bool versionRoom=!unsaved&&!link.version.isEmpty()&&needed+versionWidth+(count?gap:0)<=available;
 version_->setVisible(versionRoom&&!link.version.isEmpty()&&link.url.isEmpty());versionGroup->setVisible(versionRoom&&!link.url.isEmpty());
 // The explanation goes first: it never shows while the version had to give way.
 explanation_->setVisible(tabs_->currentIndex()>0&&!unsaved&&(versionRoom||link.version.isEmpty())&&needed+(versionRoom?versionWidth+gap:0)+width(explanation_)+gap<=available);
}
// One label set at every width: one row when it fits, else the second group
// under the first, in the same reading order. The window is never narrower
// than the wider row, so no label shortens, hides or folds.
void MainWindow::fitBand(){
 if(!band_||fittingBand_)return;fittingBand_=true;
 // A short window keeps one 4 px inset around the measured navigation track.
 if(auto* header=findChild<Ui::WindowHeader*>("windowHeader"))header->setFixedHeight(height()<Ui::scaled(720)?qMax(Ui::scaled(48),tabs_->sizeHint().height()+2*space(Space::xs)):Ui::scaled(Ui::Metric::header));
 for(auto* button:std::as_const(actionButtons_))button->setMinimumWidth(button->sizeHint().width());
 firstRow_->invalidate();secondRow_->invalidate();
 const auto margins=bandBox_->contentsMargins();const int gap=bandBox_->spacing(),edges=margins.left()+margins.right();
 const int first=firstRow_->sizeHint().width(),second=secondRow_->sizeHint().width();
 const int oneRow=edges+first+gap+divider_->sizeHint().width()+gap+second,twoRows=edges+qMax(first,second);
 setMinimumWidth(qMax(1060,twoRows));
 const bool wrap=oneRow>qMax(band_->width(),width());
 if(wrap!=bandWrapped_||!band_->property("bandWidths").isValid()){
  bandWrapped_=wrap;bandBox_->setDirection(wrap?QBoxLayout::TopToBottom:QBoxLayout::LeftToRight);divider_->setVisible(!wrap);
  firstRowEnd_->changeSize(0,0,wrap?QSizePolicy::Expanding:QSizePolicy::Fixed,QSizePolicy::Minimum);firstRow_->invalidate();
 }
 // What the catalog reports (command-band-measurements.json).
 band_->setProperty("bandWidths",QVariantMap{{"oneRow",oneRow},{"twoRows",twoRows},{"firstRow",first},{"secondRow",second}});band_->setProperty("wrapped",wrap);
 bandBox_->invalidate();fittingBand_=false;
}
void MainWindow::updateDelay(){
 if(!delay_)return;
 const auto preferences=session_.document()["preferences"].toObject();const int delay=preferences["captureDelaySeconds"].toInt();const bool hotkeys=preferences["captureDelayShortcuts"].toBool();
 const auto title=commandtext::delayTitle(delay);
 if(delay_->text()!=title)delay_->setText(title);
 // A set delay is the lead role's accent, so it is not missed; none is a quiet status control.
 if(delay_->property("delaySet")!=QVariant(delay>0)||!delay_->property("delaySet").isValid()){delay_->setProperty("delaySet",delay>0);delay_->setProperty("uiStatus",delay==0);Ui::role(delay_,delay>0?"lead":"quiet");}
 for(auto* choice:delay_->menu()->actions())if(choice->isCheckable())choice->setChecked(choice->data().toInt()==delay);
 if(auto* row=delay_->menu()->findChild<QAction*>("captureDelayHotkeys"))row->setVisible(delay>0&&!hotkeys);
 delay_->setAccessibleName(title);
 const auto tip=delay>0?(hotkeys?QStringLiteral("Wait %1 seconds before captures (hotkeys wait too)"):QStringLiteral("Wait %1 seconds before captures (hotkeys capture immediately)")).arg(delay):QStringLiteral("Add a countdown before captures");
 delay_->setToolTip(tip);delay_->setAccessibleDescription(tip+". Scrolling capture and recording never wait.");
 delay_->setMinimumWidth(delay_->sizeHint().width());
}
// Each action button's state, tooltip and description: availability, its
// hotkey, its output line, the capture delay and a recording's phase.
void MainWindow::refreshCommands(){
 const auto document=session_.document();const auto summary=commandtext::outputSummary(document);const auto defaults=document["captureDefaults"].toObject();
 const int delay=document["preferences"].toObject()["captureDelaySeconds"].toInt();
 QString phase;if(history_)for(const auto& value:history_->activeOperations())if(const auto text=commandtext::recordingPhase(value.toObject());!text.isEmpty())phase=text;
 for(auto* button:std::as_const(actionButtons_)){
  const auto action=button->property("captureAction").toString();const bool routed=Settings::presentationOrder().contains(action);
  const auto route=Settings::effectiveAction(document,action);const auto key=QKeySequence(route["shortcut"].toString()).toString(QKeySequence::NativeText);
  const auto output=routed?unavailableOutput(document,action):QString{};const auto source=unavailableSource(session_.diagnostics(),action);
  const bool ownOutput=route["destination"]!=defaults["destination"]||route["profileId"]!=defaults["profileId"]||route["publicationMode"]!=defaults["publicationMode"];
  button->setEnabled(true);button->setProperty("captureUnavailable",!(output.isEmpty()?source:output).isEmpty());button->setProperty("captureBadge",!source.isEmpty()||(!output.isEmpty()&&ownOutput));
  if(action=="region")Ui::role(button,"lead");button->setIcon(Ui::icon(button->property("glyph").toString()));
  // Both gaps, as the tooltip lists them, so a screen reader hears why this action can't run.
  QStringList gapList;if(!output.isEmpty())gapList<<output;if(!source.isEmpty())gapList<<source;const auto gaps=gapList.join(' ');
  auto description=gaps.isEmpty()?explanations_.value(action):(action=="edit"?"Region capture unavailable: "+gaps+" Existing images can still be edited.":"Unavailable: "+gaps);
  if(routed)description+="\nOutput: "+summary.output.value(action);
  // The label stays "Record screen" during a recording; the phase is said here.
  if(action==Settings::recordingActionName()&&!phase.isEmpty())description+="\n"+phase;
  button->setAccessibleDescription(description);
  QStringList tip{button->accessibleName()+(key.isEmpty()?QString{}:QString(" · ")+key)};
  // Copy text has no output line: its tooltip says what it does instead. Record screen says what it does above its output line.
  if(!routed)tip<<explanations_.value(action);else{if(action==Settings::recordingActionName())tip<<explanations_.value(action);tip<<summary.detail.value(action);}
  if(!output.isEmpty())tip<<output;if(!source.isEmpty())tip<<source;
  if(delay>0&&Settings::delayableAction(action))tip<<commandtext::delayTooltip(delay);
  if(action==Settings::recordingActionName()&&!phase.isEmpty())tip<<phase;
  button->setToolTip(tip.join('\n'));
 }
}
void MainWindow::updateAvailability(){
 QStringList unavailable,reasons;int routed=0;for(auto* button:actionButtons_){if(button->property("captureAction")==Settings::textActionName())continue;++routed;const auto reason=unavailableOutput(session_.document(),button->property("captureAction").toString());if(reason.isEmpty())continue;unavailable<<button->accessibleName();if(!reasons.contains(reason))reasons<<reason;}
 const auto message=unavailable.isEmpty()?QString{}:unavailable.size()==routed?reasons.join(" "):"Choose an output for "+unavailable.join(", ")+".";outputWarningText_->setText(message);outputWarningText_->setAccessibleDescription(message+" Opens Settings → Capture.");auto* recovery=findChild<QWidget*>("actionRecoveryNotification");const bool capture=tabs_&&tabs_->currentIndex()==2&&settings_&&settings_->findChild<QTabWidget*>("settingsTabs")->currentIndex()==1;const auto* inlineRecovery=settings_?settings_->findChild<QWidget*>("captureOutputBlock"):nullptr;if(capture&&inlineRecovery&&!inlineRecovery->isHidden()&&recovery&&recovery->property("outputRecovery").toBool())recovery->hide();outputWarning_->setVisible(!(settings_&&settings_->changesUnsaved())&&!capture&&!unavailable.isEmpty()&&!(recovery&&!recovery->isHidden()&&recovery->property("outputRecovery").toBool()));if(outputGroup_)outputGroup_->setVisible(!capture);
}
void MainWindow::showActionFailure(const QString& action,const Error& error){
 const bool output=error.code=="output_required",local=error.code=="archive_failed";
 if(output)showCaptureSettings({action});else if(local)showLocalCopySettings();else {navigate("destinations");const auto route=Settings::effectiveAction(session_.document(),action);auto* profiles=findChild<QListWidget*>("destinationList");const auto destinations=session_.document()["profiles"].toArray();QString provider;for(int i=0;i<destinations.size();++i)if(destinations[i].toObject()["id"]==route["profileId"]){if(profiles)profiles->setCurrentRow(i);provider=destinations[i].toObject()["provider"].toString();}const auto target=(provider=="s3"||provider=="backblaze-b2")?QString("s3Bucket"):provider=="proton-drive"?QString("protonFolder"):QString("destinationLabel");QTimer::singleShot(0,this,[this,target]{auto* control=findChild<QWidget*>(target);if(!control||!control->isVisible())control=findChild<QWidget*>("destinationLabel");if(control){if(auto* scroll=findChild<QScrollArea*>("destinationScroll"))scroll->ensureWidgetVisible(control,24,24);control->setFocus(Qt::OtherFocusReason);control->setAttribute(Qt::WA_KeyboardFocusChange);}});}
 // A notice like the others: a warning banner with its recovery action and ×, never danger-coloured text (council 22).
 auto* notice=findChild<QWidget*>("actionRecoveryNotification");if(!notice){notice=new Ui::Surface("bg",Qt::BottomEdge,centralWidget());notice->setObjectName("actionRecoveryNotification");auto* row=new QHBoxLayout(notice);row->setContentsMargins(space(Space::m),space(Space::s),space(Space::m),space(Space::s));auto* banner=new Ui::Banner(Ui::Banner::Tone::Warning,{},notice);banner->setObjectName("actionRecoveryBanner");row->addWidget(banner);auto* text=banner->textLabel();text->setObjectName("actionRecoveryError");banner->setAction(QStringLiteral(" "));auto* retry=banner->actionButton();retry->setObjectName("actionRecoveryLink");connect(retry,&QPushButton::clicked,this,[this,retry]{const auto action=retry->property("action").toString(),page=retry->property("page").toString();if(page=="capture")showCaptureSettings({action});else if(page=="local")showLocalCopySettings();else {const Error error{retry->property("code").toString(),retry->property("message").toString(),{}};showActionFailure(action,error);}});banner->setDismissible(true,"Dismiss action error");auto* dismiss=banner->dismissButton();dismiss->setObjectName("dismissActionRecovery");connect(dismiss,&QToolButton::clicked,this,[this,notice]{notice->hide();updateAvailability();});auto* box=static_cast<QVBoxLayout*>(centralWidget()->layout());box->insertWidget(box->indexOf(pages_),notice);connect(tabs_,&QTabBar::currentChanged,this,[this,notice]{notice->hide();updateAvailability();});}
 auto* banner=notice->findChild<Ui::Banner*>("actionRecoveryBanner");banner->setText(error.message);auto* retry=notice->findChild<QPushButton*>("actionRecoveryLink");banner->setAction(output?"Change output":local?"Keep local copies":"Review destination");retry->setProperty("action",action);retry->setProperty("code",error.code);retry->setProperty("message",error.message);retry->setProperty("page",output?"capture":local?"local":"destinations");notice->setProperty("outputRecovery",output);notice->show();updateAvailability();
}
void MainWindow::showErrorLog(const QJsonObject& target){settings();navigate("settings");settings_->showErrorLog(target);}
// Destinations, with this destination selected; the page as it is when it no longer exists.
void MainWindow::showDestination(const QString& profileId){settings();navigate("destinations");settings_->selectDestination(profileId);}
void MainWindow::showCaptureSettings(const QStringList& actions){navigate("settings");settings_->showCaptureSettings(actions);}
void MainWindow::showLocalCopySettings(){navigate("settings");settings_->showLocalCopySettings();}
MainWindow::~MainWindow(){
 if(settings_)disconnect(settings_,nullptr,this,nullptr);disconnect(history_,nullptr,this,nullptr);disconnect(tabs_,nullptr,this,nullptr);disconnect(&session_,nullptr,this,nullptr);
}
bool MainWindow::chooseImageToEdit(){
 auto folder=QStandardPaths::writableLocation(QStandardPaths::PicturesLocation);if(folder.isEmpty())folder=QDir::homePath();
 return chooser_->open({FileDialog::Mode::Open,"Edit an image file — put.gd",folder,{},{{"Images",{"*.png","*.jpg","*.jpeg"}}}},[this](const FileDialog::Result& result){imageChooserFinished(result);});
}
void MainWindow::imageChooserFinished(const FileDialog::Result& result){
 chooserFailure_->setVisible(result.failed());
 if(result.failed())Ui::feedback(chooserError_,"Couldn't open the file chooser"+(result.error.isEmpty()?QString("."):": "+result.error.left(1).toLower()+result.error.mid(1)+(result.error.endsWith('.')?"":".")),true);
 if(result.chosen())emit editRequested("file",result.path);
}
QString MainWindow::droppedImage(const QMimeData* data){
 if(!data||data->urls().size()!=1||!data->urls().first().isLocalFile())return {};
 const QFileInfo file(data->urls().first().toLocalFile());
 return file.isFile()&&QStringList{"png","jpg","jpeg"}.contains(file.suffix().toLower())?file.absoluteFilePath():QString{};
}
bool MainWindow::eventFilter(QObject* watched,QEvent* event){
 if(watched==outputWarningText_&&(event->type()==QEvent::Move||event->type()==QEvent::Resize||event->type()==QEvent::Show||event->type()==QEvent::FontChange))if(auto* glyph=outputWarning_->findChild<QLabel*>("commandOutputWarningGlyph"))glyph->move(outputWarning_->layout()->contentsMargins().left(),outputWarningText_->y()+(outputWarningText_->fontMetrics().lineSpacing()-glyph->height())/2);
 if(watched->objectName()=="appVersionBrowserFeedback"&&(event->type()==QEvent::ShowToParent||event->type()==QEvent::HideToParent))static_cast<QWidget*>(watched)->parentWidget()->setVisible(event->type()==QEvent::ShowToParent);
 if((watched==this||watched==band_)&&event->type()==QEvent::Resize)fitBand();
 // Refit after Qt finishes the layout/font event, never inside its geometry update.
 if(watched->objectName()=="statusContent"&&(event->type()==QEvent::Resize||event->type()==QEvent::LayoutRequest||event->type()==QEvent::FontChange||event->type()==QEvent::StyleChange)&&!statusFitPending_){statusFitPending_=true;QTimer::singleShot(0,this,[this]{updateSaveStatus();statusFitPending_=false;});}
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
