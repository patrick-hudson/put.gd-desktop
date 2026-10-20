#include "HistoryWindow.h"
#include "ViewState.h"
#include "VisualStyle.h"
#include "RedesignTheme.h"
#include "ByteSize.h"
#include <QApplication>
#include <QAction>
#include <QKeySequence>
#include <QClipboard>
#include <QComboBox>
#include <QDesktopServices>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QDir>
#include <QGridLayout>
#include <QHideEvent>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPlainTextEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QResizeEvent>
#include <QScrollArea>
#include <QSaveFile>
#include <QShowEvent>
#include <QSignalBlocker>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <algorithm>
#include <utility>
namespace putgd {
namespace {
// A push button laid out by its child row. It reports that row's size in the
// current font and theme, so a wider desktop font never clips the label.
class RowButton final:public QPushButton {
public:
 using QPushButton::QPushButton;
 QSize sizeHint()const override{const auto row=layout()?layout()->totalSizeHint():QSize();return {row.width(),std::max(row.height(),QPushButton::sizeHint().height())};}
 QSize minimumSizeHint()const override{return sizeHint();}
};
// A video's poster carries a play glyph and its length, so a still is never
// mistaken for a screenshot (#112).
bool video(const QJsonObject& record){return record["category"]=="videos"||record["mime"].toString().startsWith("video/");}
void paintVideoBadge(QPainter& p,const QRect& picture,const QString& length){
 using namespace RedesignTheme;p.save();p.setRenderHint(QPainter::Antialiasing);p.setPen(Qt::NoPen);p.setBrush(mediaScrim());
 const int size=qBound(18,qMin(picture.width(),picture.height())*2/5,48);const QRect circle(picture.center()-QPoint(size/2,size/2),QSize(size,size));
 p.drawEllipse(circle);VisualStyle::icon("play",onMedia()).paint(&p,circle.adjusted(size/4+size/16,size/4,-size/4+size/16,-size/4));
 if(!length.isEmpty()&&picture.height()>=40){
  auto font=p.font();font.setPixelSize(qBound(10,size/3,13));font.setBold(true);p.setFont(font);const QFontMetrics metrics(font);
  const QSize pill(metrics.horizontalAdvance(length)+10,metrics.height()+2);const QRect box(picture.bottomRight()-QPoint(pill.width()+3,pill.height()+3),pill);
  p.setBrush(mediaScrim());p.drawRoundedRect(box,4,4);p.setPen(onMedia());p.drawText(box,Qt::AlignCenter,length);
 }
 p.restore();
}
class HistoryDelegate final:public QStyledItemDelegate {
public:
 explicit HistoryDelegate(QListView* view):QStyledItemDelegate(view),view_(view){}
 std::function<void(const QModelIndex&)> primary;
 std::function<void(const QModelIndex&,const QPoint&)> menu;
 bool grid()const{return view_->viewMode()==QListView::IconMode;}
 int heading(const QModelIndex& i)const{return !grid()&&!i.data(HistoryModel::DayRole).toString().isEmpty()?34:0;}
 QRect primaryRect(QRect r,const QModelIndex& i)const{r.adjust(6,heading(i)+3,-6,-3);return grid()?QRect(r.left()+14,r.bottom()-44,r.width()-66,32):QRect(r.right()-140,r.center().y()-16,98,32);}
 QRect menuRect(QRect r,const QModelIndex& i)const{auto b=primaryRect(r,i);return QRect(grid()?r.right()-46:b.right()+8,b.top(),32,32);}
 QSize sizeHint(const QStyleOptionViewItem&,const QModelIndex& i)const override{return grid()?QSize(248,276):QSize(390,106+heading(i));}
 void paint(QPainter* p,const QStyleOptionViewItem& option,const QModelIndex& i)const override{
  using namespace RedesignTheme;p->save();p->setRenderHint(QPainter::Antialiasing);auto outer=option.rect;const auto group=i.data(HistoryModel::DayRole).toString();
  if(heading(i)){auto f=option.font;f.setPointSizeF(qMax(8.0,f.pointSizeF()-1));f.setBold(true);p->setFont(f);p->setPen(textMuted());p->drawText(outer.adjusted(16,0,-8,0),Qt::AlignLeft|Qt::AlignTop,group);}
  auto r=outer.adjusted(6,heading(i)+3,-6,-3);const bool chosen=option.state&QStyle::State_Selected;const bool failed=i.data(HistoryModel::FailedRole).toBool();
  p->setPen(QPen(chosen?accent():(failed?danger():border()),chosen?1.5:1));p->setBrush(chosen?selected():(grid()?surface():bg()));p->drawRoundedRect(r,9,9);
  QRect thumb=grid()?QRect(r.left()+1,r.top()+1,r.width()-2,145):QRect(r.left()+12,r.top()+15,74,56);p->setPen(Qt::NoPen);p->setBrush(surfaceRaised());p->drawRoundedRect(thumb,6,6);
  const auto image=qvariant_cast<QImage>(i.data(HistoryModel::ThumbnailRole));if(!image.isNull()){const auto scaled=image.scaled(thumb.size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);const QPoint origin(thumb.center().x()-scaled.width()/2,thumb.center().y()-scaled.height()/2);p->drawImage(origin,scaled);if(video(i.data(HistoryModel::RecordRole).toJsonObject()))paintVideoBadge(*p,QRect(origin,scaled.size()),HistoryModel::duration(image.text(HistoryModel::durationKey).toLongLong()));}else{const auto category=i.data(HistoryModel::RecordRole).toJsonObject()["category"].toString();VisualStyle::icon(category).paint(p,QRect(thumb.center()-QPoint(14,14),QSize(28,28)));}
  const auto action=i.data(HistoryModel::PrimaryActionRole).toString();const auto primaryBounds=primaryRect(outer,i);const auto moreBounds=menuRect(outer,i);
  const int left=grid()?r.left()+14:thumb.right()+16;const int top=grid()?r.top()+158:r.top()+17;const int right=grid()?r.right()-14:r.right()-14;const int textWidth=qMax(20,right-left);
  const int titleWidth=grid()?textWidth:qMax(20,primaryBounds.left()-left-10);auto f=option.font;f.setBold(true);p->setFont(f);p->setPen(text());p->drawText(QRect(left,top,titleWidth,24),Qt::AlignVCenter,QFontMetrics(f).elidedText(i.data().toString(),Qt::ElideRight,titleWidth));
  p->setFont(option.font);const auto record=i.data(HistoryModel::RecordRole).toJsonObject();const auto op=record["operation"].toObject();const bool copied=op["copyStatus"]=="copied";const auto copiedText=record["uploadStatus"]=="local_only"?QString("Copied to clipboard"):QString("Link copied");p->setPen(failed?danger():copied?success():textMuted());
  const int metaWidth=grid()?textWidth:qMax(20,primaryBounds.left()-left-10);auto subtitle=i.data(HistoryModel::SubtitleRole).toString();if(failed)subtitle=HistoryModel::failureLabel(record)+(grid()&&record["uploadStatus"]!="local_only"&&record["local"].toObject()["status"]=="saved"?" · Saved locally":"");else if(copied){if(grid())subtitle=HistoryModel::friendlyAction(op["action"].toString(record["category"].toString()))+" · "+HistoryModel::fileSize(record["bytes"].toInteger())+" · "+copiedText;else subtitle.remove(" · "+copiedText);}p->drawText(QRect(left,top+28,metaWidth,23),Qt::AlignVCenter,option.fontMetrics.elidedText(subtitle,Qt::ElideRight,metaWidth));
  if(!grid()){const auto state=failed?HistoryModel::statusText(record):copied?copiedText:QString{};p->setPen(failed?danger():success());p->drawText(QRect(left,top+55,textWidth,20),Qt::AlignVCenter,option.fontMetrics.elidedText(state,Qt::ElideRight,textWidth));}
  if(!action.isEmpty()){p->setPen(QPen(failed?accent():borderStrong()));p->setBrush(failed?accent():surface());p->drawRoundedRect(primaryBounds,6,6);p->setPen(failed?bg():text());p->drawText(primaryBounds,Qt::AlignCenter,action);}
  p->setPen(QPen(borderStrong()));p->setBrush(surface());p->drawRoundedRect(moreBounds,6,6);p->setPen(textSecondary());p->drawText(moreBounds,Qt::AlignCenter,QStringLiteral("⋯"));
  if(i.data(HistoryModel::ActiveRole).toBool()){const int progress=i.data(HistoryModel::ProgressRole).toInt();if(progress>=0){QRect bar(left,grid()?r.bottom()-53:r.bottom()-10,qMax(20,textWidth),3);p->setPen(Qt::NoPen);p->setBrush(border());p->drawRoundedRect(bar,1,1);bar.setWidth(bar.width()*progress/100);p->setBrush(accent());p->drawRoundedRect(bar,1,1);}}
  if(option.state&QStyle::State_HasFocus){p->setPen(QPen(accent(),1,Qt::DashLine));p->setBrush(Qt::NoBrush);p->drawRoundedRect(r.adjusted(3,3,-3,-3),7,7);}p->restore();
 }
 bool editorEvent(QEvent* event,QAbstractItemModel*,const QStyleOptionViewItem& option,const QModelIndex& i)override{
  if(event->type()!=QEvent::MouseButtonRelease)return false;
  auto* mouse=static_cast<QMouseEvent*>(event);if(mouse->button()!=Qt::LeftButton)return false;
  if(!i.data(HistoryModel::PrimaryActionRole).toString().isEmpty()&&primaryRect(option.rect,i).contains(mouse->position().toPoint())){if(primary)primary(i);return true;}
  if(menuRect(option.rect,i).contains(mouse->position().toPoint())){if(menu)menu(i,mouse->globalPosition().toPoint());return true;}return false;
 }
private:QListView* view_;
};
}
HistoryWindow::HistoryWindow(UiSession& session,QWidget* parent):RetainedWindow(parent),session_(session){
 setObjectName("historyPage");setWindowTitle("put.gd — History");resize(1080,760);auto* layout=new QVBoxLayout(this);layout->setContentsMargins(24,22,24,18);layout->setSpacing(14);
 auto* header=new QHBoxLayout;auto* heading=new QLabel("History",this);VisualStyle::role(heading,"title");header->addWidget(heading);header->addStretch();
 // Edit an image that is not a fresh capture. Icon, label and IconArt chevron
 // are child widgets, like the rail buttons, so the chevron matches the theme.
 auto* editImage=new RowButton(this);editImage->setObjectName("historyEditImage");editImage->setAccessibleName("Edit image");editImage->setToolTip("Edit the clipboard image or an image file, then upload it");
 auto* editRow=new QHBoxLayout(editImage);editRow->setContentsMargins(13,0,11,0);editRow->setSpacing(6);
 auto* editIcon=new QLabel(editImage);auto* editText=new QLabel("Edit image",editImage);editText->setObjectName("historyEditText");
 auto* editChevron=new QLabel(editImage);editChevron->setObjectName("historyEditChevron");
 // Pixmaps keep the colours they were drawn in: redraw them, and the summary's
 // HTML colours, when the scheme switches (#60).
 const auto paintGlyphs=[editIcon,editChevron]{editIcon->setPixmap(VisualStyle::icon("edit").pixmap(16,16));editChevron->setPixmap(VisualStyle::icon("chevron-down",RedesignTheme::textSecondary()).pixmap(12,12));};
 paintGlyphs();connect(&Theme::Notifier::instance(),&Theme::Notifier::changed,this,[this,paintGlyphs]{paintGlyphs();updateSummary();});
 for(auto* label:{editIcon,editText,editChevron}){label->setAttribute(Qt::WA_TransparentForMouseEvents);editRow->addWidget(label);}
 editRow->insertSpacing(2,2);
 auto* editSources=new QMenu(editImage);editSources->setObjectName("historyEditSources");
 connect(editSources->addAction(VisualStyle::icon("clipboard"),"Edit clipboard image"),&QAction::triggered,this,[this]{emit editRequested("clipboard",{});});
 connect(editSources->addAction(VisualStyle::icon("images"),"Edit an image file…"),&QAction::triggered,this,&HistoryWindow::chooseImageRequested);
 editImage->setMenu(editSources);header->addWidget(editImage);header->addSpacing(8);
 auto* listButton=new QToolButton(this);listButton->setObjectName("historyListMode");listButton->setText("List");listButton->setCheckable(true);listButton->setChecked(true);listButton->setAccessibleName("Show history as a list");grid_=new QToolButton(this);grid_->setObjectName("historyGridMode");grid_->setText("Gallery");grid_->setCheckable(true);grid_->setAccessibleName("Show history as a thumbnail gallery");header->addWidget(listButton);header->addWidget(grid_);layout->addLayout(header);
 connect(listButton,&QToolButton::clicked,this,[this]{setGridMode(false);});connect(grid_,&QToolButton::clicked,this,[this]{setGridMode(true);});
 activeBanner_=new QWidget(this);activeBanner_->setObjectName("historyActiveBanner");VisualStyle::role(activeBanner_,"card");auto* activeLayout=new QHBoxLayout(activeBanner_);activeLabel_=new QLabel(this);activeLabel_->setTextFormat(Qt::PlainText);activeLabel_->setWordWrap(true);activeLayout->addWidget(activeLabel_,1);progress_=new QProgressBar(this);progress_->setAccessibleName("Active upload progress");progress_->setMaximumWidth(180);activeLayout->addWidget(progress_);auto* activeCancel=new QPushButton("Cancel",this);activeCancel->setAccessibleName("Cancel active capture or upload");activeLayout->addWidget(activeCancel);connect(activeCancel,&QPushButton::clicked,this,[this]{if(activeId_.isEmpty())return;session_.request("operation.cancel",{{"id",activeId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Cancellation requested.",bool(e));g->refresh();}});});layout->addWidget(activeBanner_);activeBanner_->hide();
 auto* searchRow=new QHBoxLayout;auto* search=new QLineEdit(this);search->setObjectName("historySearch");search->setAccessibleName("Search history by file name");search->setPlaceholderText("Search history");search->setMaxLength(256);searchRow->addWidget(search,1);
 auto* filter=new QPushButton("Search",this);searchRow->addWidget(filter);auto* advancedToggle=new QToolButton(this);advancedToggle->setObjectName("historyFiltersExpanded");advancedToggle->setText("Filters");advancedToggle->setCheckable(true);advancedToggle->setAccessibleName("Show advanced history filters");searchRow->addWidget(advancedToggle);layout->addLayout(searchRow);
 auto* advanced=new QWidget(this);auto* filters=new QGridLayout(advanced);filters->setContentsMargins(0,0,0,0);const QStringList keys{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"};const QStringList labels{"From date (ISO)","To date (ISO)","Content type","Destination ID (advanced)","Upload state","Local-copy state","Remote-deletion state"};for(int i=0;i<keys.size();++i){auto* edit=new QLineEdit(this);edit->setMaxLength(256);edit->setObjectName("historyFilter"+keys[i]);edit->setPlaceholderText(labels[i]);edit->setAccessibleName(labels[i]);filters->addWidget(edit,i/4,i%4);connect(edit,&QLineEdit::returnPressed,this,&HistoryWindow::applyFilters);}layout->addWidget(advanced);advanced->hide();connect(advancedToggle,&QToolButton::toggled,advanced,&QWidget::setVisible);
 previewRequestTimer_=new QTimer(this);previewRequestTimer_->setSingleShot(true);previewRequestTimer_->setInterval(400);connect(previewRequestTimer_,&QTimer::timeout,this,&HistoryWindow::requestPreview);
 searchTimer_=new QTimer(this);searchTimer_->setSingleShot(true);searchTimer_->setInterval(250);connect(searchTimer_,&QTimer::timeout,this,&HistoryWindow::applyFilters);connect(search,&QLineEdit::textEdited,this,[this]{searchTimer_->start();});connect(search,&QLineEdit::returnPressed,this,&HistoryWindow::applyFilters);connect(filter,&QPushButton::clicked,this,&HistoryWindow::applyFilters);
 model_=new HistoryModel(session,this);table_=new QListView(this);table_->setObjectName("historyList");table_->setAccessibleName("Captures and upload activity");table_->setModel(model_);table_->setSelectionMode(QAbstractItemView::ExtendedSelection);table_->setResizeMode(QListView::Adjust);table_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);table_->setSpacing(3);table_->setContextMenuPolicy(Qt::CustomContextMenu);auto* delegate=new HistoryDelegate(table_);table_->setItemDelegate(delegate);delegate->primary=[this](const QModelIndex& i){primaryAction(i);};delegate->menu=[this](const QModelIndex& i,const QPoint& p){showRowMenu(i,p);};connect(table_,&QWidget::customContextMenuRequested,this,[this](const QPoint& p){const auto i=table_->indexAt(p);if(i.isValid())showRowMenu(i,table_->viewport()->mapToGlobal(p));});auto* rowMenu=new QAction(this);rowMenu->setShortcuts({QKeySequence(Qt::Key_Menu),QKeySequence(Qt::SHIFT|Qt::Key_F10)});rowMenu->setShortcutContext(Qt::WidgetShortcut);table_->addAction(rowMenu);connect(rowMenu,&QAction::triggered,this,[this]{const auto i=table_->currentIndex();if(i.isValid())showRowMenu(i,table_->viewport()->mapToGlobal(table_->visualRect(i).center()));});
 empty_=new QLabel("No captures yet\nCapture a region, window, desktop or clipboard to get started.",this);empty_->setObjectName("historyEmpty");empty_->setWordWrap(true);empty_->setAlignment(Qt::AlignCenter);VisualStyle::role(empty_,"muted");list_=new QStackedWidget(this);list_->addWidget(table_);list_->addWidget(empty_);
 split_=new QSplitter(Qt::Horizontal,this);split_->setChildrenCollapsible(false);split_->addWidget(list_);inspectorScroll_=new QScrollArea(this);inspectorScroll_->setObjectName("historyInspectorScroll");inspectorScroll_->setWidgetResizable(true);inspectorScroll_->setFrameShape(QFrame::NoFrame);inspectorScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);auto* inspector=new QWidget(this);inspector->setObjectName("historyInspector");auto* inspect=new QVBoxLayout(inspector);inspect->setContentsMargins(22,8,4,16);inspect->setSpacing(10);
 preview_=new QLabel("Select a capture to preview",this);preview_->setObjectName("historyPreview");VisualStyle::role(preview_,"card");preview_->setAccessibleName("Selected capture preview");preview_->setAlignment(Qt::AlignCenter);preview_->setWordWrap(true);preview_->setMinimumHeight(300);preview_->setMaximumHeight(340);preview_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);inspect->addWidget(preview_);
 title_=new QLabel(this);title_->setTextFormat(Qt::PlainText);title_->setObjectName("historyTitle");title_->setWordWrap(true);VisualStyle::role(title_,"title");inspect->addWidget(title_);outcome_=new QLabel(this);outcome_->setTextFormat(Qt::PlainText);outcome_->setWordWrap(true);outcome_->setObjectName("historyOutcome");inspect->addWidget(outcome_);
 items_=new QComboBox(this);items_->setObjectName("historyItems");items_->setAccessibleName("Select a file in this capture");items_->hide();inspect->addWidget(items_);connect(items_,&QComboBox::currentIndexChanged,this,[this]{if(restoringSelection_)return;selectedItemId_=items_->currentData().toString();loadSelectedItem();emit viewStateChanged();});
 auto* linkRow=new QHBoxLayout;link_=new QLineEdit(this);link_->setObjectName("historyLink");link_->setReadOnly(true);VisualStyle::role(link_,"link");link_->setAccessibleName("Stored share link");link_->setPlaceholderText("No share link available");linkRow->addWidget(link_,1);copy_=new QPushButton("Copy link",this);copy_->setObjectName("copyHistoryLink");VisualStyle::role(copy_,"primary");linkRow->addWidget(copy_);inspect->addLayout(linkRow);
 auto* actions=new QGridLayout;actions->setSpacing(8);open_=new QPushButton("Open in browser",this);open_->setObjectName("openHistoryLink");copyImage_=new QPushButton("Copy image",this);copyImage_->setObjectName("copyHistoryImage");saveAs_=new QPushButton("Save as…",this);saveAs_->setObjectName("saveHistoryAs");actions->addWidget(open_,0,0);actions->addWidget(copyImage_,0,1);actions->addWidget(saveAs_,1,0);editAgain_=new QPushButton("Edit again",this);editAgain_->setObjectName("editHistoryImage");actions->addWidget(editAgain_,1,1);connect(editAgain_,&QPushButton::clicked,this,[this]{editAgain(currentDetail_);});retry_=new QPushButton("Retry upload",this);retry_->setObjectName("retryOperation");VisualStyle::role(retry_,"primary");cancel_=new QPushButton("Cancel upload",this);cancel_->setObjectName("cancelOperation");actions->addWidget(retry_,2,0);actions->addWidget(cancel_,2,1);saveDiagnostics_=new QPushButton("Save diagnostics for this upload",this);saveDiagnostics_->setObjectName("saveHistoryDiagnostics");saveDiagnostics_->setToolTip("Save a redacted diagnostics package (.zip) for this upload on this computer. Nothing is uploaded.");actions->addWidget(saveDiagnostics_,3,0,1,2);saveDiagnostics_->hide();connect(saveDiagnostics_,&QPushButton::clicked,this,[this]{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1)saveDiagnostics(model_->record(rows.first().row())["operationId"].toString());});inspect->addLayout(actions);
 share_=new QPushButton("Create share link",this);share_->setObjectName("shareHistoryLink");revoke_=new QPushButton("Revoke link…",this);revoke_->setObjectName("revokeHistoryLink");inspectLink_=new QPushButton("Inspect link",this);inspectLink_->setObjectName("inspectHistoryLink");auto* sharing=new QHBoxLayout;for(auto* b:{share_,revoke_,inspectLink_}){sharing->addWidget(b);b->hide();}inspect->addLayout(sharing);connect(share_,&QPushButton::clicked,this,[this]{changeSharing("create");});connect(revoke_,&QPushButton::clicked,this,[this]{changeSharing("revoke");});connect(inspectLink_,&QPushButton::clicked,this,[this]{changeSharing("inspect");});
 auto* remove=new QPushButton("Delete from destination…",this);remove->setObjectName("deleteHistoryRemote");VisualStyle::role(remove,"danger");inspect->addWidget(remove,0,Qt::AlignLeft);connect(remove,&QPushButton::clicked,this,&HistoryWindow::prepareDelete);
 summary_=new QLabel(this);summary_->setObjectName("historySummary");summary_->setAccessibleName("File size, pixel dimensions, destination and outcomes");summary_->setWordWrap(true);summary_->setTextFormat(Qt::RichText);summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);summary_->setSizePolicy(QSizePolicy::Ignored,QSizePolicy::Preferred);inspect->addWidget(summary_);
 auto* receiptToggle=new QToolButton(this);receiptToggle->setObjectName("historyReceiptExpanded");receiptToggle->setText("Advanced file details");receiptToggle->setCheckable(true);receiptToggle->setAccessibleName("Show advanced receipt details");inspect->addWidget(receiptToggle,0,Qt::AlignLeft);details_=new QPlainTextEdit(this);details_->setObjectName("historyDetails");details_->setReadOnly(true);details_->setAccessibleName("Advanced original receipt details");details_->setMaximumHeight(180);details_->hide();inspect->addWidget(details_);connect(receiptToggle,&QToolButton::toggled,details_,&QWidget::setVisible);inspect->addStretch();inspectorScroll_->setWidget(inspector);split_->addWidget(inspectorScroll_);layout->addWidget(split_,1);inspectorScroll_->hide();
 auto* footer=new QHBoxLayout;count_=new QLabel(this);VisualStyle::role(count_,"muted");footer->addWidget(count_);footer->addStretch();cancelled_=new QLabel(this);VisualStyle::role(cancelled_,"muted");footer->addWidget(cancelled_);showCancelled_=new QToolButton(this);showCancelled_->setObjectName("historyShowCancelled");showCancelled_->setText("Show");showCancelled_->setCheckable(true);showCancelled_->setAccessibleName("Show cancelled captures");footer->addWidget(showCancelled_);connect(showCancelled_,&QToolButton::toggled,this,[this](bool show){model_->setShowCancelled(show);showCancelled_->setText(show?"Hide":"Show");emit viewStateChanged();});auto* previous=new QPushButton("Previous",this);auto* next=new QPushButton("Next",this);footer->addWidget(previous);footer->addWidget(next);connect(previous,&QPushButton::clicked,this,[this]{auto q=model_->query();q.offset=qMax(0,q.offset-q.limit);model_->setQuery(q);});connect(next,&QPushButton::clicked,this,[this]{auto q=model_->query();if(q.offset+q.limit<model_->total()){q.offset+=q.limit;model_->setQuery(q);}});layout->addLayout(footer);
 auto* bottom=new QHBoxLayout;status_=new QLabel("Select a capture to inspect its files and share link.",this);status_->setTextFormat(Qt::PlainText);status_->setAccessibleName("History status");status_->setWordWrap(true);VisualStyle::role(status_,"status");bottom->addWidget(status_,1);auto* managementToggle=new QToolButton(this);managementToggle->setObjectName("historyManagementExpanded");managementToggle->setText("Manage");managementToggle->setCheckable(true);managementToggle->setAccessibleName("Show history management actions");bottom->addWidget(managementToggle);layout->addLayout(bottom);
 auto* management=new QWidget(this);auto* manage=new QHBoxLayout(management);manage->setContentsMargins(0,0,0,0);auto* deletionRetry=new QPushButton("Retry deletion",this);auto* deletionCancel=new QPushButton("Cancel deletion",this);auto* clear=new QPushButton("Clear history records…",this);auto* cache=new QPushButton("Clean retry artifacts and previews…",this);for(auto* b:{deletionRetry,deletionCancel,clear,cache})manage->addWidget(b);layout->addWidget(management);management->hide();connect(managementToggle,&QToolButton::toggled,management,&QWidget::setVisible);
 sharingTimer_=new QTimer(this);sharingTimer_->setInterval(500);connect(sharingTimer_,&QTimer::timeout,this,&HistoryWindow::pollSharing);deletionTimer_=new QTimer(this);deletionTimer_->setInterval(500);connect(deletionTimer_,&QTimer::timeout,this,&HistoryWindow::pollDeletion);
 connect(deletionRetry,&QPushButton::clicked,this,[this]{if(deletionId_.isEmpty())return;session_.request("delete.retry",{{"id",deletionId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Deletion retry accepted.",bool(e));if(!e)g->deletionTimer_->start();}});});connect(deletionCancel,&QPushButton::clicked,this,[this]{if(deletionId_.isEmpty())return;session_.request("delete.cancel",{{"id",deletionId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Cancellation requested; already sent deletion cannot be rolled back.",bool(e));});});
 connect(clear,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Clear history","Clear local history records? Remote objects and user-owned local copies remain.")!=QMessageBox::Yes)return;session_.request("history.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"History cleared.",bool(e));g->refresh();}});});connect(cache,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Clean artifacts","Clean eligible retry artifacts and image previews? Some cleaned artifacts cannot retry. Persistent history, remote objects and local copies remain.")!=QMessageBox::Yes)return;session_.request("cache.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject result){if(g){g->releasePreview();
  // An incomplete upload that could not be aborted keeps using storage (#146).
  const auto kept=result["incompleteUploads"].toArray();const auto uploads=kept.size()==1?QStringLiteral("an incomplete upload"):QStringLiteral("%1 incomplete uploads").arg(kept.size());
  const auto message=kept.isEmpty()?QStringLiteral("Artifact cleanup completed."):QStringLiteral("Artifact cleanup completed, but %1 could not be removed. %2").arg(uploads,kept[0].toObject()["error"].toObject()["message"].toString());
  VisualStyle::feedback(g->status_,e?e.message:message,bool(e)||!kept.isEmpty());g->refresh();}});});
 connect(copy_,&QPushButton::clicked,this,[this]{const auto record=selectedRecord();if(record.isEmpty())return;if(record["uploadStatus"]=="local_only"){operationAction("operation.copy");return;}session_.request("history.copyLink",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Stored link copied.",bool(e));});});connect(open_,&QPushButton::clicked,this,[this]{if(currentDetail_["linkAvailable"].toBool())QDesktopServices::openUrl(QUrl(link_->text()));});connect(retry_,&QPushButton::clicked,this,[this]{const auto file=selectedRecord();operationAction(file["uploadStatus"]=="local_only"?"operation.retryLocalSave":"operation.retry");});connect(cancel_,&QPushButton::clicked,this,[this]{operationAction("operation.cancel");});connect(copyImage_,&QPushButton::clicked,this,[this]{exportLocal(true);});connect(saveAs_,&QPushButton::clicked,this,[this]{exportLocal(false);});
 connect(table_->selectionModel(),&QItemSelectionModel::selectionChanged,this,[this]{if(!restoringSelection_){selectedItemId_.clear();selectionChanged();}});connect(model_,&QAbstractItemModel::modelAboutToBeReset,this,[this]{if(!restoringSelection_)restoreSelection_=selectedIds();restoringSelection_=true;});
 connect(model_,&HistoryModel::loaded,this,[this]{for(int row=0;row<model_->rowCount();++row){const auto r=model_->record(row);bool selected=restoreSelection_.contains("operation:"+r["operationId"].toString());for(auto v:r["records"].toArray())selected|=restoreSelection_.contains(v.toObject()["id"].toString());if(selected){table_->selectionModel()->select(model_->index(row),QItemSelectionModel::Select|QItemSelectionModel::Rows);if(!table_->currentIndex().isValid())table_->selectionModel()->setCurrentIndex(model_->index(row),QItemSelectionModel::NoUpdate);}}restoreSelection_.clear();restoringSelection_=false;list_->setCurrentWidget(model_->rowCount()?static_cast<QWidget*>(table_):static_cast<QWidget*>(empty_));count_->setText(QString("%1 captures · %2 stored files").arg(model_->rowCount()).arg(model_->total()));cancelled_->setText(QString("%1 cancelled captures %2").arg(model_->hiddenCancelled()).arg(model_->showCancelled()?"shown":"hidden"));showCancelled_->setVisible(model_->hiddenCancelled()>0||model_->showCancelled());cancelled_->setVisible(showCancelled_->isVisible());selectionChanged();updateActivity();if(awaitingInitialization_){awaitingInitialization_=false;VisualStyle::feedback(status_,"Select a capture to inspect its files and share link.");}emit viewStateChanged();});connect(model_,&HistoryModel::error,this,[this](const QString& message){VisualStyle::feedback(status_,message,true);});
 connect(&session_,&UiSession::operationsChanged,this,&HistoryWindow::refresh);connect(&session_,&UiSession::initialized,this,&HistoryWindow::refresh);for(auto* e:findChildren<QLineEdit*>())if(!e->isReadOnly())connect(e,&QLineEdit::textChanged,this,&HistoryWindow::viewStateChanged);for(auto* t:{advancedToggle,managementToggle,receiptToggle})connect(t,&QToolButton::toggled,this,&HistoryWindow::viewStateChanged);
 for(auto* b:findChildren<QPushButton*>())if(b->accessibleName().isEmpty())b->setAccessibleName(b->text());
 setGridMode(false);refresh();
}
void HistoryWindow::applyFilters(){searchTimer_->stop();auto q=model_->query();q.offset=0;q.search=findChild<QLineEdit*>("historySearch")->text();q.from=findChild<QLineEdit*>("historyFilterfrom")->text();q.to=findChild<QLineEdit*>("historyFilterto")->text();q.mime=findChild<QLineEdit*>("historyFiltermime")->text();q.profileId=findChild<QLineEdit*>("historyFilterprofileId")->text();q.uploadStatus=findChild<QLineEdit*>("historyFilteruploadStatus")->text();q.localStatus=findChild<QLineEdit*>("historyFilterlocalStatus")->text();q.remoteStatus=findChild<QLineEdit*>("historyFilterremoteStatus")->text();model_->setQuery(q);emit viewStateChanged();}
void HistoryWindow::setGridMode(bool grid){grid_->setChecked(grid);findChild<QToolButton*>("historyListMode")->setChecked(!grid);table_->setViewMode(grid?QListView::IconMode:QListView::ListMode);table_->setMovement(QListView::Static);table_->setWrapping(grid);table_->setGridSize(grid?QSize(254,282):QSize());table_->setUniformItemSizes(grid);table_->doItemsLayout();updateInspectorLayout();emit viewStateChanged();}
void HistoryWindow::updateInspectorLayout(){if(!split_)return;const bool stacked=width()<800;split_->setOrientation(stacked?Qt::Vertical:Qt::Horizontal);inspectorScroll_->setMinimumWidth(stacked?0:360);inspectorScroll_->setMaximumHeight(stacked?330:QWIDGETSIZE_MAX);split_->setStretchFactor(0,1);split_->setStretchFactor(1,1);inspectorScroll_->setVisible(table_->selectionModel()->selectedRows().size()==1&&!grid_->isChecked());}
void HistoryWindow::resizeEvent(QResizeEvent* event){RetainedWindow::resizeEvent(event);updateInspectorLayout();}
void HistoryWindow::refresh(){if(!session_.ready()){awaitingInitialization_=true;VisualStyle::feedback(status_,"Waiting for the resident journal to initialize.");return;}model_->refresh();}
QStringList HistoryWindow::selectedIds()const{QStringList ids;for(auto index:table_->selectionModel()->selectedRows()){auto r=model_->record(index.row());const auto records=r["records"].toArray();if(records.isEmpty())ids.append("operation:"+r["operationId"].toString());else for(auto v:records)ids.append(v.toObject()["id"].toString());}return ids;}
QJsonObject HistoryWindow::selectedRecord()const{const auto rows=table_->selectionModel()->selectedRows();if(rows.size()!=1)return {};const auto group=model_->record(rows.first().row());for(const auto& v:group["records"].toArray())if(v.toObject()["id"]==items_->currentData().toString())return v.toObject();return group["records"].toArray().isEmpty()?QJsonObject{}:group["records"].toArray().first().toObject();}
QString HistoryWindow::destinationLabel(const QJsonObject& r)const{
 if(r["uploadStatus"]=="local_only")return "Local copy";
 const auto original=r["receipt"].toObject()["destination"].toObject();const auto settings=original["settings"].toObject();const auto provider=original["providerId"].toString();QString label=provider=="proton-drive"?"Proton Drive":provider=="s3"?"S3 destination":"Original destination";
 for(auto v:session_.document()["profiles"].toArray()){const auto profile=v.toObject();if(profile["id"]==r["profileId"]){label=profile["label"].toString();break;}}
 if(provider=="proton-drive"&&!settings["folderName"].toString().isEmpty())label+=" / "+settings["folderName"].toString();else if(provider=="s3"&&!settings["objectPrefix"].toString().isEmpty())label+=" / "+settings["objectPrefix"].toString();return label;
}
void HistoryWindow::selectionChanged(){emit viewStateChanged();const auto rows=table_->selectionModel()->selectedRows();{QSignalBlocker block(items_);items_->clear();if(rows.size()==1){const auto group=model_->record(rows.first().row());for(auto v:group["records"].toArray()){auto r=v.toObject();items_->addItem(r["name"].toString(),r["id"].toString());}const auto itemIndex=items_->findData(selectedItemId_);if(itemIndex>=0)items_->setCurrentIndex(itemIndex);items_->setVisible(items_->count()>1);}}loadSelectedItem();updateInspectorLayout();}
void HistoryWindow::loadSelectedItem(){
 if(previewCancelled_)previewCancelled_->store(true);
 previewRequestTimer_->stop();pendingPreviewPath_.clear();pixelDimensions_.clear();const auto seq=++selectionRequest_;currentDetail_={};preview_->clear();details_->clear();summary_->clear();title_->clear();outcome_->clear();link_->clear();for(auto* b:{copy_,open_,copyImage_,saveAs_,editAgain_})b->setEnabled(false);for(auto* b:{share_,revoke_,inspectLink_,retry_,cancel_,saveDiagnostics_})b->hide();
 const auto rows=table_->selectionModel()->selectedRows();findChild<QPushButton*>("deleteHistoryRemote")->setEnabled(!rows.isEmpty());if(rows.size()!=1)return;const auto group=model_->record(rows.first().row());title_->setText(rows.first().data().toString());outcome_->setText(HistoryModel::statusText(group));VisualStyle::role(outcome_,model_->data(rows.first(),HistoryModel::FailedRole).toBool()?"danger":group["uploadStatus"]=="uploaded"?"success":"muted");retry_->setText(model_->data(rows.first(),HistoryModel::PrimaryActionRole).toString()=="Retry local save"?"Retry local save":"Retry upload");saveDiagnostics_->setVisible(diagnosticsAvailable(rows.first().row()));retry_->setVisible(model_->data(rows.first(),HistoryModel::FailedRole).toBool()&&!model_->data(rows.first(),HistoryModel::ActiveRole).toBool()&&!group["operationId"].toString().isEmpty()&&!HistoryModel::storedNothing(group));cancel_->setVisible(model_->data(rows.first(),HistoryModel::ActiveRole).toBool());
 const auto record=selectedRecord();if(record.isEmpty()){summary_->setText("Capture in progress. File details appear after acquisition.");return;}selectedItemId_=record["id"].toString();if(!deletionTimer_->isActive())deletionId_=record["deleteOperationId"].toString();
 session_.request("history.detail",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this),seq](Error e,QJsonObject data){if(!g||g->selectionRequest_!=seq)return;if(e){VisualStyle::feedback(g->status_,e.message,true);return;}g->currentDetail_=data;g->details_->setPlainText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));g->title_->setText(g->items_->count()>1?data["name"].toString():g->table_->currentIndex().data().toString());g->link_->setText(data["receipt"].toObject()["directUrl"].toString());auto local=data["local"].toObject();const auto localPath=local["path"].toString();const bool localAvailable=local["status"]=="saved"&&QFileInfo(localPath).isFile();g->saveAs_->setEnabled(localAvailable);g->copyImage_->setEnabled(localAvailable&&data["mime"].toString().startsWith("image/")&&data["bytes"].toInteger()<=32*1024*1024);g->saveAs_->setToolTip(localAvailable?"Save the retained local original to another file":"A retained local original is needed to save another copy");g->copyImage_->setToolTip(g->copyImage_->isEnabled()?"Copy the retained original image":"A retained local image is needed to copy image bytes");g->editAgain_->setEnabled(editable(data));g->editAgain_->setToolTip(g->editAgain_->isEnabled()?"Open the retained local image in the editor and upload the result as a new item":data["mime"]=="image/png"||data["mime"]=="image/jpeg"?"Edit again needs the local copy. Turn on Keep a local copy in Settings → General for future captures.":"Only PNG and JPEG images can be edited");
 g->updateSummary();
 const bool sharing=data["capabilities"].toObject()["sharing"].toBool();for(auto* b:{g->share_,g->revoke_,g->inspectLink_})b->setVisible(sharing);const auto linkState=data["shareStatus"].toString();const bool stopped=g->sharingStopped_&&g->sharingId_==data["shareOperationId"].toString();const bool busy=g->sharingAccepting_||g->sharingTimer_->isActive()||(linkState=="pending"&&!stopped);g->share_->setEnabled(!busy&&linkState!="pending"&&linkState!="unknown"&&linkState!="changed");g->revoke_->setEnabled(!busy&&data["linkAvailable"].toBool());g->inspectLink_->setEnabled(!busy);const bool localResult=data["uploadStatus"]=="local_only"&&local["status"]=="saved";g->copy_->setText(localResult?"Copy result":"Copy link");g->copy_->setAccessibleName(g->copy_->text());g->copy_->setEnabled(data["linkAvailable"].toBool()||localResult);g->open_->setEnabled(data["linkAvailable"].toBool());
 if(data["superseded"].toBool()||data["remoteStatus"]=="superseded")VisualStyle::feedback(g->status_,"Superseded: this stored link may serve replacement content. Deletion still targets the original receipt.",true);else if(!g->sharingFeedback_.isEmpty())VisualStyle::feedback(g->status_,g->sharingFeedback_,g->sharingFeedbackError_);g->preview_->setText("Preview unavailable");g->pendingPreviewPath_=data["previewPath"].toString();g->startPreview();if(g->previewMissing(data))g->previewRequestTimer_->start();});
}
void HistoryWindow::updateActivity(){activeId_.clear();for(int row=0;row<model_->rowCount();++row){auto i=model_->index(row);if(!i.data(HistoryModel::ActiveRole).toBool())continue;const auto r=model_->record(row);activeId_=r["operationId"].toString();activeLabel_->setText(r["name"].toString()+" · "+HistoryModel::statusText(r));const int percent=i.data(HistoryModel::ProgressRole).toInt();progress_->setRange(0,percent<0?0:100);progress_->setValue(qMax(0,percent));break;}activeBanner_->setVisible(!activeId_.isEmpty());}
void HistoryWindow::operationAction(const QString& method){const auto rows=table_->selectionModel()->selectedRows();if(rows.size()!=1)return;const auto id=model_->record(rows.first().row())["operationId"].toString();if(id.isEmpty())return;session_.request(method,{{"id",id}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Action accepted.",bool(e));g->refresh();}});}
void HistoryWindow::primaryAction(const QModelIndex& i){table_->selectionModel()->select(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(i);const auto action=i.data(HistoryModel::PrimaryActionRole).toString();if(action=="Cancel")operationAction("operation.cancel");else if(action=="Retry upload")operationAction("operation.retry");else if(action=="Retry local save")operationAction("operation.retryLocalSave");else if(action=="Copy result")operationAction("operation.copy");else if(action=="Copy link"){const auto group=model_->record(i.row());if(group["records"].toArray().size()>1&&!group["operation"].toObject().isEmpty())operationAction("operation.copy");else {const auto files=group["records"].toArray();if(files.isEmpty())return;session_.request("history.copyLink",{{"id",files.first().toObject()["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Stored link copied.",bool(e));});}}}
bool HistoryWindow::editable(const QJsonObject& record){
 const auto local=record["local"].toObject();
 // A batch row holds several files; its inspector picks one to edit.
 if(record["records"].toArray().size()>1)return false;
 return (record["mime"]=="image/png"||record["mime"]=="image/jpeg")&&local["status"]=="saved"&&QFileInfo(local["path"].toString()).isFile();
}
void HistoryWindow::editAgain(const QJsonObject& record){
 if(!editable(record)){VisualStyle::feedback(status_,"Edit again needs a retained local PNG or JPEG copy.",true);return;}
 emit editRequested("file",record["local"].toObject()["path"].toString());
}
void HistoryWindow::showRowMenu(const QModelIndex& i,const QPoint& point){table_->selectionModel()->select(i,QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(i);const auto rowRecord=model_->record(i.row());QMenu menu(this);menu.addAction("Inspect files",this,[this]{setGridMode(false);});const bool operation=!model_->record(i.row())["operationId"].toString().isEmpty()&&!HistoryModel::storedNothing(rowRecord);auto* result=menu.addAction("Copy result",this,[this]{operationAction("operation.copy");});result->setEnabled(operation);auto* retry=menu.addAction("Retry upload",this,[this]{operationAction("operation.retry");});retry->setEnabled(operation);auto* cancel=menu.addAction("Cancel operation",this,[this]{operationAction("operation.cancel");});cancel->setEnabled(i.data(HistoryModel::ActiveRole).toBool());auto* retryLocal=menu.addAction("Retry local save",this,[this]{operationAction("operation.retryLocalSave");});retryLocal->setEnabled(operation);auto* edit=menu.addAction("Edit again",this,[this,rowRecord]{editAgain(rowRecord);});edit->setEnabled(editable(rowRecord));auto* diagnostics=menu.addAction("Save diagnostics for this upload",this,[this,rowRecord]{saveDiagnostics(rowRecord["operationId"].toString());});diagnostics->setEnabled(diagnosticsAvailable(i.row()));menu.addAction("Copy details",this,[this,rowRecord]{QJsonObject detail=rowRecord;if(currentDetail_["operationId"]==rowRecord["operationId"])detail["selectedFile"]=currentDetail_;QApplication::clipboard()->setText(QString::fromUtf8(QJsonDocument(detail).toJson(QJsonDocument::Indented)));VisualStyle::feedback(status_,"Operation and receipt details copied.");});menu.addSeparator();auto* remove=menu.addAction("Remove from history");remove->setEnabled(false);remove->setToolTip("Removing one capture is not supported by the resident. Manage → Clear history records removes all local records.");menu.setToolTipsVisible(true);menu.exec(point);}
void HistoryWindow::updateSummary(){
 if(currentDetail_.isEmpty())return;
 const auto data=currentDetail_;auto escaped=[](const QString& s){return s.toHtmlEscaped();};const auto muted=RedesignTheme::textMuted().name();
 const QHash<QString,QString> uploads{{"uploaded","Uploaded"},{"local_only","Local capture"},{"pending","Waiting to upload"},{"uploading","Uploading"},{"failed","Upload failed"},{"unknown","Upload outcome unknown"},{"not_requested","No upload requested"}};
 const QHash<QString,QString> copies{{"saved","Saved locally"},{"disabled","No local copy requested"},{"failed","Local save failed"},{"pending","Waiting for local save"}};
 const QHash<QString,QString> sharing{{"shared","Shared link"},{"not_requested","No share link requested"},{"private","Private upload"},{"pending","Creating share link"},{"revoked","Share link revoked"},{"failed","Sharing failed"},{"unknown","Sharing outcome unknown"},{"changed","Share link changed"}};
 const QHash<QString,QString> remote{{"present","Remote file available"},{"not_requested","Remote deletion not requested"},{"removed","Deleted from destination"},{"already_missing","Remote file already missing"},{"unknown","Remote outcome unknown"},{"failed","Remote deletion failed"},{"pending","Waiting for remote deletion"},{"deleting","Deleting from destination"},{"changed","Remote file changed"},{"superseded","Replaced by a newer upload"},{"hidden_version_retained","Hidden; original version remains"}};
 const auto size=HistoryModel::fileSize(data["bytes"].toInteger())+(pixelDimensions_.isEmpty()?QString{}:" · "+pixelDimensions_);const auto local=data["local"].toObject();
 const auto html="<table width=\"100%\" cellspacing=\"0\" cellpadding=\"3\"><tr><td width=\"31%\"><font color=\""+muted+"\">Size</font></td><td width=\"35%\"><font color=\""+muted+"\">Destination</font></td><td width=\"34%\"><font color=\""+muted+"\">File</font></td></tr><tr><td valign=\"top\">"+escaped(size)+"</td><td valign=\"top\">"+escaped(destinationLabel(data))+"</td><td valign=\"top\">"+escaped(data["name"].toString())+"</td></tr></table><p><font color=\""+muted+"\">"+escaped(uploads.value(data["uploadStatus"].toString(),"Upload not recorded"))+" · "+escaped(copies.value(local["status"].toString(),"Local copy not recorded"))+"<br>"+escaped(sharing.value(data["shareStatus"].toString(),"Sharing not recorded"))+" · "+escaped(remote.value(data["remoteStatus"].toString(),"Remote outcome not recorded"))+"<br>"+escaped(data["mime"].toString())+"</font></p>";
 summary_->setText(html);
}
void HistoryWindow::startPreview(){
 if(pendingPreviewPath_.isEmpty()||!isVisible()||previewLoading_)return;
 const auto path=std::exchange(pendingPreviewPath_,{});const auto original=currentDetail_["local"].toObject()["status"]=="saved"?currentDetail_["local"].toObject()["path"].toString():QString{};const auto seq=selectionRequest_;previewLoading_=true;auto image=std::make_shared<QImage>();previewCancelled_=std::make_shared<std::atomic_bool>(false);auto cancelled=previewCancelled_;
 session_.background([path,original,image,cancelled](Error&){
  if(cancelled->load())return QJsonObject{};
  QFileInfo f(path);if(!f.isFile()||f.size()>8*1024*1024)return QJsonObject{};QImageReader reader(path);auto s=reader.size();if(!s.isValid()||qint64(s.width())*s.height()>16000000)return QJsonObject{};reader.setScaledSize(s.scaled(900,540,Qt::KeepAspectRatio));const auto length=reader.text(HistoryModel::durationKey);*image=reader.read();if(!length.isEmpty())image->setText(HistoryModel::durationKey,length);
  QJsonObject metadata;if(!original.isEmpty()&&QFileInfo(original).isFile()&&QFileInfo(original).size()<=32*1024*1024){QImageReader originalReader(original);const auto size=originalReader.size();if(size.isValid()){metadata["width"]=size.width();metadata["height"]=size.height();}}return metadata;
 },this,[g=QPointer<HistoryWindow>(this),seq,image](Error,QJsonObject metadata){if(!g)return;g->previewLoading_=false;if(g->isVisible()&&seq==g->selectionRequest_){if(!image->isNull()){auto pixmap=QPixmap::fromImage(*image).scaled(g->preview_->contentsRect().size(),Qt::KeepAspectRatio,Qt::SmoothTransformation);const auto length=HistoryModel::duration(image->text(HistoryModel::durationKey).toLongLong());if(video(g->currentDetail_)){QPainter painter(&pixmap);paintVideoBadge(painter,QRect(QPoint(0,0),pixmap.deviceIndependentSize().toSize()),length);if(!length.isEmpty()){g->pixelDimensions_=length;g->updateSummary();}}g->preview_->setPixmap(pixmap);}if(metadata["width"].toInt()>0){g->pixelDimensions_=QString("%1×%2").arg(metadata["width"].toInt()).arg(metadata["height"].toInt());g->updateSummary();}}g->startPreview();});
}
bool HistoryWindow::previewMissing(const QJsonObject& detail)const{
 const auto category=detail["category"].toString(),id=detail["id"].toString(),path=detail["previewPath"].toString();
 return !id.isEmpty()&&(category=="images"||category=="videos")&&!previewRefused_.contains(id)&&(path.isEmpty()||!QFileInfo(path).isFile());
}
void HistoryWindow::requestPreview(){
 const auto id=currentDetail_["id"].toString();if(!isVisible()||!previewRequestId_.isEmpty()||!previewMissing(currentDetail_))return;
 previewRequestId_=id;preview_->setText("Making a preview…");
 session_.request("history.preview",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject result){
  if(!g)return;g->previewRequestId_.clear();const bool current=g->currentDetail_["id"]==id;
  if(e){
   // Fail-soft: no status message. A refused item is not asked for again in
   // this window; a busy queue or a running upload may change.
   if(!QStringList{"preview_busy","operation_active","client_timeout","initializing"}.contains(e.code))g->previewRefused_.insert(id);
   if(current&&g->preview_->pixmap().isNull())g->preview_->setText("Preview unavailable");
  }else{
   const auto path=result["previewPath"].toString();g->model_->reloadThumbnail(path);
   if(current){g->pendingPreviewPath_=path;g->startPreview();}
  }
  // The selection moved on while this one was being made.
  if(!current&&g->previewMissing(g->currentDetail_))g->previewRequestTimer_->start();
 });
}
void HistoryWindow::releasePreview(){previewRequestTimer_->stop();if(previewCancelled_)previewCancelled_->store(true);pendingPreviewPath_.clear();++selectionRequest_;preview_->clear();model_->releaseThumbnails();}
void HistoryWindow::hideEvent(QHideEvent* event){releasePreview();RetainedWindow::hideEvent(event);}
void HistoryWindow::showEvent(QShowEvent* event){RetainedWindow::showEvent(event);selectionChanged();table_->viewport()->update();}
void HistoryWindow::exportLocal(bool image){
 if(image){const auto record=selectedRecord();if(record.isEmpty())return;session_.request("history.copyImage",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Original image copied.",bool(e));});return;}

 const auto local=currentDetail_["local"].toObject();const auto path=local["path"].toString();if(local["status"]!="saved"||!QFileInfo(path).isFile())return;
 const auto destination=QFileDialog::getSaveFileName(this,"Save original file as",currentDetail_["name"].toString());if(destination.isEmpty())return;if(QFileInfo(destination).absoluteFilePath()==QFileInfo(path).absoluteFilePath()){VisualStyle::feedback(status_,"This is already the retained local file.");return;}
  session_.background([path,destination](Error& e){QFile source(path);QSaveFile target(destination);if(!source.open(QIODevice::ReadOnly)||!target.open(QIODevice::WriteOnly)){e={"file_copy","Could not open the original or selected destination file.",{}};return QJsonObject{};}while(!source.atEnd()){const auto bytes=source.read(1024*1024);if(bytes.isEmpty()&&source.error()!=QFileDevice::NoError){e={"file_copy","Could not read the retained original file.",{}};return QJsonObject{};}if(target.write(bytes)!=bytes.size()){e={"file_copy","Could not write the selected destination file.",{}};return QJsonObject{};}}if(!target.commit())e={"file_copy","Could not finish saving the original file.",{}};return QJsonObject{};},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Original file saved.",bool(e));});
}
void HistoryWindow::changeSharing(const QString& action){
 const auto file=selectedRecord();if(file.isEmpty()||sharingAccepting_||sharingTimer_->isActive())return;
 const auto historyId=file["id"].toString();
 if(action=="revoke"&&QMessageBox::question(this,"Revoke recorded share link", "Revoke this file’s public share link?\nThe uploaded file remains. Existing member access and downloaded copies remain.",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
 sharingAccepting_=true;emit viewStateChanged();for(auto* button:{share_,revoke_,inspectLink_})button->setEnabled(false);
 const auto method=action=="create"?"history.share":action=="revoke"?"history.revokeLink":"history.inspectLink";
 session_.request(method,{{"id",historyId}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject data){
  if(!g)return;
  g->sharingAccepting_=false;emit g->viewStateChanged();
  if(e){VisualStyle::feedback(g->status_,e.code+": "+e.message,true);g->selectionChanged();return;}
  g->sharingId_=data["shareOperationId"].toString();g->sharingStopped_=false;g->sharingFeedback_.clear();emit g->viewStateChanged();g->sharingTimer_->start();g->pollSharing();
 });
}
void HistoryWindow::pollSharing(){
 if(sharingPollActive_)return;
 if(sharingId_.isEmpty()){sharingTimer_->stop();return;}sharingPollActive_=true;const auto id=sharingId_;
 session_.request("share.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){
  if(!g)return;
  g->sharingPollActive_=false;if(id!=g->sharingId_)return;
  if(e){g->sharingFeedback_=e.code+": "+e.message;g->sharingFeedbackError_=true;VisualStyle::feedback(g->status_,g->sharingFeedback_,true);g->sharingTimer_->stop();g->refresh();return;}
  const auto state=data["state"].toString();g->sharingStopped_=state=="interrupted";if(state=="running"||state=="accepted")return;
  g->sharingTimer_->stop();const auto error=data["error"].toObject();g->sharingFeedback_="Link: "+data["status"].toString()+(error["message"].toString().isEmpty()?QString{}:" — "+error["message"].toString());g->sharingFeedbackError_=!error["code"].toString().isEmpty();VisualStyle::feedback(g->status_,g->sharingFeedback_,g->sharingFeedbackError_);g->refresh();
 });
}
void HistoryWindow::prepareDelete(){auto ids=selectedIds();ids.removeIf([](const QString& id){return id.startsWith("operation:");});if(table_->selectionModel()->selectedRows().size()==1&&!selectedRecord().isEmpty())ids={selectedRecord()["id"].toString()};if(ids.isEmpty())return;session_.request("delete.prepare",{{"ids",QJsonArray::fromStringList(ids)}},this,[g=QPointer<HistoryWindow>(this),ids](Error e,QJsonObject preview){if(!g)return;if(e){VisualStyle::feedback(g->status_,e.code+": "+e.message,true);return;}QString text="Delete these original recorded remote targets?\nLocal copies and history records remain. Browser/CDN caches may still serve bytes.\n";for(auto v:preview["targets"].toArray()){auto target=v.toObject();auto receipt=target["receipt"].toObject();text+="\nHistory ID: "+target["historyId"].toString()+"\nOriginal destination / exact target: "+QString::fromUtf8(QJsonDocument(receipt).toJson(QJsonDocument::Compact))+"\nEffect: "+target["effect"].toString()+"\n";}if(QMessageBox::question(g,"Confirm exact remote deletion",text,QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;g->deleteAccepting_=true;emit g->viewStateChanged();g->session_.request("delete.confirm",{{"ids",QJsonArray::fromStringList(ids)},{"acknowledgement",preview["acknowledgement"]}},g,[g](Error failure,QJsonObject data){if(!g)return;g->deleteAccepting_=false;emit g->viewStateChanged();VisualStyle::feedback(g->status_,failure?failure.code+": "+failure.message:"Remote deletion accepted; individual outcomes will update.",bool(failure));if(!failure){g->deletionId_=data["deleteOperationId"].toString();emit g->viewStateChanged();g->deletionTimer_->start();}});});}
void HistoryWindow::pollDeletion(){if(deletionPollActive_)return;if(deletionId_.isEmpty()){deletionTimer_->stop();return;}deletionPollActive_=true;const auto id=deletionId_;session_.request("delete.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){if(!g)return;g->deletionPollActive_=false;if(g->deletionId_!=id)return;if(e){VisualStyle::feedback(g->status_,e.message,true);g->deletionTimer_->stop();return;}QString status="Remote deletion: "+data["state"].toString();for(auto v:data["items"].toArray()){auto item=v.toObject();status+="\n"+item["status"].toString()+" "+item["error"].toObject()["message"].toString();}g->status_->setText(status);if(data["state"]!="running"&&data["state"]!="accepted"){g->deletionTimer_->stop();g->refresh();}});}
QJsonObject HistoryWindow::saveViewState()const{
 if(deleteAccepting_||sharingAccepting_)return {{"stateError","request_awaiting_acceptance"}};
 const auto& q=model_->query();QJsonObject query{{"limit",q.limit},{"offset",q.offset},{"search",q.search},{"category",q.category},{"mime",q.mime},{"profileId",q.profileId},{"uploadStatus",q.uploadStatus},{"localStatus",q.localStatus},{"remoteStatus",q.remoteStatus},{"from",q.from},{"to",q.to}};QJsonObject filters{{"search",findChild<QLineEdit*>("historySearch")->text()}};for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})filters[key]=findChild<QLineEdit*>("historyFilter"+key)->text();
 return {{"query",query},{"filters",filters},{"selectedIds",QJsonArray::fromStringList(restoringSelection_?restoreSelection_:selectedIds())},{"selectedItemId",selectedItemId_},{"deletionId",deletionId_},{"sharingId",sharingId_},{"filtersExpanded",findChild<QToolButton*>("historyFiltersExpanded")->isChecked()},{"managementExpanded",findChild<QToolButton*>("historyManagementExpanded")->isChecked()},{"receiptExpanded",findChild<QToolButton*>("historyReceiptExpanded")->isChecked()},{"gridMode",grid_->isChecked()},{"showCancelled",showCancelled_->isChecked()},{"geometry",QJsonArray{x(),y(),width(),height()}}};
}
bool HistoryWindow::restoreViewState(const QJsonObject& state){
 if(validateUiState({{"schemaVersion",1},{"history",state}}))return false;
 const QSignalBlocker stateSignals(this);searchTimer_->stop();const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());const auto f=state["filters"].toObject();findChild<QLineEdit*>("historySearch")->setText(f["search"].toString());for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})findChild<QLineEdit*>("historyFilter"+key)->setText(f[key].toString());findChild<QToolButton*>("historyFiltersExpanded")->setChecked(state["filtersExpanded"].toBool());findChild<QToolButton*>("historyManagementExpanded")->setChecked(state["managementExpanded"].toBool());findChild<QToolButton*>("historyReceiptExpanded")->setChecked(state["receiptExpanded"].toBool());setGridMode(state["gridMode"].toBool());showCancelled_->setChecked(state["showCancelled"].toBool());restoreSelection_.clear();for(auto id:state["selectedIds"].toArray())restoreSelection_.append(id.toString());selectedItemId_=state["selectedItemId"].toString();restoringSelection_=true;deletionId_=state["deletionId"].toString();if(!deletionId_.isEmpty()){deletionTimer_->start();pollDeletion();}sharingId_=state["sharingId"].toString();sharingStopped_=false;sharingFeedback_.clear();sharingTimer_->stop();if(!sharingId_.isEmpty()){sharingTimer_->start();pollSharing();}
 const auto data=state["query"].toObject();UiHistoryQuery q;q.limit=data["limit"].toInt(50);q.offset=data["offset"].toInt();q.search=data["search"].toString();q.category=data["category"].toString();q.mime=data["mime"].toString();q.profileId=data["profileId"].toString();q.uploadStatus=data["uploadStatus"].toString();q.localStatus=data["localStatus"].toString();q.remoteStatus=data["remoteStatus"].toString();q.from=data["from"].toString();q.to=data["to"].toString();model_->setQuery(q);return true;
}
// A failed (or partial or interrupted) upload with an operation gets a package for just
// that operation; the resident shows "diagnostics saved" with Show in folder (#122).
bool HistoryWindow::diagnosticsAvailable(int row)const{
 const auto index=model_->index(row);return index.isValid()&&index.data(HistoryModel::FailedRole).toBool()&&!index.data(HistoryModel::ActiveRole).toBool()&&!model_->record(row)["operationId"].toString().isEmpty();
}
void HistoryWindow::saveDiagnostics(const QString& operationId,std::function<void(Error,QJsonObject)> done){
 if(operationId.isEmpty()){if(done)done({"invalid_request","This row has no operation to package.",{}},{});return;}
 QJsonObject params{{"operation",operationId},{"notify",true}};
 const auto folder=session_.document()["preferences"].toObject()["diagnostics"].toObject()["packageFolder"].toString();if(QFileInfo(folder).isDir())params["output"]=folder;
 saveDiagnostics_->setEnabled(false);VisualStyle::feedback(status_,"Saving diagnostics for this upload…");
 session_.request("diagnostics.bundle",params,this,[g=QPointer<HistoryWindow>(this),done](Error e,QJsonObject result){
  if(g){g->saveDiagnostics_->setEnabled(true);VisualStyle::feedback(g->status_,e?e.message:"Diagnostics saved to "+QDir::toNativeSeparators(result["path"].toString())+". Nothing was uploaded.",bool(e));}
  if(done)done(e,result);
 });
}
void HistoryWindow::saveDiagnosticsFor(const QString& id,std::function<void(Error,QJsonObject)> reply){
 for(int row=0;row<model_->rowCount();++row){
  const auto group=model_->record(row);bool match=group["operationId"]==id;
  for(const auto& v:group["records"].toArray())match|=v.toObject()["id"]==id;
  if(!match)continue;
  table_->selectionModel()->select(model_->index(row),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);table_->setCurrentIndex(model_->index(row));
  if(!diagnosticsAvailable(row)){reply({"diagnostics_row_not_failed","Save diagnostics is offered for failed, partial and interrupted uploads only.",{{"operationId",group["operationId"]}}},{});return;}
  saveDiagnostics(group["operationId"].toString(),std::move(reply));return;
 }
 reply({"history_row_not_found","That upload is not on the History page shown; search or page to it first.",{{"id",id}}},{});
}
}
