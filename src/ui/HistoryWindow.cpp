#include "HistoryWindow.h"
#include "ViewState.h"
#include "VisualStyle.h"
#include "ByteSize.h"
#include <QApplication>
#include <QSplitter>
#include <QStackedWidget>
#include <QResizeEvent>
#include <QScrollArea>
#include <QToolButton>
#include <QTableView>
#include <QHeaderView>
#include <QLineEdit>
#include <QLabel>
#include <QPushButton>
#include <QPlainTextEdit>
#include <QPointer>
#include <QMessageBox>
#include <QDesktopServices>
#include <QUrl>
#include <QVBoxLayout>
#include <QGridLayout>
#include <QJsonDocument>
#include <QImageReader>
#include <QFileInfo>
#include <QTimer>
#include <QHideEvent>
#include <QShowEvent>
#include <QSignalBlocker>
#include <utility>
namespace putgd {
HistoryWindow::HistoryWindow(UiSession& session,QWidget* parent):RetainedWindow(parent),session_(session){setObjectName("putgdWindow");VisualStyle::apply(*qApp);setWindowIcon(VisualStyle::icon("brand"));setWindowTitle("put.gd — History");resize(1000,700);auto* layout=new QVBoxLayout(this);layout->setContentsMargins(20,18,20,18);layout->setSpacing(12);
 layout->addWidget(VisualStyle::header("History","Stored links and file outcomes. Available offline.",this));
 auto* filters=new QGridLayout;filters->setHorizontalSpacing(10);filters->setVerticalSpacing(10);layout->addLayout(filters);
 auto* search=new QLineEdit(this);search->setObjectName("historySearch");search->setAccessibleName("Search history by stored name");search->setMaxLength(256);search->setPlaceholderText("Search files by name…");filters->addWidget(search,0,0,1,3);auto* filter=new QPushButton("Search",this);filters->addWidget(filter,0,3);
 auto* advancedToggle=new QToolButton(this);advancedToggle->setObjectName("historyFiltersExpanded");advancedToggle->setText("Filters");advancedToggle->setCheckable(true);advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);advancedToggle->setArrowType(Qt::RightArrow);filters->addWidget(advancedToggle,0,4);
 auto* advanced=new QWidget(this);auto* advancedLayout=new QGridLayout(advanced);advancedLayout->setContentsMargins(0,0,0,0);advancedLayout->setSpacing(10);layout->addWidget(advanced);advanced->hide();connect(advancedToggle,&QToolButton::toggled,this,[advanced,advancedToggle](bool visible){advanced->setVisible(visible);advancedToggle->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});
 QHash<QString,QLineEdit*> fields;const QHash<QString,QString> filterLabels{{"from","From date (ISO)"},{"to","To date (ISO)"},{"mime","Content type"},{"profileId","Destination ID"},{"uploadStatus","Upload state"},{"localStatus","Local-copy state"},{"remoteStatus","Remote-deletion state"}};int filterIndex=0;for(auto key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"}){auto* e=new QLineEdit(this);e->setObjectName("historyFilter"+key);e->setAccessibleName(filterLabels.value(key));e->setPlaceholderText(filterLabels.value(key));advancedLayout->addWidget(e,filterIndex/4,filterIndex%4);++filterIndex;fields[key]=e;}
 empty_=new QLabel("No history entries.\nUploads and local captures appear here.",this);empty_->setWordWrap(true);empty_->setAlignment(Qt::AlignCenter);empty_->setObjectName("historyEmpty");VisualStyle::role(empty_,"muted");
 model_=new HistoryModel(session_,this);table_=new QTableView(this);table_->setObjectName("historyTable");table_->setAccessibleName("Stored upload and local capture history");table_->setModel(model_);table_->setSelectionBehavior(QAbstractItemView::SelectRows);table_->setSelectionMode(QAbstractItemView::ExtendedSelection);VisualStyle::decorateView(table_,{4,5,6});table_->setShowGrid(false);table_->verticalHeader()->hide();table_->verticalHeader()->setDefaultSectionSize(38);table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);table_->horizontalHeader()->setStretchLastSection(false);table_->setColumnHidden(2,true);table_->setColumnHidden(3,true);
 table_->horizontalHeader()->setSectionResizeMode(0,QHeaderView::Stretch);table_->setColumnWidth(1,170);
 for(int column:{4,5,6})table_->setColumnWidth(column,qMax(table_->fontMetrics().horizontalAdvance(model_->headerData(column,Qt::Horizontal,Qt::DisplayRole).toString())+28,table_->fontMetrics().horizontalAdvance("Version retained")+38));
 list_=new QStackedWidget(this);list_->addWidget(table_);list_->addWidget(empty_);
 split_=new QSplitter(Qt::Horizontal,this);split_->setObjectName("historySplit");split_->setChildrenCollapsible(false);split_->addWidget(list_);
 auto* inspector=VisualStyle::card("File details","",this);inspector->setMinimumWidth(230);auto* inspectorLayout=qobject_cast<QVBoxLayout*>(inspector->layout());
 preview_=new QLabel(this);preview_->setObjectName("historyPreview");preview_->setWordWrap(true);preview_->setAlignment(Qt::AlignCenter);preview_->setMaximumHeight(180);VisualStyle::role(preview_,"muted");inspectorLayout->addWidget(preview_);
 details_=new QPlainTextEdit(this);details_->setObjectName("historyDetails");details_->setAccessibleName("Original receipt details JSON");details_->setReadOnly(true);details_->setMinimumHeight(90);details_->hide();inspectorLayout->addWidget(details_,1);
 summary_=new QPlainTextEdit(this);summary_->setObjectName("historySummary");summary_->setAccessibleName("Readable original receipt and independent outcomes");summary_->setReadOnly(true);summary_->setMinimumHeight(90);inspectorLayout->addWidget(summary_,1);
 auto* receiptToggle=new QToolButton(this);receiptToggle->setObjectName("historyReceiptExpanded");receiptToggle->setText("Exact receipt");receiptToggle->setCheckable(true);receiptToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);receiptToggle->setArrowType(Qt::RightArrow);inspectorLayout->addWidget(receiptToggle);connect(receiptToggle,&QToolButton::toggled,this,[this,receiptToggle](bool visible){details_->setVisible(visible);summary_->setVisible(!visible);receiptToggle->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});inspectorScroll_=new QScrollArea(this);inspectorScroll_->setObjectName("historyInspectorScroll");inspectorScroll_->setAccessibleName("Original receipt inspector");inspectorScroll_->setWidgetResizable(true);inspectorScroll_->setFrameShape(QFrame::NoFrame);inspectorScroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);inspectorScroll_->setWidget(inspector);split_->addWidget(inspectorScroll_);inspectorScroll_->hide();layout->addWidget(split_,1);updateInspectorLayout();
 auto* paging=new QHBoxLayout;paging->setSpacing(8);layout->addLayout(paging);count_=new QLabel("Loading local history…",this);VisualStyle::role(count_,"muted");count_->setWordWrap(true);paging->addWidget(count_,1);auto* previous=new QPushButton("Previous",this);auto* next=new QPushButton("Next",this);paging->addWidget(previous);paging->addWidget(next);auto* refreshButton=new QPushButton("Refresh",this);paging->addWidget(refreshButton);
 auto apply=[this,search,fields]{UiHistoryQuery q;q.search=search->text();q.from=fields["from"]->text();q.to=fields["to"]->text();q.mime=fields["mime"]->text();q.profileId=fields["profileId"]->text();q.uploadStatus=fields["uploadStatus"]->text();q.localStatus=fields["localStatus"]->text();q.remoteStatus=fields["remoteStatus"]->text();model_->setQuery(q);};connect(filter,&QPushButton::clicked,this,apply);connect(search,&QLineEdit::returnPressed,this,apply);connect(previous,&QPushButton::clicked,this,[this]{auto q=model_->query();q.offset=qMax(0,q.offset-q.limit);model_->setQuery(q);});connect(next,&QPushButton::clicked,this,[this]{auto q=model_->query();if(q.offset+q.limit<model_->total()){q.offset+=q.limit;model_->setQuery(q);}});connect(refreshButton,&QPushButton::clicked,this,&HistoryWindow::refresh);
 status_=new QLabel("Select an entry to inspect or copy its stored link.",this);status_->setAccessibleName("History status");status_->setWordWrap(true);VisualStyle::role(status_,"status");layout->addWidget(status_);
 auto* buttons=new QHBoxLayout;buttons->setSpacing(10);layout->addLayout(buttons);
 copy_=new QPushButton("Copy link",this);copy_->setObjectName("copyHistoryLink");VisualStyle::role(copy_,"primary");open_=new QPushButton("Open link",this);open_->setObjectName("openHistoryLink");copy_->setEnabled(false);open_->setEnabled(false);buttons->addWidget(copy_);buttons->addWidget(open_);
 share_=new QPushButton("Create share link",this);share_->setObjectName("shareHistoryLink");revoke_=new QPushButton("Revoke link…",this);revoke_->setObjectName("revokeHistoryLink");inspectLink_=new QPushButton("Inspect link",this);inspectLink_->setObjectName("inspectHistoryLink");
 for(auto* button:{share_,revoke_,inspectLink_}){button->hide();buttons->addWidget(button);}
 connect(share_,&QPushButton::clicked,this,[this]{changeSharing("create");});connect(revoke_,&QPushButton::clicked,this,[this]{changeSharing("revoke");});connect(inspectLink_,&QPushButton::clicked,this,[this]{changeSharing("inspect");});
 sharingTimer_=new QTimer(this);sharingTimer_->setInterval(500);connect(sharingTimer_,&QTimer::timeout,this,&HistoryWindow::pollSharing);
 buttons->addStretch();auto* remove=new QPushButton("Delete selected uploads…",this);remove->setToolTip("Delete selected remote uploads; local copies and history remain");remove->setObjectName("deleteHistoryRemote");VisualStyle::role(remove,"danger");buttons->addWidget(remove);
 auto* managementToggle=new QToolButton(this);managementToggle->setObjectName("historyManagementExpanded");managementToggle->setText("Manage");managementToggle->setCheckable(true);managementToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);managementToggle->setArrowType(Qt::RightArrow);buttons->addWidget(managementToggle);
 auto* management=new QWidget(this);auto* managementLayout=new QGridLayout(management);managementLayout->setContentsMargins(0,0,0,0);managementLayout->setSpacing(8);layout->addWidget(management);management->hide();connect(managementToggle,&QToolButton::toggled,this,[management,managementToggle](bool visible){management->setVisible(visible);managementToggle->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});
 auto* retry=new QPushButton("Retry deletion",this);managementLayout->addWidget(retry,0,0);auto* cancel=new QPushButton("Cancel deletion",this);managementLayout->addWidget(cancel,0,1);auto* clear=new QPushButton("Clear history records…",this);managementLayout->addWidget(clear,1,0);auto* cache=new QPushButton("Clean retry artifacts and previews…",this);managementLayout->addWidget(cache,1,1);
 connect(table_->selectionModel(),&QItemSelectionModel::selectionChanged,this,[this]{if(!restoringSelection_)selectionChanged();});
 connect(model_,&QAbstractItemModel::modelAboutToBeReset,this,[this]{if(!restoringSelection_)restoreSelection_=selectedIds();restoringSelection_=true;});
