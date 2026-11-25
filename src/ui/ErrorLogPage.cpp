#include "ErrorLogPage.h"
#include "HistoryWindow.h"
#include "PreviewReader.h"
#include "Ui.h"
#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QGridLayout>
#include <QHeaderView>
#include <QImageReader>
#include <QJsonDocument>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMenu>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSplitter>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>
namespace putgd {
using Ui::Space;
using Ui::space;
namespace {
enum Column {TimeColumn,ActionColumn,ItemColumn,DestinationColumn,MessageColumn,StatusColumn};
constexpr int IdRole=Qt::UserRole+1;
const QStringList& stepOrder() {static const QStringList order{"unlock_credentials","open_destination","retry","remove_from_history","show_in_history","save_diagnostics"};return order;}
// The details put.gd already records with an error, in plain names, in this order.
const QList<std::pair<QString,QString>>& detailFields() {
 static const QList<std::pair<QString,QString>> fields{
  {"httpStatus","HTTP status"},{"serviceCode","Service code"},{"requestStage","Stage"},{"stage","Stage"},{"check","Check"},
  {"requestId","S3 request ID"},{"extendedRequestId","S3 extended request ID"},{"curlCode","Network code"},{"elapsedMs","Time taken"},
  {"deletionStatus","Deletion outcome"},{"shareStatus","Link status"},{"copyStatus","Copy status"},{"method","Session method"}};
 return fields;
}
QString detailValue(const QString& key,const QJsonValue& value) {
 if(key=="elapsedMs"&&value.isDouble())return QString::number(value.toDouble()/1000.0,'f',1)+" s";
 if(value.isString())return value.toString();
 if(value.isDouble())return QString::number(value.toDouble());
 if(value.isBool())return value.toBool()?QStringLiteral("Yes"):QStringLiteral("No");
 return {};
}
}
QString ErrorLogPage::kindLabel(const QString& kind) {
 static const QHash<QString,QString> labels{{"upload","Upload"},{"capture","Capture"},{"recording","Recording"},{"local_save","Local copy"},{"copy","Copy"},{"deletion","Deletion"},
  {"share_link","Share link"},{"destination_test","Destination test"},{"destination_setup","Destination setup"},{"credentials","Credentials"},{"settings_save","Settings save"},
  {"text_recognition","Copy text"},{"diagnostics","Diagnostics"},{"history","History"},{"desktop_setup","Desktop setup"},{"other","Other"}};
 return labels.value(kind,kind);
}
QString ErrorLogPage::stepLabel(const QString& step,const QString& kind) {
 if(step=="unlock_credentials")return "Unlock saved credentials";
 if(step=="open_destination")return "Open destination settings";
 if(step=="remove_from_history")return "Remove from History only";
 if(step=="show_in_history")return "Show in History";
 if(step=="save_diagnostics")return "Save diagnostics for this operation";
 if(kind=="deletion")return "Retry deletion";
 if(kind=="destination_test")return "Test connection again";
 if(kind=="copy")return "Copy again";
 if(kind=="local_save")return "Retry local copy";
 return "Retry upload";
}
ErrorLogPage::ErrorLogPage(UiSession& session,QWidget* parent):QWidget(parent),session_(session) {
 setObjectName("errorLogPage");
 auto* outer=new QVBoxLayout(this);outer->setContentsMargins(0,0,0,0);outer->setSpacing(0);
 auto* body=new Ui::Surface("preferencePage",{},this);body->setObjectName("errorLogBody");outer->addWidget(body,1);
 auto* box=new QVBoxLayout(body);box->setContentsMargins(space(Space::xxl),qMin(Ui::pageHeaderTop(),space(Space::l)),space(Space::xxl),space(Space::xl));box->setSpacing(space(Space::l));
 // The same header as the other Settings pages: title and one line of what it is for.
 auto* heading=new QLabel("Error log",body);heading->setObjectName("settingsPageHeadingErrors");Ui::role(heading,"title");heading->setMinimumHeight(Ui::scaled(Ui::Metric::control));heading->setAlignment(Qt::AlignLeft|Qt::AlignVCenter);
 auto* titleRow=new QHBoxLayout;titleRow->setContentsMargins(0,0,0,0);titleRow->setSpacing(space(Space::l));titleRow->addWidget(heading,1);
 clear_=new QPushButton("Clear…",body);clear_->setObjectName("errorLogClear");clear_->setAccessibleName("Clear the error log");
 auto* clearMenu=new QMenu(clear_);clearMenu->setObjectName("errorLogClearMenu");
 clearMenu->addAction("Clear resolved entries…",this,[this]{openClear(true);})->setObjectName("errorLogClearResolved");
 clearMenu->addAction("Clear all entries…",this,[this]{openClear(false);})->setObjectName("errorLogClearAll");
 clear_->setMenu(clearMenu);titleRow->addWidget(clear_,0,Qt::AlignVCenter);box->addLayout(titleRow);
 auto* intro=new QLabel("Problems put.gd ran into, newest first. Select one to see what happened and what to do next. Entries are kept for 90 days, up to 500.",body);intro->setObjectName("settingsPageDescriptionErrors");intro->setWordWrap(true);Ui::role(intro,"muted");box->addWidget(intro);
 // Filters: kind of action, search and open problems only.
 auto* filters=new QHBoxLayout;filters->setSpacing(space(Space::s));
 kind_=Ui::choiceBox(body);kind_->setObjectName("errorLogKind");kind_->setAccessibleName("Kind of action");kind_->addItem("All actions",QString{});
 for(const auto& kind:QStringList{"upload","capture","recording","local_save","copy","deletion","share_link","destination_test","destination_setup","credentials","settings_save","text_recognition","diagnostics","history","desktop_setup","other"})kind_->addItem(kindLabel(kind),kind);
 kind_->setMinimumHeight(Ui::scaled(Ui::Metric::control));filters->addWidget(kind_);
 search_=new QLineEdit(body);search_->setObjectName("errorLogSearch");search_->setPlaceholderText("Search messages, files and destinations");search_->setAccessibleName("Search the error log");search_->setClearButtonEnabled(true);search_->setMinimumHeight(Ui::scaled(Ui::Metric::control));filters->addWidget(search_,1);
 openOnly_=new QCheckBox("Open problems only",body);openOnly_->setObjectName("errorLogOpenOnly");filters->addWidget(openOnly_);
 box->addLayout(filters);
 searchDelay_=new QTimer(this);searchDelay_->setSingleShot(true);searchDelay_->setInterval(250);
 connect(searchDelay_,&QTimer::timeout,this,[this]{offset_=0;load();});
 connect(search_,&QLineEdit::textEdited,searchDelay_,qOverload<>(&QTimer::start));
 connect(kind_,&QComboBox::activated,this,[this]{offset_=0;load();});
 connect(openOnly_,&QCheckBox::toggled,this,[this]{offset_=0;load();});
 // The table and, below it, the selected entry.
 auto* split=new QSplitter(Qt::Vertical,body);split->setObjectName("errorLogSplit");split->setChildrenCollapsible(false);box->addWidget(split,1);
 auto* listPane=new QWidget(split);auto* listBox=new QVBoxLayout(listPane);listBox->setContentsMargins(0,0,0,0);listBox->setSpacing(space(Space::s));
 table_=new QTreeWidget(listPane);table_->setObjectName("errorLogTable");table_->setAccessibleName("Error log entries, newest first");
 table_->setColumnCount(6);table_->setHeaderLabels({"Time","Action","Item","Destination","Message","Status"});table_->setRootIsDecorated(false);table_->setUniformRowHeights(true);
 table_->setSelectionMode(QAbstractItemView::SingleSelection);table_->setSelectionBehavior(QAbstractItemView::SelectRows);table_->setAllColumnsShowFocus(true);table_->setIconSize(QSize(Ui::scaled(32),Ui::scaled(32)));
 table_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);table_->setTextElideMode(Qt::ElideRight);table_->header()->setStretchLastSection(false);
 // Fixed widths that fit their usual text ("Yesterday 23:59", "Destination test"); the message takes the rest.
 for(const auto& [column,width]:QList<std::pair<int,int>>{{TimeColumn,128},{ActionColumn,136},{ItemColumn,200},{DestinationColumn,150},{StatusColumn,88}}){table_->header()->setSectionResizeMode(column,QHeaderView::Interactive);table_->setColumnWidth(column,Ui::scaled(width));}
 table_->header()->setSectionResizeMode(MessageColumn,QHeaderView::Stretch);table_->setMinimumHeight(Ui::scaled(160));
 listBox->addWidget(table_,1);
 empty_=new QLabel("No errors. When something goes wrong, it's listed here with what to do next.",listPane);empty_->setObjectName("errorLogEmpty");empty_->setWordWrap(true);empty_->setAlignment(Qt::AlignCenter);Ui::role(empty_,"muted");empty_->hide();listBox->addWidget(empty_);
 auto* pager=new QHBoxLayout;pager->setSpacing(space(Space::s));pageText_=new QLabel(listPane);pageText_->setObjectName("errorLogPageText");Ui::role(pageText_,"meta");pager->addWidget(pageText_,1);
 newer_=new QPushButton("Newer",listPane);newer_->setObjectName("errorLogNewer");older_=new QPushButton("Older",listPane);older_->setObjectName("errorLogOlder");pager->addWidget(newer_);pager->addWidget(older_);listBox->addLayout(pager);
 connect(newer_,&QPushButton::clicked,this,[this]{offset_=qMax(0,offset_-pageSize);load();});
 connect(older_,&QPushButton::clicked,this,[this]{if(offset_+pageSize<total_){offset_+=pageSize;load();}});
 // The selected entry: what happened, the recorded details and the next steps.
 auto* scroll=new QScrollArea(split);scroll->setObjectName("errorLogDetailsScroll");scroll->setWidgetResizable(true);scroll->setFrameShape(QFrame::NoFrame);scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);Ui::transparentView(scroll);
 details_=new QWidget(scroll);details_->setObjectName("errorLogDetails");scroll->setWidget(details_);auto* detailBox=new QVBoxLayout(details_);detailBox->setContentsMargins(0,space(Space::l),0,0);detailBox->setSpacing(space(Space::m));
 detailTitle_=new QLabel(details_);detailTitle_->setObjectName("errorLogDetailTitle");detailTitle_->setWordWrap(true);detailTitle_->setTextFormat(Qt::PlainText);Ui::role(detailTitle_,"sectionTitle");detailBox->addWidget(detailTitle_);
 detailMessage_=new QLabel(details_);detailMessage_->setObjectName("errorLogDetailMessage");detailMessage_->setWordWrap(true);detailMessage_->setTextFormat(Qt::PlainText);Ui::readOnlyText(detailMessage_);detailBox->addWidget(detailMessage_);
 resolution_=new QLabel(details_);resolution_->setObjectName("errorLogResolution");resolution_->setWordWrap(true);resolution_->setTextFormat(Qt::PlainText);Ui::role(resolution_,"success");resolution_->hide();detailBox->addWidget(resolution_);
 facts_=new QGridLayout;facts_->setHorizontalSpacing(space(Space::l));facts_->setVerticalSpacing(space(Space::xs));facts_->setColumnStretch(1,1);detailBox->addLayout(facts_);
 steps_=new QHBoxLayout;steps_->setSpacing(space(Space::s));detailBox->addLayout(steps_);
 for(const auto& step:stepOrder()){auto* button=new QPushButton(details_);button->setObjectName("errorLogStep_"+step);button->setProperty("step",step);button->hide();steps_->addWidget(button);connect(button,&QPushButton::clicked,this,[this,step]{runStep(step);});}
 steps_->addStretch(1);
 auto* feedbackRow=new QHBoxLayout;feedbackRow->setSpacing(space(Space::s));
 feedback_=new QLabel(details_);feedback_->setObjectName("errorLogFeedback");feedback_->setWordWrap(true);feedback_->setTextFormat(Qt::PlainText);feedback_->hide();feedbackRow->addWidget(feedback_,1);
 revealPackage_=new QPushButton("Show in folder",details_);revealPackage_->setObjectName("errorLogShowPackage");Ui::role(revealPackage_,"textAction");revealPackage_->hide();feedbackRow->addWidget(revealPackage_);detailBox->addLayout(feedbackRow);
 connect(revealPackage_,&QPushButton::clicked,this,[this]{if(packagePath_.isEmpty())return;session_.request("diagnostics.reveal",{{"path",packagePath_}},this,[g=QPointer<ErrorLogPage>(this)](Error e,QJsonObject){if(g&&e)g->feedback(e.message,true);});});
 technicalToggle_=new QToolButton(details_);technicalToggle_->setObjectName("errorLogTechnicalToggle");technicalToggle_->setText("Technical details");Ui::disclosure(technicalToggle_);technicalToggle_->setProperty("uiFlush",true);detailBox->addWidget(technicalToggle_,0,Qt::AlignLeft);
 technical_=new QPlainTextEdit(details_);technical_->setObjectName("errorLogTechnical");technical_->setReadOnly(true);technical_->setLineWrapMode(QPlainTextEdit::NoWrap);Ui::role(technical_,"diagnostics");technical_->setAccessibleName("Error log entry, as recorded");technical_->setMinimumHeight(Ui::scaled(140));technical_->hide();detailBox->addWidget(technical_);
 auto* copyEntry=new QPushButton("Copy details",details_);copyEntry->setObjectName("errorLogCopyDetails");copyEntry->hide();detailBox->addWidget(copyEntry,0,Qt::AlignLeft);
 connect(technicalToggle_,&QToolButton::toggled,this,[this,copyEntry](bool on){technical_->setVisible(on);copyEntry->setVisible(on);});
 connect(copyEntry,&QPushButton::clicked,this,[this]{QApplication::clipboard()->setText(technical_->toPlainText());feedback("Details copied.",false);});
 detailBox->addStretch(1);details_->hide();
 unlockPoll_=new QTimer(this);unlockPoll_->setInterval(500);
 connect(unlockPoll_,&QTimer::timeout,this,[this]{
  if(unlockPending_)return;unlockPending_=true;
  session_.request("setup.inspect",{{"id",unlockSetupId_}},this,[g=QPointer<ErrorLogPage>(this),setupId=unlockSetupId_](Error e,QJsonObject job){
   if(!g)return;g->unlockPending_=false;if(setupId!=g->unlockSetupId_)return;
   if(!e&&job["state"]=="running"){g->feedback(QStringLiteral("Waiting for the keyring…"),false);return;}
   g->unlockPoll_->stop();g->stepRunning_=false;if(!g->selected_.isEmpty())g->showDetails(g->selected_);
   const auto error=e?e:Error{job["error"].toObject()["code"].toString(),job["error"].toObject()["message"].toString(),{}};
   if(error)g->feedback(error.message,true);
   else g->feedback(QStringLiteral("Credentials loaded. Choose %1 to try again.").arg(ErrorLogPage::stepLabel("retry",g->unlockKind_)),false);
  });
 });
 // The selected entry gets the larger share: its buttons are what the page is for.
 split->setStretchFactor(0,2);split->setStretchFactor(1,3);split->setSizes({Ui::scaled(240),Ui::scaled(420)});
 connect(table_,&QTreeWidget::currentItemChanged,this,[this](QTreeWidgetItem* item){if(item)select(item->data(0,IdRole).toString());});
 connect(&session_,&UiSession::errorLogChanged,this,[this]{
  const int open=session_.errorLog()["unresolved"].toInt();if(open!=unresolved_){unresolved_=open;emit unresolvedChanged(open);}
  if(isVisible())refresh();else reloadPending_=true;});
}
void ErrorLogPage::showEvent(QShowEvent* event) {
 QWidget::showEvent(event);
 if(reloadPending_||lastRevision_.isEmpty()){reloadPending_=false;load();}
}
void ErrorLogPage::refresh() {load();}
QString ErrorLogPage::timeText(const QString& iso) const {
 const auto time=QDateTime::fromString(iso,Qt::ISODateWithMs).toLocalTime();if(!time.isValid())return iso;
 const QLocale locale;
 if(time.date()==QDate::currentDate())return "Today "+locale.toString(time.time(),QLocale::ShortFormat);
 if(time.date()==QDate::currentDate().addDays(-1))return "Yesterday "+locale.toString(time.time(),QLocale::ShortFormat);
 return locale.toString(time,QLocale::ShortFormat);
}
void ErrorLogPage::load(std::function<void(Error)> done) {
 QJsonObject params{{"limit",pageSize},{"offset",offset_}};
 if(!kind_->currentData().toString().isEmpty())params["kind"]=kind_->currentData().toString();
 if(!search_->text().trimmed().isEmpty())params["search"]=search_->text().trimmed().left(256);
 if(openOnly_->isChecked())params["resolved"]=false;
 loading_=true;
 session_.request("errors.list",params,this,[g=QPointer<ErrorLogPage>(this),done](Error e,QJsonObject page){
  if(!g)return;g->loading_=false;
  if(e){g->empty_->setText("Couldn't read the error log: "+e.message);g->empty_->show();g->table_->hide();if(done)done(e);return;}
  g->entries_=page["entries"].toArray();g->total_=page["total"].toInt();g->lastRevision_=page["revision"].toString();
  g->populate();if(done)done({});
 });
}
void ErrorLogPage::populate() {
 const auto keep=wanted_.isEmpty()?selected_.value("id").toString():wanted_;
 const QSignalBlocker block(table_);table_->clear();
 for(const auto& value:entries_){
  const auto entry=value.toObject();auto* item=new QTreeWidgetItem(table_);item->setData(0,IdRole,entry["id"].toString());
  item->setText(TimeColumn,timeText(entry["time"].toString()));item->setToolTip(TimeColumn,entry["time"].toString());
  item->setText(ActionColumn,kindLabel(entry["kind"].toString()));
  const auto name=entry["itemName"].toString();item->setText(ItemColumn,name);item->setToolTip(ItemColumn,name);
  const auto preview=entry["previewPath"].toString();
  if(!preview.isEmpty()){if(auto* pixmap=thumbnails_.object(preview))item->setIcon(ItemColumn,QIcon(*pixmap));else loadThumbnail(preview);}
  else if(!name.isEmpty())item->setIcon(ItemColumn,Ui::icon(entry["kind"]=="recording"?"video":"image"));
  item->setText(DestinationColumn,entry["destination"].toString());item->setToolTip(DestinationColumn,entry["destination"].toString());
  const int times=entry["occurrences"].toInt(1);
  item->setText(MessageColumn,entry["title"].toString()+(times>1?QStringLiteral(" (%1 times)").arg(times):QString{}));item->setToolTip(MessageColumn,entry["message"].toString());
  const bool resolved=entry["resolved"].toBool();item->setText(StatusColumn,resolved?QStringLiteral("Resolved"):QStringLiteral("Open"));
  item->setToolTip(StatusColumn,resolved?entry["resolution"].toString():QStringLiteral("Not resolved yet"));
  item->setData(StatusColumn,Qt::AccessibleTextRole,resolved?QStringLiteral("Resolved: ")+entry["resolution"].toString():QStringLiteral("Open"));
  item->setSizeHint(0,QSize(0,Ui::scaled(44)));
 }
 empty_->setText(search_->text().isEmpty()&&kind_->currentIndex()==0&&!openOnly_->isChecked()?QStringLiteral("No errors. When something goes wrong, it's listed here with what to do next."):QStringLiteral("No entries match these filters."));
 empty_->setVisible(entries_.isEmpty());table_->setVisible(!entries_.isEmpty());
 pageText_->setText(total_==0?QString{}:QStringLiteral("%1–%2 of %3 · %4 open").arg(offset_+1).arg(offset_+entries_.size()).arg(total_).arg(session_.errorLog()["unresolved"].toInt()));
 newer_->setEnabled(offset_>0);older_->setEnabled(offset_+pageSize<total_);newer_->setVisible(total_>pageSize);older_->setVisible(total_>pageSize);
 clear_->setEnabled(total_>0||session_.errorLog()["total"].toInt()>0);
 // A pending link is shown as the link; otherwise the page keeps its row after a reload.
 if(!wanted_.isEmpty())select(wanted_,Link);else if(!keep.isEmpty())select(keep,Reselect);
 else if(entries_.isEmpty()){selected_={};details_->hide();}
}
void ErrorLogPage::select(const QString& id,Selection kind) {
 for(int row=0;row<table_->topLevelItemCount();++row)if(table_->topLevelItem(row)->data(0,IdRole).toString()==id){const QSignalBlocker block(table_);table_->setCurrentItem(table_->topLevelItem(row));table_->scrollToItem(table_->topLevelItem(row));break;}
 // Each selection, by click, link or agent, supersedes the replies of the ones before it; the
 // page reselecting its row after a reload does not.
 const auto ticket=kind==User?++selection_:selection_;
 if(kind==Link&&wanted_!=id)return;
 for(const auto& value:entries_)if(value.toObject()["id"]==id){wanted_.clear();showDetails(value.toObject());return;}
 // Not on this page (an older entry, or one a filter hides): read it on its own.
 session_.request("errors.inspect",{{"id",id}},this,[g=QPointer<ErrorLogPage>(this),ticket,kind,id](Error e,QJsonObject entry){
  // A reload's own reselection never displaces a link that is still on its way.
  // A reload's reselection never displaces a link still on its way; the link's own reply shows it.
  if(!g||g->selection_!=ticket||(kind==Reselect&&!g->wanted_.isEmpty())||(kind==Link&&g->wanted_!=id))return;
  if(kind!=Reselect)g->wanted_.clear();
  if(e){g->selected_={};g->details_->hide();return;}
  g->showDetails(entry);
 });
}
void ErrorLogPage::showDetails(const QJsonObject& entry) {
 const bool changed=selected_.value("id")!=entry["id"];selected_=entry;
 if(changed){feedback({},false);revealPackage_->hide();packagePath_.clear();}
 const auto kind=entry["kind"].toString(),name=entry["itemName"].toString();
 detailTitle_->setText(kindLabel(kind)+(name.isEmpty()?QString{}:QStringLiteral(": ")+name));
 detailMessage_->setText(entry["message"].toString());
 const bool resolved=entry["resolved"].toBool();
 resolution_->setText(resolved?QStringLiteral("Resolved %1: %2").arg(timeText(entry["resolvedAt"].toString()),entry["resolution"].toString()):QString{});resolution_->setVisible(resolved);
 while(auto* item=facts_->takeAt(0)){delete item->widget();delete item;}
 QList<std::pair<QString,QString>> facts;
 const int times=entry["occurrences"].toInt(1);
 facts.append({"When",timeText(entry["time"].toString())+(times>1?QStringLiteral(" (%1 times since %2)").arg(times).arg(timeText(entry["firstTime"].toString())):QString{})});
 facts.append({"Action",kindLabel(kind)});
 if(!name.isEmpty())facts.append({"Item",name+(entry.contains("itemPresent")&&!entry["itemPresent"].toBool()?QStringLiteral(" (no longer in History)"):QString{})});
 if(!entry["destination"].toString().isEmpty())facts.append({"Destination",entry["destination"].toString()});
 facts.append({"Error code",entry["code"].toString()});
 const auto details=entry["details"].toObject();QSet<QString> shown;
 for(const auto& [key,label]:detailFields()){if(!details.contains(key)||shown.contains(label))continue;const auto text=detailValue(key,details[key]);if(text.isEmpty())continue;shown.insert(label);facts.append({label,text});}
 for(const auto& [key,label]:QList<std::pair<QString,QString>>{{"operationId","Operation ID"},{"historyId","Item ID"},{"deletionId","Deletion ID"},{"sharingId","Link change ID"},{"setupId","Setup ID"}})
  if(!entry[key].toString().isEmpty())facts.append({label,entry[key].toString()});
 int row=0;for(const auto& [label,value]:facts){
  auto* caption=new QLabel(label,details_);Ui::role(caption,"meta");caption->setAlignment(Qt::AlignLeft|Qt::AlignTop);
  auto* text=new QLabel(value,details_);text->setTextFormat(Qt::PlainText);text->setWordWrap(true);Ui::readOnlyText(text);text->setObjectName("errorLogFact_"+QString(label).remove(' '));
  if(label.endsWith("ID")||label=="Error code")Ui::role(text,"mono");
  facts_->addWidget(caption,row,0);facts_->addWidget(text,row,1);++row;
 }
 const auto steps=entry["nextSteps"].toArray();
 // A resolved problem keeps the steps that look at it, not those that fix it.
 const auto offered=[&steps,resolved](const QString& step){return steps.contains(QJsonValue(step))&&!(resolved&&(step=="unlock_credentials"||step=="retry"||step=="remove_from_history"));};
 QString first;for(const auto& step:stepOrder())if(offered(step)){first=step;break;}
 for(int index=0;index<steps_->count();++index){
  auto* button=qobject_cast<QPushButton*>(steps_->itemAt(index)->widget());if(!button)continue;
  const auto step=button->property("step").toString();
  button->setText(stepLabel(step,kind));button->setAccessibleName(stepLabel(step,kind));button->setVisible(offered(step));
  const bool absent=(step=="show_in_history"||step=="remove_from_history")&&entry.contains("itemPresent")&&!entry["itemPresent"].toBool();
  button->setEnabled(!stepRunning_&&!absent);button->setToolTip(absent?QStringLiteral("This item is no longer in History."):QString{});
  Ui::role(button,step==first?"primary":"secondary");
 }
 technical_->setPlainText(QString::fromUtf8(QJsonDocument(entry).toJson(QJsonDocument::Indented)));
 details_->show();
}
void ErrorLogPage::feedback(const QString& text,bool error) {Ui::feedback(feedback_,text,error);feedback_->setVisible(!text.isEmpty());}
void ErrorLogPage::runStep(const QString& step,std::function<void(Error,QJsonObject)> done) {
 const auto entry=selected_;const auto id=entry["id"].toString();
 const auto finish=[done](Error e,QJsonObject r){if(done)done(e,r);};
 if(id.isEmpty()||!entry["nextSteps"].toArray().contains(QJsonValue(step))){finish({"error_action_unavailable","The selected entry doesn't offer that step.",{}},{});return;}
 if(step=="open_destination"){emit destinationRequested(entry["profileId"].toString());finish({},{{"step",step},{"profileId",entry["profileId"]}});return;}
 if(step=="show_in_history"){emit historyRequested(entry["historyId"].toString());finish({},{{"step",step},{"historyId",entry["historyId"]}});return;}
 if(step=="remove_from_history"){openRemove(entry,finish);return;}
 stepRunning_=true;showDetails(entry);
 feedback(step=="unlock_credentials"?QStringLiteral("Unlocking saved credentials…"):step=="save_diagnostics"?QStringLiteral("Saving diagnostics for this operation…"):QStringLiteral("Starting…"),false);
 session_.request("errors.act",{{"id",id},{"action",step}},this,[g=QPointer<ErrorLogPage>(this),step,id,finish,kind=entry["kind"].toString()](Error e,QJsonObject answer){
  if(!g){finish(e,answer);return;}
  g->stepRunning_=false;const auto result=answer["result"].toObject();
  if(g->selected_.value("id")==id)g->showDetails(g->selected_);
  if(e){g->feedback(e.message,true);finish(e,answer);return;}
  if(step=="unlock_credentials"){
   // A destination's credentials come back as a setup job; an item's at once.
   const auto setupId=result["setupId"].toString();
   if(setupId.isEmpty()){g->feedback(QStringLiteral("Credentials loaded. Choose %1 to try again.").arg(ErrorLogPage::stepLabel("retry",kind)),false);finish({},answer);return;}
   // Followed by the page's own timer until the job ends; the keyring may take its time.
   g->unlockSetupId_=setupId;g->unlockKind_=kind;g->stepRunning_=true;if(g->selected_.value("id")==id)g->showDetails(g->selected_);g->unlockPoll_->start();finish({},answer);return;
  }
  if(step=="save_diagnostics"){
   g->packagePath_=result["path"].toString();g->revealPackage_->setVisible(!g->packagePath_.isEmpty());
   g->feedback("Diagnostics saved to "+QDir::toNativeSeparators(g->packagePath_)+". Nothing was uploaded.",false);finish({},answer);return;
  }
  g->feedback(kind=="deletion"?QStringLiteral("Deletion retried. This entry shows Resolved once the file is deleted."):kind=="destination_test"?QStringLiteral("Test connection started."):QStringLiteral("Retry started. This entry shows Resolved once it works."),false);
  finish({},answer);
 });
}
// Remove from History only (#1759): History's own deletion dialog for the file, from a fresh
// delete.prepare, so the wording, the button and its refusal are the ones History shows.
void ErrorLogPage::openRemove(const QJsonObject& entry,std::function<void(Error,QJsonObject)> done) {
 if(removeConfirm_){done({"ui_confirmation_open","A Remove from History only confirmation is already open. Confirm or cancel it first.",{}},{});return;}
 const auto historyId=entry["historyId"].toString();
 stepRunning_=true;showDetails(entry);feedback({},false);
 session_.request("delete.prepare",{{"ids",QJsonArray{historyId}}},this,[g=QPointer<ErrorLogPage>(this),entry,historyId,done](Error e,QJsonObject preview){
  if(!g){done(e,{});return;}
  g->stepRunning_=false;if(g->selected_.value("id")==entry["id"])g->showDetails(g->selected_);
  if(e){g->feedback(e.message,true);done(e,{});return;}
  // Added back for the same place, the file can be deleted there: History offers Delete, not this.
  if(!preview["targets"].toArray().first().toObject()["destinationRemoved"].toBool()){
   const Error back{"destination_not_removed","This file's destination is in Settings again, so put.gd can delete it there. Delete it from History instead.",{{"historyId",historyId}}};
   g->feedback(back.message,true);done(back,{});return;
  }
  auto* dialog=HistoryWindow::deletionConfirmation(g,preview,{{historyId,entry["itemName"].toString()}},g->session_.document()["profiles"].toArray());
  dialog->setObjectName("errorLogRemoveConfirm");g->removeConfirm_=dialog;
  // What an agent's refused confirmRemove reports, as delete.prepare said it.
  const auto target=preview["targets"].toArray().first().toObject();
  dialog->setProperty("historyId",historyId);dialog->setProperty("retainedBy",target["retainedBy"].toString());
  connect(dialog,&QDialog::finished,g,[g,dialog,entry](int result){
   dialog->deleteLater();if(!g)return;
   // Closed now: the reply's state says so, though the dialog is deleted later.
   g->removeConfirm_=nullptr;
   g->finishRemove(result==HistoryWindow::forgetResult,entry,std::exchange(g->removeDone_,{}));
  });
  dialog->open();done({},{});
 });
}
void ErrorLogPage::finishRemove(bool accepted,const QJsonObject& entry,std::function<void(Error,QJsonObject)> done) {
 const auto finish=[done](Error e,QJsonObject r){if(done)done(e,r);};
 if(!accepted){finish({},{});return;}
 const auto id=entry["id"].toString();
 stepRunning_=true;if(selected_.value("id")==id)showDetails(selected_);feedback(QStringLiteral("Removing from History…"),false);
 session_.request("errors.act",{{"id",id},{"action","remove_from_history"}},this,[g=QPointer<ErrorLogPage>(this),id,finish](Error e,QJsonObject answer){
  if(!g){finish(e,answer);return;}
  g->stepRunning_=false;if(g->selected_.value("id")==id)g->showDetails(g->selected_);
  g->feedback(e?e.message:QStringLiteral("Removed from History. The file stays where it was uploaded."),bool(e));
  finish(e,answer);
 });
}
void ErrorLogPage::openClear(bool resolvedOnly) {
 if(confirm_)return;
 clearResolvedOnly_=resolvedOnly;
 const int count=resolvedOnly?qMax(0,session_.errorLog()["total"].toInt()-session_.errorLog()["unresolved"].toInt()):session_.errorLog()["total"].toInt();
 const auto text=resolvedOnly?QStringLiteral("Remove the %1 resolved %2 from the error log? Open problems stay. History, uploads and log files are untouched.").arg(count).arg(count==1?"entry":"entries")
                             :QStringLiteral("Remove all %1 %2 from the error log? History, uploads and log files are untouched.").arg(count).arg(count==1?"entry":"entries");
 confirm_=Ui::confirm(this,"Clear the error log",text,resolvedOnly?"Clear resolved":"Clear all","Cancel",true);confirm_->setObjectName("errorLogClearConfirm");
 connect(confirm_,&QMessageBox::finished,this,[this](int result){finishClear(result==QMessageBox::Yes);});
 confirm_->open();
}
void ErrorLogPage::finishClear(bool accepted,std::function<void(Error,QJsonObject)> done) {
 if(confirm_){disconnect(confirm_,nullptr,this,nullptr);confirm_->hide();confirm_->deleteLater();confirm_=nullptr;}
 if(!accepted){if(done)done({},{{"cleared",false}});return;}
 QJsonObject params;if(clearResolvedOnly_)params["resolvedOnly"]=true;
 session_.request("errors.clear",params,this,[g=QPointer<ErrorLogPage>(this),done](Error e,QJsonObject result){
  if(!g)return;
  if(e){g->feedback(e.message,true);}else{g->offset_=0;g->selected_={};g->details_->hide();g->load();}
  if(done)done(e,result);
 });
}
void ErrorLogPage::loadThumbnail(const QString& path) {
 if(thumbnailRequested_.contains(path)||thumbnailRequested_.size()>=64)return;
 thumbnailRequested_.insert(path);auto image=std::make_shared<QImage>();const int side=Ui::scaled(32)*2;
 session_.background([path,image,side](Error&){
  QFileInfo file(path);if(!file.isFile()||file.size()>8*1024*1024)return QJsonObject{};
  QImageReader reader(path);preparePreviewReader(reader,path);const auto size=reader.size();if(!size.isValid()||qint64(size.width())*size.height()>16000000)return QJsonObject{};
  reader.setScaledSize(size.scaled(side,side,Qt::KeepAspectRatioByExpanding));*image=reader.read();return QJsonObject{};
 },this,[g=QPointer<ErrorLogPage>(this),path,image](Error,QJsonObject){
  if(!g||image->isNull())return;
  const int side=Ui::scaled(32);auto pixmap=QPixmap::fromImage(image->copy(QRect(QPoint((image->width()-qMin(image->width(),image->height()))/2,(image->height()-qMin(image->width(),image->height()))/2),QSize(qMin(image->width(),image->height()),qMin(image->width(),image->height()))))).scaled(side*2,side*2,Qt::KeepAspectRatio,Qt::SmoothTransformation);
  pixmap.setDevicePixelRatio(2);g->thumbnails_.insert(path,new QPixmap(pixmap));
  for(int row=0;row<g->table_->topLevelItemCount();++row){auto* item=g->table_->topLevelItem(row);for(const auto& value:std::as_const(g->entries_))if(value.toObject()["id"].toString()==item->data(0,IdRole).toString()&&value.toObject()["previewPath"].toString()==path)item->setIcon(ItemColumn,QIcon(pixmap));}
 });
}
void ErrorLogPage::showEntry(const QJsonObject& target) {
 // Open from History's link: the entry itself, on a page with no filters hiding it.
 const QSignalBlocker kinds(kind_),resolved(openOnly_);kind_->setCurrentIndex(0);openOnly_->setChecked(false);search_->clear();offset_=0;
 const auto id=target["id"].toString(),historyId=target["historyId"].toString(),operationId=target["operationId"].toString(),kind=target["kind"].toString();
 // Every link supersedes the lookups of the ones before it.
 ++selection_;
 if(!id.isEmpty()){wanted_=id;load();return;}
 if(historyId.isEmpty()&&operationId.isEmpty()){wanted_.clear();selected_={};load([this](Error e){if(!e&&!entries_.isEmpty())select(entries_.first().toObject()["id"].toString());});return;}
 // An agent's selection still pending is superseded too: a reload meanwhile keeps the current row.
 wanted_.clear();
 QJsonObject params{{"limit",1}};if(!historyId.isEmpty())params["historyId"]=historyId;if(!operationId.isEmpty())params["operationId"]=operationId;if(!kind.isEmpty())params["kind"]=kind;
 const auto ticket=++selection_;
 session_.request("errors.list",params,this,[g=QPointer<ErrorLogPage>(this),ticket](Error e,QJsonObject page){
  if(!g||g->selection_!=ticket)return;const auto entries=page["entries"].toArray();
  g->wanted_=e||entries.isEmpty()?QString{}:entries.first().toObject()["id"].toString();g->load();
 });
}
// The open Remove from History only confirmation, as its labels and buttons read; empty when none.
QJsonObject ErrorLogPage::removeConfirmation() const {
 if(!removeConfirm_)return {};
 const auto text=[this](const char* name){auto* label=removeConfirm_->findChild<QLabel*>(QLatin1String(name));return label?label->text():QString{};};
 return {{"open",true},{"heading",text("deleteConfirmationHeading")},{"text",text("deleteConfirmationText")},{"canRemove",bool(removeConfirm_->findChild<QPushButton*>("forgetHistoryRecords"))}};
}
QJsonObject ErrorLogPage::state() const {
 QJsonArray rows;
 for(const auto& value:entries_){const auto entry=value.toObject();rows.append(QJsonObject{{"id",entry["id"]},{"time",entry["time"]},{"kind",entry["kind"]},{"action",kindLabel(entry["kind"].toString())},
  {"itemName",entry["itemName"]},{"thumbnail",!entry["previewPath"].toString().isEmpty()},{"destination",entry["destination"]},{"message",entry["title"]},{"resolved",entry["resolved"]},{"occurrences",entry["occurrences"]}});}
 QJsonArray buttons;
 for(int index=0;index<steps_->count();++index)if(auto* button=qobject_cast<QPushButton*>(steps_->itemAt(index)->widget());button&&!button->isHidden())
  buttons.append(QJsonObject{{"step",button->property("step").toString()},{"label",button->text()},{"enabled",button->isEnabled()}});
 QJsonObject result{{"visible",isVisible()},{"filter",QJsonObject{{"kind",kind_->currentData().toString()},{"search",search_->text()},{"showResolved",!openOnly_->isChecked()}}},
  {"page",QJsonObject{{"offset",offset_},{"total",total_},{"size",pageSize}}},{"rows",rows},{"loading",loading_},{"confirming",bool(confirm_)},{"removeConfirmation",removeConfirmation()},
  {"feedback",feedback_->isHidden()?QString{}:feedback_->text()},{"feedbackError",feedback_->property("feedbackTone").toString()=="error"}};
 if(!selected_.isEmpty()&&details_->isVisible())result["selected"]=QJsonObject{{"entry",selected_},{"title",detailTitle_->text()},{"buttons",buttons},{"resolution",resolution_->isHidden()?QString{}:resolution_->text()}};
 if(!packagePath_.isEmpty())result["package"]=packagePath_;
 return result;
}
void ErrorLogPage::command(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply) {
 const auto action=params["action"].toString();
 const auto answer=[g=QPointer<ErrorLogPage>(this),reply](Error e,QJsonObject){if(!g){reply({"ui_view_closed","Open Settings before driving it.",{}},{});return;}reply(e,g->state());};
 if(action=="inspect"){reply({},state());return;}
 if(action=="refresh"){load([answer](Error e){answer(e,{});});return;}
 if(action=="select"){
  const auto id=params["id"].toString();
  bool onPage=false;for(const auto& value:std::as_const(entries_))onPage|=value.toObject()["id"]==id;
  if(onPage){select(id);reply({},state());return;}
  // Pending like a link (wanted_): a reload meanwhile keeps it instead of reselecting the old row.
  const auto ticket=++selection_;wanted_=id;
  session_.request("errors.inspect",{{"id",id}},this,[g=QPointer<ErrorLogPage>(this),reply,answer,ticket,id](Error e,QJsonObject entry){if(!g)return;const bool current=g->selection_==ticket&&g->wanted_==id;if(current)g->wanted_.clear();if(e){reply(e,{});return;}if(current)g->showDetails(entry);answer({},{});});return;
 }
 // A step answers once it has started; its outcome shows in feedback (inspect), as on screen.
 if(action=="step"){
  const auto step=params["step"].toString();
  if(stepRunning_){reply({"error_step_running","A step for this entry is still running; inspect until it ends.",{}},state());return;}
  if(selected_.isEmpty()||!selected_["nextSteps"].toArray().contains(QJsonValue(step))){reply({"error_action_unavailable","The selected entry doesn't offer that step.",{}},state());return;}
  // Remove from History only answers once its confirmation is open (or refused).
  if(step=="remove_from_history"){runStep(step,[answer](Error e,QJsonObject r){answer(e,r);});return;}
  runStep(step);reply({},state());return;
 }
 if(action=="setFilter"){
  if(params.contains("kind")){const auto index=kind_->findData(params["kind"].toString());kind_->setCurrentIndex(qMax(0,index));}
  if(params.contains("search"))search_->setText(params["search"].toString());
  if(params.contains("showResolved")){const QSignalBlocker block(openOnly_);openOnly_->setChecked(!params["showResolved"].toBool());}
  offset_=0;load([answer](Error e){answer(e,{});});return;
 }
 if(action=="nextPage"||action=="previousPage"){
  if(action=="nextPage"&&offset_+pageSize<total_)offset_+=pageSize;else if(action=="previousPage")offset_=qMax(0,offset_-pageSize);
  load([answer](Error e){answer(e,{});});return;
 }
 if(action=="clear"){if(confirm_){reply({"ui_confirmation_open","A Clear confirmation is already open; confirm or cancel it first.",{}},state());return;}openClear(params["resolvedOnly"].toBool());reply({},state());return;}
 if(action=="confirmRemove"||action=="cancelRemove"){
  if(!removeConfirm_){reply({"ui_no_confirmation","No Remove from History only confirmation is open.",{}},{});return;}
  auto* forget=removeConfirm_->findChild<QPushButton*>("forgetHistoryRecords");
  // Without the button the record stays, and the dialog says why, as on screen.
  if(action=="confirmRemove"&&!forget){QJsonObject details{{"historyId",removeConfirm_->property("historyId").toString()}};if(const auto by=removeConfirm_->property("retainedBy").toString();!by.isEmpty())details["retainedBy"]=by;reply({"history_record_retained",removeConfirmation()["text"].toString(),details},state());return;}
  removeDone_=[answer](Error e,QJsonObject r){answer(e,r);};
  if(action=="confirmRemove")forget->click();else removeConfirm_->reject();
  return;
 }
 if(action=="confirmClear"||action=="cancelClear"){
  if(!confirm_){reply({"ui_no_confirmation","No Clear confirmation is open.",{}},{});return;}
  finishClear(action=="confirmClear",[answer](Error e,QJsonObject){answer(e,{});});return;
 }
 reply({"ui_command_unknown","The error log does not support that command.",{}},{});
}
}
