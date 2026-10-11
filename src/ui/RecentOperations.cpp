#include "RecentOperations.h"
#include "ViewState.h"
#include <QJsonArray>
#include "VisualStyle.h"
#include "ByteSize.h"
#include <QApplication>
#include <QHeaderView>
#include <QTreeWidget>
#include <QStackedWidget>
#include <QLabel>
#include <QPushButton>
#include <QPointer>
#include <QVBoxLayout>
#include <QSignalBlocker>
#include <QStyledItemDelegate>
#include <QPainter>
namespace putgd {
namespace {
QString readableState(QString value){value.replace('_',' ');return value.isEmpty()?QString("Not recorded"):value.left(1).toUpper()+value.mid(1);}
QString fileDetails(const QJsonObject& item){
 auto first=ByteSize::summary(item["artifact"].toObject()["bytes"].toInteger());
 const auto error=item["error"].toObject()["message"].toString();if(!error.isEmpty())first+=" · "+error;
 auto second="Remote: "+readableState(item["remoteStatus"].toString());
 const auto localError=item["local"].toObject()["error"].toObject()["message"].toString();if(!localError.isEmpty())second+=" · Local save: "+localError;
 return first+"\n"+second;
}
// File rows reserve two compact lines for evidence. Parent operation badges keep
// the shared renderer; full file paths and errors remain in the existing tooltips.
class FileDetailsDelegate final:public QStyledItemDelegate {
public:
 FileDetailsDelegate(QAbstractItemDelegate* fallback,QObject* parent):QStyledItemDelegate(parent),fallback_(fallback){}
 void paint(QPainter* painter,const QStyleOptionViewItem& option,const QModelIndex& index)const override{
  const auto text=index.data().toString();if(!index.parent().isValid()||!text.contains('\n')){fallback_->paint(painter,option,index);return;}
  QStyleOptionViewItem cell(option);initStyleOption(&cell,index);cell.text.clear();cell.widget->style()->drawControl(QStyle::CE_ItemViewItem,&cell,painter,cell.widget);
  const auto lines=text.split('\n');const auto height=cell.fontMetrics.height();const auto top=cell.rect.center().y()-height;
  painter->save();painter->setClipRect(cell.rect);painter->setFont(cell.font);painter->setPen(cell.palette.color(cell.state&QStyle::State_Enabled?QPalette::Active:QPalette::Disabled,cell.state&QStyle::State_Selected?QPalette::HighlightedText:QPalette::Text));
  for(int line=0;line<2;++line){const QRect bounds(cell.rect.left()+8,top+line*height, qMax(0,cell.rect.width()-16),height);painter->drawText(bounds,Qt::AlignLeft|Qt::AlignVCenter,cell.fontMetrics.elidedText(lines.value(line),line==1&&index.column()==3?Qt::ElideMiddle:Qt::ElideRight,bounds.width()));}
  painter->restore();
 }
 QSize sizeHint(const QStyleOptionViewItem& option,const QModelIndex& index)const override{
  auto size=fallback_->sizeHint(option,index);if(index.parent().isValid()&&index.data().toString().contains('\n')){QStyleOptionViewItem cell(option);initStyleOption(&cell,index);size.setHeight(qMax(size.height(),2*cell.fontMetrics.height()+12));}return size;
 }
private:QAbstractItemDelegate* fallback_;
};
}
RecentOperations::RecentOperations(UiSession& operations,QWidget* parent):RetainedWindow(parent),operations_(operations){setObjectName("putgdWindow");VisualStyle::apply(*qApp);setWindowIcon(VisualStyle::icon("brand"));setWindowTitle("put.gd — Activity");resize(1000,700);auto* layout=new QVBoxLayout(this);layout->setContentsMargins(20,18,20,18);layout->setSpacing(12);
 layout->addWidget(VisualStyle::header("Activity","Capture, upload and clipboard results.",this));
 count_=new QLabel("Recent operations",this);VisualStyle::role(count_,"sectionTitle");layout->addWidget(count_);
 empty_=new QLabel("No recent activity.\nCapture or use the clipboard to see results here.",this);empty_->setWordWrap(true);empty_->setAlignment(Qt::AlignCenter);empty_->setObjectName("activityEmpty");VisualStyle::role(empty_,"muted");
 tree_=new QTreeWidget(this);tree_->setObjectName("recentOperations");tree_->setAccessibleName("Recent captures and individual outcomes");tree_->setHeaderLabels({"Action / file","Operation ID / link","Upload","Clipboard / local file","Details"});VisualStyle::decorateView(tree_,{2,3});tree_->setRootIsDecorated(true);tree_->setIndentation(22);tree_->setUniformRowHeights(false);auto* detailsDelegate=new FileDetailsDelegate(tree_->itemDelegate(),tree_);tree_->setItemDelegateForColumn(3,detailsDelegate);tree_->setItemDelegateForColumn(4,detailsDelegate);tree_->header()->setStretchLastSection(false);for(int column=0;column<5;++column)tree_->header()->setSectionResizeMode(column,QHeaderView::Interactive);tree_->header()->setSectionResizeMode(0,QHeaderView::Stretch);tree_->setColumnWidth(1,140);tree_->setColumnWidth(2,110);tree_->setColumnWidth(3,170);tree_->setColumnWidth(4,190);list_=new QStackedWidget(this);list_->addWidget(tree_);list_->addWidget(empty_);layout->addWidget(list_,1);
 status_=new QLabel("Select an operation to copy its result or retry.",this);status_->setAccessibleName("Activity status");status_->setWordWrap(true);VisualStyle::role(status_,"status");layout->addWidget(status_);auto* buttons=new QHBoxLayout;buttons->setSpacing(10);layout->addLayout(buttons);
 auto add=[this,buttons](const QString& label,const QString& name,auto fn){auto* button=new QPushButton(label,this);button->setObjectName(name);if(name=="copyOperationResult")VisualStyle::role(button,"primary");buttons->addWidget(button,0,Qt::AlignLeft);connect(button,&QPushButton::clicked,this,[this,fn]{auto* row=tree_->currentItem();if(!row)return;while(row->parent())row=row->parent();const auto id=row->data(0,Qt::UserRole).toString();fn(id,[g=QPointer<RecentOperations>(this)](Error e,QJsonObject){if(g){VisualStyle::feedback(g->status_,e?e.code+": "+e.message:"Action accepted.",bool(e));g->refresh();}});});};
 add("Copy result","copyOperationResult",[this](auto id,auto reply){operations_.request("operation.copy",{{"id",id}},this,reply);});buttons->addStretch();add("Retry operation","retryOperation",[this](auto id,auto reply){operations_.request("operation.retry",{{"id",id}},this,reply);});add("Retry local save","retryLocalSave",[this](auto id,auto reply){operations_.request("operation.retryLocalSave",{{"id",id}},this,reply);});add("Cancel operation","cancelOperation",[this](auto id,auto reply){operations_.request("operation.cancel",{{"id",id}},this,reply);});connect(&operations_,&UiSession::operationsChanged,this,[this]{refresh();});connect(&operations_,&UiSession::initialized,this,[this]{refresh();});connect(tree_,&QTreeWidget::itemSelectionChanged,this,[this]{if(restoring_)return;selected_.clear();selectedItem_.clear();if(auto* row=tree_->currentItem()){if(row->parent()){selectedItem_=row->data(0,Qt::UserRole).toString();row=row->parent();}selected_=row->data(0,Qt::UserRole).toString();}emit viewStateChanged();});
 connect(tree_,&QTreeWidget::itemExpanded,this,[this](QTreeWidgetItem* item){if(restoring_)return;collapsed_.remove(item->data(0,Qt::UserRole).toString());emit viewStateChanged();});
 connect(tree_,&QTreeWidget::itemCollapsed,this,[this](QTreeWidgetItem* item){if(restoring_)return;collapsed_.insert(item->data(0,Qt::UserRole).toString());emit viewStateChanged();});
 if(operations_.ready())refresh();}
void RecentOperations::refresh(){const auto sequence=++request_;if(loadInFlight_){loadPending_=true;return;}loadInFlight_=true;operations_.request("operation.recent",{},this,[g=QPointer<RecentOperations>(this),sequence](Error e,QJsonObject data){if(!g)return;g->loadInFlight_=false;if(g->loadPending_){g->loadPending_=false;g->refresh();return;}if(sequence!=g->request_)return;if(e){VisualStyle::feedback(g->status_,e.code+": "+e.message,true);return;}g->restoring_=true;g->tree_->clear();QSet<QString> present;const auto values=data["operations"].toArray();g->list_->setCurrentWidget(values.isEmpty()?static_cast<QWidget*>(g->empty_):static_cast<QWidget*>(g->tree_));g->count_->setText(QString("Recent operations: %1").arg(values.size()));for(auto value:values){auto op=value.toObject();const auto operationId=op["id"].toString();const auto displayedId=operationId.size()>8?operationId.left(8)+QString::fromUtf8("…"):operationId;auto* row=new QTreeWidgetItem(g->tree_,{op["action"].toString(),displayedId,op["state"].toString(),op["copyStatus"].toString(),op["error"].toObject()["message"].toString()});row->setData(0,Qt::UserRole,operationId);present.insert(operationId);row->setIcon(0,VisualStyle::icon(op["action"].toString()));for(int column=0;column<5;++column)row->setToolTip(column,row->text(column));row->setToolTip(1,operationId);for(auto itemValue:op["items"].toArray()){auto item=itemValue.toObject();auto local=item["local"].toObject();auto* child=new QTreeWidgetItem(row,{item["artifact"].toObject()["name"].toString(),item["receipt"].toObject()["directUrl"].toString(),item["uploadStatus"].toString(),(local["path"].toString().isEmpty()?local["status"].toString():readableState(local["status"].toString())+"\n"+local["path"].toString()),fileDetails(item)});child->setData(0,Qt::UserRole,item["artifact"].toObject()["id"].toString());if(operationId==g->selected_&&child->data(0,Qt::UserRole).toString()==g->selectedItem_)g->tree_->setCurrentItem(child);child->setIcon(0,VisualStyle::icon(item["artifact"].toObject()["category"].toString()));for(int column=0;column<5;++column)child->setToolTip(column,child->text(column));child->setToolTip(3,local["status"].toString()+" "+local["path"].toString());child->setToolTip(4,child->text(4)+"\nRemote state: "+item["remoteStatus"].toString());}row->setExpanded(!g->collapsed_.contains(operationId));if(operationId==g->selected_&&g->selectedItem_.isEmpty())g->tree_->setCurrentItem(row);}g->collapsed_.intersect(present);if(!present.contains(g->selected_)){g->selected_.clear();g->selectedItem_.clear();}g->restoring_=false;emit g->viewStateChanged();});}
QJsonObject RecentOperations::saveViewState()const{return {{"selectedId",selected_},{"selectedItemId",selectedItem_},{"collapsedIds",QJsonArray::fromStringList(collapsed_.values())},{"geometry",QJsonArray{x(),y(),width(),height()}}};}
bool RecentOperations::restoreViewState(const QJsonObject& state){if(validateUiState({{"schemaVersion",1},{"activity",state}}))return false;const QSignalBlocker stateSignals(this);selected_=state["selectedId"].toString();selectedItem_=state["selectedItemId"].toString();collapsed_.clear();for(auto id:state["collapsedIds"].toArray())collapsed_.insert(id.toString());auto box=state["geometry"].toArray();setGeometry(box[0].toInt(),box[1].toInt(),box[2].toInt(),box[3].toInt());refresh();return true;}
}