connect(model_,&HistoryModel::loaded,this,[this]{
 for(int row=0;row<model_->rowCount();++row)if(restoreSelection_.contains(model_->record(row)["id"].toString()))table_->selectionModel()->select(model_->index(row,0),QItemSelectionModel::Select|QItemSelectionModel::Rows);
 restoreSelection_.clear();restoringSelection_=false;emit viewStateChanged();if(awaitingInitialization_){awaitingInitialization_=false;VisualStyle::feedback(status_,"Select an entry to inspect or copy its stored link.");}list_->setCurrentWidget(model_->total()==0?static_cast<QWidget*>(empty_):static_cast<QWidget*>(table_));count_->setText(QString("%1–%2 of %3 entries").arg(model_->total()==0?0:model_->query().offset+1).arg(model_->query().offset+model_->rowCount()).arg(model_->total()));selectionChanged();});connect(model_,&HistoryModel::error,this,[this](const QString& message){VisualStyle::feedback(status_,message,true);});
 connect(copy_,&QPushButton::clicked,this,[this]{auto ids=selectedIds();if(ids.size()!=1)return;session_.request("history.copyLink",{{"id",ids.first()}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.code+": "+e.message:"Stored link copied.",bool(e));});});connect(open_,&QPushButton::clicked,this,[this]{auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1&&model_->linkAvailable(rows.first().row()))QDesktopServices::openUrl(QUrl(model_->record(rows.first().row())["receipt"].toObject()["directUrl"].toString()));});connect(remove,&QPushButton::clicked,this,&HistoryWindow::prepareDelete);
 connect(retry,&QPushButton::clicked,this,[this]{if(deletionId_.isEmpty())return;session_.request("delete.retry",{{"id",deletionId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Deletion retry accepted.",bool(e));if(!e)g->deletionTimer_->start();}});});connect(cancel,&QPushButton::clicked,this,[this]{if(deletionId_.isEmpty())return;session_.request("delete.cancel",{{"id",deletionId_}},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Cancellation requested; already sent deletion cannot be rolled back.",bool(e));});});
 connect(clear,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Clear history","Clear local history records? No remote objects or user-owned local copies will be removed.")!=QMessageBox::Yes)return;session_.request("history.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"History cleared; remote objects unchanged.",bool(e));g->refresh();}});});connect(cache,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Clean artifacts","Clean eligible retry artifacts and image previews? Persistent history, remote objects, and local copies remain. Some cleaned artifacts cannot retry.")!=QMessageBox::Yes)return;session_.request("cache.clear",{},this,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Artifact cleanup completed.",bool(e));});});deletionTimer_=new QTimer(this);deletionTimer_->setInterval(500);connect(deletionTimer_,&QTimer::timeout,this,&HistoryWindow::pollDeletion);connect(&session_,&UiSession::operationsChanged,this,[this]{refresh();});connect(&session_,&UiSession::initialized,this,[this]{refresh();});
 for(auto* edit:findChildren<QLineEdit*>())connect(edit,&QLineEdit::textChanged,this,&HistoryWindow::viewStateChanged);
 for(auto* toggle:{advancedToggle,managementToggle,receiptToggle})connect(toggle,&QToolButton::toggled,this,&HistoryWindow::viewStateChanged);
 refresh();}
