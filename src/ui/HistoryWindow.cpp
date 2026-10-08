#include "HistoryWindow.h"
#include "VisualStyle.h"
#include "ByteSize.h"
#include <QApplication>
#include <QSplitter>
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
#include <utility>
namespace putgd {
HistoryWindow::HistoryWindow(HistoryService& history,OperationService& operations,QWidget* parent):RetainedWindow(parent),history_(history),operations_(operations){setObjectName("putgdWindow");VisualStyle::apply(*qApp);setWindowIcon(VisualStyle::icon("brand"));setWindowTitle("put.gd — History");resize(1000,700);auto* layout=new QVBoxLayout(this);layout->setContentsMargins(20,18,20,18);layout->setSpacing(12);
 layout->addWidget(VisualStyle::header("History","Stored links and file outcomes. Available offline.",this));
 auto* filters=new QGridLayout;filters->setHorizontalSpacing(10);filters->setVerticalSpacing(10);layout->addLayout(filters);
 auto* search=new QLineEdit(this);search->setObjectName("historySearch");search->setAccessibleName("Search history by stored name");search->setMaxLength(256);search->setPlaceholderText("Search files by name…");filters->addWidget(search,0,0,1,3);auto* filter=new QPushButton("Search",this);filters->addWidget(filter,0,3);
 auto* advancedToggle=new QToolButton(this);advancedToggle->setText("Filters");advancedToggle->setCheckable(true);advancedToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);advancedToggle->setArrowType(Qt::RightArrow);filters->addWidget(advancedToggle,0,4);
 auto* advanced=new QWidget(this);auto* advancedLayout=new QGridLayout(advanced);advancedLayout->setContentsMargins(0,0,0,0);advancedLayout->setSpacing(10);layout->addWidget(advanced);advanced->hide();connect(advancedToggle,&QToolButton::toggled,this,[advanced,advancedToggle](bool visible){advanced->setVisible(visible);advancedToggle->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});
 QHash<QString,QLineEdit*> fields;const QHash<QString,QString> filterLabels{{"from","From date (ISO)"},{"to","To date (ISO)"},{"mime","Content type"},{"profileId","Destination ID"},{"uploadStatus","Upload state"},{"localStatus","Local-copy state"},{"remoteStatus","Remote-deletion state"}};int filterIndex=0;for(auto key:QStringList{"from","to","mime","profileId","uploadStatus","localStatus","remoteStatus"}){auto* e=new QLineEdit(this);e->setObjectName("historyFilter"+key);e->setAccessibleName(filterLabels.value(key));e->setPlaceholderText(filterLabels.value(key));advancedLayout->addWidget(e,filterIndex/4,filterIndex%4);++filterIndex;fields[key]=e;}
 empty_=new QLabel("No history entries.\nUploads and local captures appear here.",this);empty_->setWordWrap(true);empty_->setAlignment(Qt::AlignCenter);VisualStyle::role(empty_,"status");empty_->hide();layout->addWidget(empty_);
 model_=new HistoryModel(history_,this);table_=new QTableView(this);table_->setObjectName("historyTable");table_->setAccessibleName("Stored upload and local capture history");table_->setModel(model_);table_->setSelectionBehavior(QAbstractItemView::SelectRows);table_->setSelectionMode(QAbstractItemView::ExtendedSelection);VisualStyle::decorateView(table_,{4,5,6});table_->setShowGrid(false);table_->verticalHeader()->hide();table_->verticalHeader()->setDefaultSectionSize(38);table_->horizontalHeader()->setSectionResizeMode(QHeaderView::Interactive);table_->horizontalHeader()->setStretchLastSection(false);for(int column=0;column<7;++column)table_->setColumnWidth(column,column==0?220:column==1?170:150);
 auto* split=new QSplitter(Qt::Horizontal,this);split->setChildrenCollapsible(false);split->addWidget(table_);table_->setMinimumWidth(340);
 auto* inspector=VisualStyle::card("File details","",this);inspector->setMinimumWidth(230);inspector->setMaximumWidth(340);auto* inspectorLayout=qobject_cast<QVBoxLayout*>(inspector->layout());
 preview_=new QLabel("No entry selected",this);preview_->setObjectName("historyPreview");preview_->setWordWrap(true);preview_->setAlignment(Qt::AlignCenter);preview_->setMaximumHeight(180);VisualStyle::role(preview_,"muted");inspectorLayout->addWidget(preview_);
 details_=new QPlainTextEdit(this);details_->setObjectName("historyDetails");details_->setAccessibleName("Original receipt details JSON");details_->setReadOnly(true);details_->setPlaceholderText("Select an entry to view its stored receipt.");details_->setMinimumHeight(90);details_->hide();inspectorLayout->addWidget(details_,1);
 summary_=new QPlainTextEdit(this);summary_->setObjectName("historySummary");summary_->setAccessibleName("Readable original receipt and independent outcomes");summary_->setReadOnly(true);summary_->setPlaceholderText("Select an entry to view its destination and outcomes.");summary_->setMinimumHeight(90);inspectorLayout->addWidget(summary_,1);
 auto* receiptToggle=new QToolButton(this);receiptToggle->setText("Exact receipt");receiptToggle->setCheckable(true);receiptToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);receiptToggle->setArrowType(Qt::RightArrow);inspectorLayout->addWidget(receiptToggle);connect(receiptToggle,&QToolButton::toggled,this,[this,receiptToggle](bool visible){details_->setVisible(visible);summary_->setVisible(!visible);receiptToggle->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});auto* inspectorScroll=new QScrollArea(this);inspectorScroll->setObjectName("historyInspectorScroll");inspectorScroll->setAccessibleName("Original receipt inspector");inspectorScroll->setWidgetResizable(true);inspectorScroll->setFrameShape(QFrame::NoFrame);inspectorScroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);inspectorScroll->setMinimumWidth(290);inspectorScroll->setMaximumWidth(340);inspectorScroll->setWidget(inspector);split->addWidget(inspectorScroll);split->setSizes({650,290});layout->addWidget(split,1);
 auto* paging=new QHBoxLayout;paging->setSpacing(8);layout->addLayout(paging);count_=new QLabel("Loading local history…",this);VisualStyle::role(count_,"muted");count_->setWordWrap(true);paging->addWidget(count_,1);auto* previous=new QPushButton("Previous",this);auto* next=new QPushButton("Next",this);paging->addWidget(previous);paging->addWidget(next);auto* refreshButton=new QPushButton("Refresh",this);paging->addWidget(refreshButton);
 auto apply=[this,search,fields]{HistoryQuery q;q.search=search->text();q.from=fields["from"]->text();q.to=fields["to"]->text();q.mime=fields["mime"]->text();q.profileId=fields["profileId"]->text();q.uploadStatus=fields["uploadStatus"]->text();q.localStatus=fields["localStatus"]->text();q.remoteStatus=fields["remoteStatus"]->text();model_->setQuery(q);};connect(filter,&QPushButton::clicked,this,apply);connect(search,&QLineEdit::returnPressed,this,apply);connect(previous,&QPushButton::clicked,this,[this]{auto q=model_->query();q.offset=qMax(0,q.offset-q.limit);model_->setQuery(q);});connect(next,&QPushButton::clicked,this,[this]{auto q=model_->query();if(q.offset+q.limit<model_->total()){q.offset+=q.limit;model_->setQuery(q);}});connect(refreshButton,&QPushButton::clicked,this,&HistoryWindow::refresh);
 status_=new QLabel("Select an entry to inspect or copy its stored link.",this);status_->setAccessibleName("History status");status_->setWordWrap(true);VisualStyle::role(status_,"status");layout->addWidget(status_);
 auto* buttons=new QHBoxLayout;buttons->setSpacing(10);layout->addLayout(buttons);
 copy_=new QPushButton("Copy link",this);copy_->setObjectName("copyHistoryLink");VisualStyle::role(copy_,"primary");open_=new QPushButton("Open link",this);open_->setObjectName("openHistoryLink");copy_->setEnabled(false);open_->setEnabled(false);buttons->addWidget(copy_);buttons->addWidget(open_);buttons->addStretch();auto* remove=new QPushButton("Delete selected uploads…",this);remove->setToolTip("Delete selected remote uploads; local copies and history remain");remove->setObjectName("deleteHistoryRemote");VisualStyle::role(remove,"danger");buttons->addWidget(remove);
 auto* managementToggle=new QToolButton(this);managementToggle->setText("Manage");managementToggle->setCheckable(true);managementToggle->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);managementToggle->setArrowType(Qt::RightArrow);buttons->addWidget(managementToggle);
 auto* management=new QWidget(this);auto* managementLayout=new QGridLayout(management);managementLayout->setContentsMargins(0,0,0,0);managementLayout->setSpacing(8);layout->addWidget(management);management->hide();connect(managementToggle,&QToolButton::toggled,this,[management,managementToggle](bool visible){management->setVisible(visible);managementToggle->setArrowType(visible?Qt::DownArrow:Qt::RightArrow);});
 auto* retry=new QPushButton("Retry deletion",this);managementLayout->addWidget(retry,0,0);auto* cancel=new QPushButton("Cancel deletion",this);managementLayout->addWidget(cancel,0,1);auto* clear=new QPushButton("Clear history records…",this);managementLayout->addWidget(clear,1,0);auto* cache=new QPushButton("Clean retry artifacts and previews…",this);managementLayout->addWidget(cache,1,1);
 connect(table_->selectionModel(),&QItemSelectionModel::selectionChanged,this,[this]{selectionChanged();});connect(model_,&HistoryModel::loaded,this,[this]{if(awaitingInitialization_){awaitingInitialization_=false;VisualStyle::feedback(status_,"Select an entry to inspect or copy its stored link.");}empty_->setVisible(model_->total()==0);count_->setText(QString("%1–%2 of %3 entries").arg(model_->total()==0?0:model_->query().offset+1).arg(model_->query().offset+model_->rowCount()).arg(model_->total()));selectionChanged();});connect(model_,&HistoryModel::error,this,[this](const QString& message){VisualStyle::feedback(status_,message,true);});
 connect(copy_,&QPushButton::clicked,this,[this]{auto ids=selectedIds();if(ids.size()!=1)return;history_.copyLink(ids.first(),[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.code+": "+e.message:"Stored link copied.",bool(e));});});connect(open_,&QPushButton::clicked,this,[this]{auto rows=table_->selectionModel()->selectedRows();if(rows.size()==1&&model_->linkAvailable(rows.first().row()))QDesktopServices::openUrl(QUrl(model_->record(rows.first().row())["receipt"].toObject()["directUrl"].toString()));});connect(remove,&QPushButton::clicked,this,&HistoryWindow::prepareDelete);
 connect(retry,&QPushButton::clicked,this,[this]{if(deletionId_.isEmpty())return;history_.retryDelete(deletionId_,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"Deletion retry accepted.",bool(e));if(!e)g->deletionTimer_->start();}});});connect(cancel,&QPushButton::clicked,this,[this]{if(deletionId_.isEmpty())return;history_.cancelDelete(deletionId_,[g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Cancellation requested; already sent deletion cannot be rolled back.",bool(e));});});
 connect(clear,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Clear history","Clear local history records? No remote objects or user-owned local copies will be removed.")!=QMessageBox::Yes)return;history_.clear([g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.message:"History cleared; remote objects unchanged.",bool(e));g->refresh();}});});connect(cache,&QPushButton::clicked,this,[this]{if(QMessageBox::question(this,"Clean artifacts","Clean eligible retry artifacts and image previews? Persistent history, remote objects, and local copies remain. Some cleaned artifacts cannot retry.")!=QMessageBox::Yes)return;operations_.cleanup([g=QPointer<HistoryWindow>(this)](Error e,QJsonObject){if(g)VisualStyle::feedback(g->status_,e?e.message:"Artifact cleanup completed.",bool(e));});});deletionTimer_=new QTimer(this);deletionTimer_->setInterval(500);connect(deletionTimer_,&QTimer::timeout,this,&HistoryWindow::pollDeletion);connect(&operations_,&OperationService::changed,this,[this]{refresh();});connect(&operations_,&OperationService::initialized,this,[this]{refresh();});refresh();}
void HistoryWindow::refresh(){if(!operations_.ready()){awaitingInitialization_=true;VisualStyle::feedback(status_,"History is waiting for the resident journal to initialize.");return;}model_->refresh();}
QStringList HistoryWindow::selectedIds()const{QStringList ids;for(auto row:table_->selectionModel()->selectedRows())ids.append(model_->record(row.row())["id"].toString());return ids;}
void HistoryWindow::selectionChanged(){if(previewCancelled_)previewCancelled_->store(true);pendingPreviewPath_.clear();auto rows=table_->selectionModel()->selectedRows();bool available=rows.size()==1&&model_->linkAvailable(rows.first().row());copy_->setEnabled(available);open_->setEnabled(available);details_->clear();summary_->clear();preview_->clear();preview_->setMinimumHeight(36);preview_->setText("No entry selected");const auto seq=++selectionRequest_;if(rows.size()!=1)return;auto record=model_->record(rows.first().row());if(!deletionTimer_||!deletionTimer_->isActive())deletionId_=record["deleteOperationId"].toString();history_.detail(record["id"].toString(),[g=QPointer<HistoryWindow>(this),seq](Error e,QJsonObject data){if(!g||g->selectionRequest_!=seq)return;if(e){VisualStyle::feedback(g->status_,e.message,true);return;}g->details_->setPlainText(QString::fromUtf8(QJsonDocument(data).toJson(QJsonDocument::Indented)));auto local=data["local"].toObject();auto receipt=data["receipt"].toObject();auto state=[](QString value){value.replace('_',' ');return value.isEmpty()?QString("Not recorded"):value;};g->summary_->setPlainText(data["name"].toString()+"\n"+data["mime"].toString()+"\n"+ByteSize::summary(data["bytes"].toInteger())+"\n\nOriginal destination\n"+data["profileId"].toString()+"\n\nStored link\n"+receipt["directUrl"].toString()+"\n\nUpload: "+state(data["uploadStatus"].toString())+"\nLocal copy: "+state(local["status"].toString())+"\nRemote: "+state(data["remoteStatus"].toString())+(local["path"].toString().isEmpty()?QString{}:"\n\nLocal file\n"+local["path"].toString()));if(data["superseded"].toBool()||data["remoteStatus"]=="superseded")VisualStyle::feedback(g->status_,"Superseded: this original stored link may serve replacement content. Deletion still targets the original receipt.",true);else VisualStyle::feedback(g->status_,"File details loaded.");g->preview_->setText("Preview unavailable — "+data["mime"].toString());g->pendingPreviewPath_=data["previewPath"].toString();g->startPreview();});}
void HistoryWindow::startPreview(){
 if(pendingPreviewPath_.isEmpty()||!isVisible()||previewLoading_)return;
 const auto path=std::exchange(pendingPreviewPath_,{});
 const auto seq=selectionRequest_;
 previewLoading_=true;
 auto image=std::make_shared<QImage>();
 previewCancelled_=std::make_shared<std::atomic_bool>(false);
 const auto cancelled=previewCancelled_;
 operations_.background([path,image,cancelled](Error&){
  if(cancelled->load())return QJsonObject{};
  QImageReader reader(path);const auto size=reader.size();
  if(!QFileInfo(path).isFile()||QFileInfo(path).size()>8*1024*1024||!size.isValid()||qint64(size.width())*size.height()>16000000)return QJsonObject{};
  reader.setScaledSize(size.scaled(220,150,Qt::KeepAspectRatio));*image=reader.read();return QJsonObject{};
 },[g=QPointer<HistoryWindow>(this),seq,image](Error,QJsonObject){
  if(!g)return;
  g->previewLoading_=false;
  if(!g->isVisible())return;
  if(seq==g->selectionRequest_&&!image->isNull()){g->preview_->setMinimumHeight(image->height());g->preview_->setPixmap(QPixmap::fromImage(*image));}
  g->startPreview();
 });
}
void HistoryWindow::hideEvent(QHideEvent* event){
 if(previewCancelled_)previewCancelled_->store(true);
 pendingPreviewPath_.clear();
 ++selectionRequest_; // Invalidate pending detail/preview replies before releasing the image.
 preview_->clear();
 RetainedWindow::hideEvent(event);
}
void HistoryWindow::showEvent(QShowEvent* event){
 RetainedWindow::showEvent(event);
 selectionChanged();
}
void HistoryWindow::prepareDelete(){auto ids=selectedIds();if(ids.isEmpty())return;history_.prepareDelete(ids,[g=QPointer<HistoryWindow>(this),ids](Error e,QJsonObject preview){if(!g)return;if(e){VisualStyle::feedback(g->status_,e.code+": "+e.message,true);return;}QString text="Delete these original recorded remote targets?\nLocal copies and history records remain. Browser/CDN caches may still serve bytes.\n";for(auto v:preview["targets"].toArray()){auto target=v.toObject();auto receipt=target["receipt"].toObject();text+="\nHistory ID: "+target["historyId"].toString()+"\nOriginal destination / exact target: "+QString::fromUtf8(QJsonDocument(receipt).toJson(QJsonDocument::Compact))+"\nEffect: "+target["effect"].toString()+"\n";}if(QMessageBox::question(g,"Confirm exact remote deletion",text,QMessageBox::Yes|QMessageBox::Cancel,QMessageBox::Cancel)!=QMessageBox::Yes)return;g->history_.deleteConfirmed(ids,preview["acknowledgement"].toString(),[g](Error failure,QJsonObject data){if(!g)return;VisualStyle::feedback(g->status_,failure?failure.code+": "+failure.message:"Remote deletion accepted; individual outcomes will update.",bool(failure));if(!failure){g->deletionId_=data["deleteOperationId"].toString();g->deletionTimer_->start();}});});}
void HistoryWindow::pollDeletion(){if(deletionPollActive_)return;if(deletionId_.isEmpty()){deletionTimer_->stop();return;}deletionPollActive_=true;const auto id=deletionId_;history_.inspectDelete(id,[g=QPointer<HistoryWindow>(this),id](Error e,QJsonObject data){if(!g)return;g->deletionPollActive_=false;if(g->deletionId_!=id)return;if(e){VisualStyle::feedback(g->status_,e.message,true);g->deletionTimer_->stop();return;}QString status="Deletion "+data["id"].toString()+": "+data["state"].toString();for(auto v:data["items"].toArray()){auto item=v.toObject();status+="\n"+item["historyId"].toString()+": "+item["status"].toString()+" "+item["error"].toObject()["message"].toString();}g->status_->setText(status);if(data["state"]!="running"&&data["state"]!="accepted"){g->deletionTimer_->stop();g->refresh();}});}
}