void HistoryWindow::refresh(){if(!session_.ready()){awaitingInitialization_=true;VisualStyle::feedback(status_,"History is waiting for the resident journal to initialize.");return;}model_->refresh();}
QStringList HistoryWindow::selectedIds()const{QStringList ids;for(auto row:table_->selectionModel()->selectedRows())ids.append(model_->record(row.row())["id"].toString());return ids;}
void HistoryWindow::selectionChanged(){for(auto* button:{share_,revoke_,inspectLink_})button->hide();emit viewStateChanged();if(previewCancelled_)previewCancelled_->store(true);pendingPreviewPath_.clear();auto rows=table_->selectionModel()->selectedRows();bool available=rows.size()==1&&model_->linkAvailable(rows.first().row());copy_->setEnabled(available);open_->setEnabled(available);details_->clear();summary_->clear();preview_->clear();preview_->setMinimumHeight(36);inspectorScroll_->setVisible(rows.size()==1);const auto seq=++selectionRequest_;if(rows.size()!=1)return;updateInspectorLayout();auto record=model_->record(rows.first().row());if(!deletionTimer_||!deletionTimer_->isActive())deletionId_=record["deleteOperationId"].toString();session_.request("history.detail",{{"id",record["id"]}},this,[g=QPointer<HistoryWindow>(this),seq](Error e,QJsonObject data){if(!g||g->selectionRequest_!=seq)return;if(e){VisualStyle::feedback(g->status_,e.message,true);return;}g->details_->setPlainText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));auto local=data["local"].toObject();auto receipt=data["receipt"].toObject();auto state=[](QString value){value.replace('_',' ');return value.isEmpty()?QString("Not recorded"):value;};g->summary_->setPlainText(data["name"].toString()+"\n"+data["mime"].toString()+"\n"+ByteSize::summary(data["bytes"].toInteger())+"\n\nOriginal destination\n"+data["profileId"].toString()+"\n\nStored link\n"+receipt["directUrl"].toString()+"\n\nUpload: "+state(data["uploadStatus"].toString())+"\nLocal copy: "+state(local["status"].toString())+"\nSharing: "+state(data["shareStatus"].toString())+"\nRemote: "+state(data["remoteStatus"].toString())+(local["path"].toString().isEmpty()?QString{}:"\n\nLocal file\n"+local["path"].toString()));const bool sharing=data["capabilities"].toObject()["sharing"].toBool();const auto linkState=data["shareStatus"].toString();
 for(auto* button:{g->share_,g->revoke_,g->inspectLink_})button->setVisible(sharing);
 const bool stopped=g->sharingStopped_&&g->sharingId_==data["shareOperationId"].toString();
 const bool busy=g->sharingAccepting_||g->sharingTimer_->isActive()||(linkState=="pending"&&!stopped);
 g->share_->setEnabled(!busy&&linkState!="pending"&&linkState!="unknown"&&linkState!="changed");g->revoke_->setEnabled(!busy&&data["linkAvailable"].toBool());g->inspectLink_->setEnabled(!busy);
 g->copy_->setEnabled(data["linkAvailable"].toBool());g->open_->setEnabled(data["linkAvailable"].toBool());
 if(data["superseded"].toBool()||data["remoteStatus"]=="superseded")VisualStyle::feedback(g->status_,"Superseded: this original stored link may serve replacement content. Deletion still targets the original receipt.",true);else if(!g->sharingFeedback_.isEmpty())VisualStyle::feedback(g->status_,g->sharingFeedback_,g->sharingFeedbackError_);else VisualStyle::feedback(g->status_,"File details loaded.");g->preview_->setText("Preview unavailable — "+data["mime"].toString());g->pendingPreviewPath_=data["previewPath"].toString();g->startPreview();});}
void HistoryWindow::updateInspectorLayout(){
 if(!split_||!inspectorScroll_)return;
 const bool stacked=width()<1120;
 const auto orientation=stacked?Qt::Vertical:Qt::Horizontal;
 const bool changed=split_->orientation()!=orientation;
 inspectorScroll_->setMinimumWidth(stacked?0:250);
 inspectorScroll_->setMaximumWidth(stacked?QWIDGETSIZE_MAX:300);
 inspectorScroll_->setMaximumHeight(stacked?220:QWIDGETSIZE_MAX);
 split_->setOrientation(orientation);
 split_->setStretchFactor(0,1);split_->setStretchFactor(1,0);
 if(changed)split_->setSizes(stacked?QList<int>{400,200}:QList<int>{800,280});
}
void HistoryWindow::resizeEvent(QResizeEvent* event){RetainedWindow::resizeEvent(event);updateInspectorLayout();}
void HistoryWindow::startPreview(){
 if(pendingPreviewPath_.isEmpty()||!isVisible()||previewLoading_)return;
 const auto path=std::exchange(pendingPreviewPath_,{});
 const auto seq=selectionRequest_;
 previewLoading_=true;
 auto image=std::make_shared<QImage>();
 previewCancelled_=std::make_shared<std::atomic_bool>(false);
 const auto cancelled=previewCancelled_;
 session_.background([path,image,cancelled](Error&){
  if(cancelled->load())return QJsonObject{};
  QImageReader reader(path);const auto size=reader.size();
  if(!QFileInfo(path).isFile()||QFileInfo(path).size()>8*1024*1024||!size.isValid()||qint64(size.width())*size.height()>16000000)return QJsonObject{};
  reader.setScaledSize(size.scaled(220,150,Qt::KeepAspectRatio));*image=reader.read();return QJsonObject{};
 },this,[g=QPointer<HistoryWindow>(this),seq,image](Error,QJsonObject){
  if(!g)return;
  g->previewLoading_=false;
  if(!g->isVisible())return;
  if(seq==g->selectionRequest_&&!image->isNull()){g->preview_->setMinimumHeight(image->height());g->preview_->setPixmap(QPixmap::fromImage(*image));}
  g->startPreview();
 });
}
void HistoryWindow::releasePreview(){
 if(previewCancelled_)previewCancelled_->store(true);
 pendingPreviewPath_.clear();
 ++selectionRequest_; // Invalidate pending detail/preview replies before releasing the image.
 preview_->clear();
}
void HistoryWindow::hideEvent(QHideEvent* event){
 releasePreview();RetainedWindow::hideEvent(event);
}
void HistoryWindow::showEvent(QShowEvent* event){
 RetainedWindow::showEvent(event);
 selectionChanged();
}
void HistoryWindow::changeSharing(const QString& action){
 auto ids=selectedIds();if(ids.size()!=1||sharingAccepting_||sharingTimer_->isActive())return;
 const auto historyId=ids.first();
 if(action=="revoke"&&QMessageBox::question(this,"Revoke recorded share link", "Revoke the public share link for history entry "+historyId+"?\nThe uploaded file remains. Existing member access and downloaded copies remain.",QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;
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
void HistoryWindow::prepareDelete(){auto ids=selectedIds();if(ids.isEmpty())return;session_.request("delete.prepare",{{"ids",QJsonArray::fromStringList(ids)}},this,[g=QPointer<HistoryWindow>(this),ids](Error e,QJsonObject preview){if(!g)return;if(e){VisualStyle::feedback(g->status_,e.code+": "+e.message,true);return;}QString text="Delete these original recorded remote targets?\nLocal copies and history records remain. Browser/CDN caches may still serve bytes.\n";for(auto v:preview["targets"].toArray()){auto target=v.toObject();auto receipt=target["receipt"].toObject();text+="\nHistory ID: "+target["historyId"].toString()+"\nOriginal destination / exact target: "+QString::fromUtf8(QJsonDocument(receipt).toJson(QJsonDocument::Compact))+"\nEffect: "+target["effect"].toString()+"\n";}if(QMessageBox::question(g,"Confirm exact remote deletion",text,QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;g->deleteAccepting_=true;emit g->viewStateChanged();g->session_.request("delete.confirm",{{"ids",QJsonArray::fromStringList(ids)},{"acknowledgement",preview["acknowledgement"]}},g,[g](Error failure,QJsonObject data){if(!g)return;g->deleteAccepting_=false;emit g->viewStateChanged();VisualStyle::feedback(g->status_,failure?failure.code+": "+failure.message:"Remote deletion accepted; individual outcomes will update.",bool(failure));if(!failure){g->deletionId_=data["deleteOperationId"].toString();emit g->viewStateChanged();g->deletionTimer_->start();}});});}
void HistoryWindow::pollDeletion(){if(deletionPollActive_)return;if(deletionId_.isEmpty()){deletionTimer_->stop();return;}deletionPollActive_=true;const auto id=deletionId_;session_.request("delete.inspect",{{"id",id}},this,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){if(!g)return;g->deletionPollActive_=false;if(g->deletionId_!=id)return;if(e){VisualStyle::feedback(g->status_,e.message,true);g->deletionTimer_->stop();return;}QString status="Deletion "+data["id"].toString()+": "+data["state"].toString();for(auto v:data["items"].toArray()){auto item=v.toObject();status+="\n"+item["historyId"].toString()+": "+item["status"].toString()+" "+item["error"].toObject()["message"].toString();}g->status_->setText(status);if(data["state"]!="running"&&data["state"]!="accepted"){g->deletionTimer_->stop();g->refresh();}});}
QJsonObject HistoryWindow::saveViewState()const{
 if(deleteAccepting_||sharingAccepting_)return {{"stateError","request_awaiting_acceptance"}};
 const auto& q=model_->query();QJsonObject query{{"limit",q.limit},{"offset",q.offset},{"search",q.search},{"category",q.category},{"mime",q.mime},{"profileId",q.profileId},{"uploadStatus",q.uploadStatus},{"localStatus",q.localStatus},{"remoteStatus",q.remoteStatus},{"from",q.from},{"to",q.to}};
 QJsonObject filters{{"search",findChild<QLineEdit*>("historySearch")->text()}};for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})filters[key]=findChild<QLineEdit*>("historyFilter"+key)->text();
 return {{"query",query},{"filters",filters},{"selectedIds",QJsonArray::fromStringList(restoringSelection_?restoreSelection_:selectedIds())},{"deletionId",deletionId_},{"sharingId",sharingId_},{"filtersExpanded",findChild<QToolButton*>("historyFiltersExpanded")->isChecked()},{"managementExpanded",findChild<QToolButton*>("historyManagementExpanded")->isChecked()},{"receiptExpanded",findChild<QToolButton*>("historyReceiptExpanded")->isChecked()},{"geometry",QJsonArray{x(),y(),width(),height()}}};
}
bool HistoryWindow::restoreViewState(const QJsonObject& state){
 if(validateUiState({{"schemaVersion",1},{"history",state}}))return false;
 const QSignalBlocker stateSignals(this);const auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());
 const auto f=state["filters"].toObject();findChild<QLineEdit*>("historySearch")->setText(f["search"].toString());for(const auto& key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"})findChild<QLineEdit*>("historyFilter"+key)->setText(f[key].toString());
 findChild<QToolButton*>("historyFiltersExpanded")->setChecked(state["filtersExpanded"].toBool());findChild<QToolButton*>("historyManagementExpanded")->setChecked(state["managementExpanded"].toBool());findChild<QToolButton*>("historyReceiptExpanded")->setChecked(state["receiptExpanded"].toBool());
 restoreSelection_.clear();for(auto id:state["selectedIds"].toArray())restoreSelection_.append(id.toString());restoringSelection_=true;deletionId_=state["deletionId"].toString();if(!deletionId_.isEmpty()){deletionTimer_->start();pollDeletion();}
 // Retain only the accepted job ID; restoration inspects it and never repeats a mutation.
 sharingId_=state["sharingId"].toString();sharingStopped_=false;sharingFeedback_.clear();sharingTimer_->stop();if(!sharingId_.isEmpty()){sharingTimer_->start();pollSharing();}
 const auto data=state["query"].toObject();UiHistoryQuery q;q.limit=data["limit"].toInt(50);q.offset=data["offset"].toInt();q.search=data["search"].toString();q.category=data["category"].toString();q.mime=data["mime"].toString();q.profileId=data["profileId"].toString();q.uploadStatus=data["uploadStatus"].toString();q.localStatus=data["localStatus"].toString();q.remoteStatus=data["remoteStatus"].toString();q.from=data["from"].toString();q.to=data["to"].toString();model_->setQuery(q);return true;
}
}
