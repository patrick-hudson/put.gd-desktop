// History actions that remove or cancel something: each reaches exactly the
// confirmed targets. Layout, copy and view states are left to acceptance and
// the rendered catalog.
#include <QDateTime>
#include <QApplication>
#include <QDialog>
#include <QLabel>
#include <QListView>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSignalSpy>
#include <QTimer>
#include "ui/HistoryModel.h"
#include "ui/HistoryWindow.h"
#include "fixtures/UiSessionFixture.h"
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
// A destination with a separate link step whose create reply can be lost (#1496).
class LinkStepFixture final:public StorageProvider {
public:
 OpaqueProvider storage;std::atomic_int shares{0},inspections{0};std::atomic_bool loseReply{true};
 ProviderCapabilities capabilities(const Destination& d)const override{auto c=storage.capabilities(d);c.sharing=true;c.privateUploads=true;return c;}
 QString defaultPublicationMode(const Destination&)const override{return "share";}
 QString serializationKey(const Destination& d,const QJsonObject& o,const QString& n)const override{return storage.serializationKey(d,o,n);}
 Error validateSettings(const QJsonObject& s)const override{return storage.validateSettings(s);}
 void checkName(const Destination& d,const QString& n,std::function<void(NameCheck)> cb,std::shared_ptr<std::atomic_bool> c={})override{storage.checkName(d,n,cb,c);}
 void upload(const UploadRequest& r,std::function<void(UploadResult)> cb)override{storage.upload(r,[cb](UploadResult result){result.receipt.directUrl.clear();cb(result);});}
 void stat(const StorageReceipt& r,std::function<void(StatResult)> cb)override{storage.stat(r,cb);}
 void reconcile(const UploadRequest& r,std::function<void(StatResult)> cb)override{storage.reconcile(r,cb);}
 void remove(const DeleteRequest& r,DurableCheckpoint d,std::function<void(DeleteResult)> cb)override{storage.remove(r,d,cb);}
 void share(const ShareRequest& r,DurableCheckpoint durable,std::function<void(ShareResult)> cb)override{
  auto receipt=r.receipt;if(r.action=="inspect"){++inspections;receipt.directUrl.clear();cb({"private",receipt,{},{}});return;}
  durable({{"schemaVersion",1},{"phase","intent"}},[this,r,receipt,cb](Error e)mutable{if(e){cb({"failed",r.receipt,{},e});return;}++shares;if(r.action=="create")receipt.directUrl="https://example.invalid/finished#secret";cb({loseReply.load()?"unknown":"shared",receipt,{},{}});});
 }
};
class HistoryGui:public QObject {
 Q_OBJECT
 // The remote-deletion confirmation, as an agent or test finds it.
 static QDialog* deletionDialog(){for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QDialog*>(widget);dialog&&dialog->isVisible()&&dialog->objectName()=="confirmAction"&&dialog->windowTitle()=="Delete from destination")return dialog;return nullptr;}
 static void selectRow(QListView* view,int row){view->selectionModel()->select(view->model()->index(row,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);view->setCurrentIndex(view->model()->index(row,0));}
private slots:

 // Cancel all cancels only the uploads that were running when it was confirmed.
 void cancelAllCapturesExactIdsAndRequiresConfirmation(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();fixture.server.setUiCommand([&](const QString& name,const QJsonObject& p,JsonReply reply){QCOMPARE(name,QString("history"));window.command(p,std::move(reply));});
  auto add=[&](const QString& id){HistoryStore ledger(operations.databasePath());Operation op;op.id=id;op.created=QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);op.restore(OperationState::Running);op.phase="uploading";op.settings=settings.snapshot();QVERIFY(!ledger.saveOperation(op));window.refresh();};add("first-active");add("second-active");PUTGD_TRY_COMPARE(window.historyState()["activeOperationCount"].toInt(),2);
  QStringList cancellations;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& p){if(method=="operation.cancel")cancellations.append(p["id"].toString());};auto drive=[&](const QString& action){bool done=false;Error error;fixture.client.request("ui.history",{{"action",action}},&window,[&](Error e,QJsonObject){error=e;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.message));};
  drive("cancelAll");QVERIFY(cancellations.isEmpty());drive("keepUploading");QVERIFY(cancellations.isEmpty());drive("cancelAll");add("new-active");PUTGD_TRY_COMPARE(window.historyState()["activeOperationCount"].toInt(),3);drive("confirmCancelAll");PUTGD_TRY_COMPARE(cancellations.size(),2);cancellations.sort();QCOMPARE(cancellations,QStringList({"first-active","second-active"}));
 }
 // Deleting a selection names exactly the selected files and deletes only those.
 void multiSelectionOffersBatchActionsWithExactIds(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  for(int i=0;i<3;++i){operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->uploads.load(),i+1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1100,700);window.show();
  auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),3);
  const QStringList chosenNames{model->record(0)["name"].toString(),model->record(1)["name"].toString()};const QStringList chosen{model->record(0)["id"].toString(),model->record(1)["id"].toString()};const auto kept=model->record(2)["id"].toString();
  list->selectionModel()->select(QItemSelection(model->index(0),model->index(1)),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->selectionModel()->setCurrentIndex(model->index(1),QItemSelectionModel::NoUpdate);PUTGD_TRY_VERIFY(window.findChild<QPushButton*>("historyDeleteSelected")->isVisible());
  QString shown,details;QTimer accept;accept.setInterval(10);connect(&accept,&QTimer::timeout,this,[&]{if(auto* dialog=deletionDialog()){for(auto* label:dialog->findChildren<QLabel*>())shown+=label->text()+"\n";details=dialog->findChild<QPlainTextEdit*>("deleteConfirmationDetails")->toPlainText();dialog->findChild<QPushButton*>("confirmActionButton")->click();}});accept.start();
  window.findChild<QPushButton*>("historyDeleteSelected")->click();PUTGD_TRY_COMPARE(provider->deletes.load(),2);accept.stop();
  for(const auto& name:chosenNames){QVERIFY2(shown.contains(name),qPrintable(shown));}
  for(const auto& id:chosen){QVERIFY(details.contains(id));}
  QVERIFY(!details.contains(kept));
  auto remote=[&](const QString& id){for(int row=0;row<model->rowCount();++row)if(model->record(row)["id"]==id)return model->record(row)["remoteStatus"].toString();return QString{};};
  PUTGD_TRY_VERIFY(remote(chosen[0])=="removed"&&remote(chosen[1])=="removed");QVERIFY(remote(kept)!="removed");PUTGD_COMPARE_STEADY(provider->deletes.load(),2,50);
 }
 // Delete prepares exactly the selected files, cancelling sends no delete, and
 // a selected row that leaves the list on a reload is never acted on.
 void closedDetailsPutTheSelectionBarInTheHeader(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  for(int i=0;i<3;++i){operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->uploads.load(),i+1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1500,900);window.show();auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),3);
  fixture.server.setUiCommand([&](const QString&,const QJsonObject& params,JsonReply reply){window.command(params,std::move(reply));});
  auto drive=[&](const QJsonObject& params){Error error;QJsonObject result;bool done=false;fixture.client.request("ui.history",params,&window,[&](Error e,QJsonObject r){error=e;result=r;done=true;});if(!putgd::test::waitFor([&]{return done;},5000))error={"timeout","ui.history did not answer",{}};return std::pair{error,result};};
  QVERIFY(!drive({{"action","toggleDetails"}}).first);
  const QStringList chosen{model->record(0)["id"].toString(),model->record(1)["id"].toString()};
  QVERIFY(!drive({{"action","select"},{"ids",QJsonArray::fromStringList(chosen)}}).first);PUTGD_TRY_VERIFY(window.findChild<QPushButton*>("historyDeleteSelected")->isVisible());
  QStringList prepared;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& params){if(method=="delete.prepare")for(const auto& id:params["ids"].toArray())prepared<<id.toString();};
  bool seenOpen=false;QTimer cancel;cancel.setInterval(10);connect(&cancel,&QTimer::timeout,this,[&]{if(deletionDialog()){seenOpen=true;window.command({{"action","cancelConfirmation"}},[](Error e,QJsonObject){QVERIFY(!e);});}});cancel.start();
  window.findChild<QPushButton*>("historyDeleteSelected")->click();PUTGD_TRY_VERIFY(seenOpen);PUTGD_TRY_VERIFY(!deletionDialog());cancel.stop();prepared.sort();auto expected=chosen;expected.sort();QCOMPARE(prepared,expected);QCOMPARE(provider->deletes.load(),0);
  // A reload that drops one selected row leaves the other selected and Delete refuses.
  {HistoryStore ledger(operations.databasePath());QVERIFY(!ledger.expireBefore(QDateTime::fromString(model->record(1)["created"].toString(),Qt::ISODateWithMs).addMSecs(1).toUTC().toString(Qt::ISODateWithMs)));}window.refresh();
  PUTGD_TRY_COMPARE(model->rowCount(),1);PUTGD_TRY_COMPARE(window.historyState()["selectedRows"].toInt(),1);QCOMPARE(window.historyState()["selectedIds"].toArray(),(QJsonArray{chosen[0]}));
  prepared.clear();QCOMPARE(drive({{"action","deleteSelected"}}).first.code,QString("history_selection_invalid"));PUTGD_VERIFY_STEADY(prepared.isEmpty()&&provider->deletes.load()==0,50);
 }
 // Retrying a failed deletion acts only on the selected row's deletion.
 void deletionRecoveryFollowsOnlyTheSelectedTarget(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  for(int i=0;i<2;++i){operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->uploads.load(),i+1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1400,800);window.show();auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),2);const auto file=model->record(0)["records"].toArray().first().toObject();
  {HistoryStore ledger(operations.databasePath());QVERIFY(!ledger.saveDeletion({{"id","synthetic-deletion"},{"state","completed"},{"items",QJsonArray{QJsonObject{{"historyId",file["id"]},{"receipt",file["receipt"]},{"status","failed"},{"error",QJsonObject{{"code","synthetic_failure"},{"message","Synthetic removal failure"}}}}}}}));}window.refresh();PUTGD_TRY_COMPARE(model->record(0)["deleteOperationId"].toString(),QString("synthetic-deletion"));selectRow(list,0);
  auto* retry=window.findChild<QPushButton*>("retryHistoryDeletion");PUTGD_TRY_VERIFY(retry->isVisible());QCOMPARE(window.historyState()["deletion"].toObject()["historyId"],file["id"]);
  selectRow(list,1);PUTGD_TRY_VERIFY(!retry->isVisible());QVERIFY(window.historyState()["deletion"].toObject().isEmpty());selectRow(list,0);PUTGD_TRY_VERIFY(retry->isVisible());retry->click();PUTGD_TRY_COMPARE(provider->deletes.load(),1);PUTGD_TRY_VERIFY(!retry->isVisible());QCOMPARE(provider->uploads.load(),2);
 }
 // A Proton Drive target carrying S3 effects cannot be confirmed.
 void deletionConfirmationRejectsStaleProtonEffects(){
  QWidget parent;parent.resize(1060,680);parent.show();QJsonArray targets;QHash<QString,QString> names;
  names["0"]="Synthetic.png";targets.append(QJsonObject{{"historyId","0"},{"effect","Deletes exactly this object version. Older versions and other objects in the bucket remain."},{"receipt",QJsonObject{{"destination",QJsonObject{{"providerId","proton-drive"},{"settings",QJsonObject{{"folderName","put.gd"}}}}},{"objectReference",QJsonObject{{"key","stale-s3-key"},{"versionId","stale-s3-version"},{"nodeUid","synthetic-node"}}}}}});
  std::unique_ptr<QDialog> dialog(HistoryWindow::deletionConfirmation(&parent,{{"targets",targets}},names,{}));dialog->show();QCoreApplication::processEvents();QVERIFY(!dialog->findChild<QPushButton*>("confirmActionButton"));
  QSignalSpy accepted(dialog.get(),&QDialog::accepted);dialog->findChild<QPushButton*>("confirmCancel")->click();QVERIFY(!dialog->isVisible());QCOMPARE(accepted.count(),0);
 }
 // A link whose object was removed is never offered for copying.
 // An uploaded file whose link was not confirmed is finished by one click: the link-only
 // retry for exactly its operation, which makes the link once and uploads nothing (#1496).
 void unconfirmedLinkIsFinishedInOneClick(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<LinkStepFixture>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->shares.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  // As after a crash in the link step: the operation is interrupted, not partial (m74 acceptance, #1512).
  {HistoryStore ledger(operations.databasePath());auto op=ledger.operation(ledger.recent(1).first().toObject()["id"].toString());QCOMPARE(op.stateName(),QString("partial"));op.restore(OperationState::Interrupted);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1100,700);window.show();fixture.server.setUiCommand([&](const QString& name,const QJsonObject& p,JsonReply reply){QCOMPARE(name,QString("history"));window.command(p,std::move(reply));});
  auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),1);
  const auto row=model->record(0);const auto itemId=row["id"].toString(),operationId=row["operationId"].toString();
  QCOMPARE(model->index(0).data(HistoryModel::PrimaryActionRole).toString(),QString("Finish share link"));QCOMPARE(HistoryModel::statusText(row),QString("Share link not finished"));
  PUTGD_TRY_COMPARE(window.historyState()["finishableIds"].toArray(),QJsonArray{itemId});
  // The pane keeps the state when the file has no local copy (m74 acceptance, #1512).
  selectRow(list,0);auto paneSays=[&](const QString& text){for(auto* label:window.findChildren<QLabel*>())if(label->isVisible()&&label->text().contains(text))return true;return false;};PUTGD_TRY_VERIFY(paneSays("Share link not finished. No local copy"));
  QJsonObject retry;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& p){if(method=="operation.retry")retry=p;};provider->loseReply=false;
  auto drive=[&](const QJsonObject& p){bool done=false;Error error;fixture.client.request("ui.history",p,&window,[&](Error e,QJsonObject){error=e;done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.message));};
  drive({{"action","select"},{"ids",QJsonArray{itemId}}});drive({{"action","finishShareLink"}});
  PUTGD_TRY_COMPARE(retry["scope"].toString(),QString("sharing"));QCOMPARE(retry["id"].toString(),operationId);
  PUTGD_TRY_COMPARE(provider->shares.load(),2);PUTGD_TRY_VERIFY(model->linkAvailable(0));PUTGD_TRY_VERIFY(window.historyState()["finishableIds"].toArray().isEmpty());
  QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(provider->inspections.load(),1);
 }
 // Finish share link is offered only when the retry would do nothing but links; the
 // operation's own items count, so a file hidden by a filter or page still blocks it.
 void finishShareLinkNeedsEveryUploadDone(){
  const QJsonObject file{{"id","file-a"},{"operationId","op"},{"uploadStatus","uploaded"},{"shareStatus","unknown"},{"shareIntent","create"},{"linkFinishable",true},{"receipt",QJsonObject{}}};
  auto row=[](QJsonObject first,const QJsonArray& files,const QJsonObject& operation={}){first["records"]=files;if(!operation.isEmpty())first["operation"]=operation;return first;};
  QCOMPARE(HistoryModel::finishShareLabel(row(file,{file})),QString("Finish share link"));
  auto second=file;second["id"]="file-b";QCOMPARE(HistoryModel::finishShareLabel(row(file,{file,second})),QString("Finish share links"));
  const QJsonObject hidden{{"uploadStatus","failed"},{"shareStatus","not_requested"}};QVERIFY(!HistoryModel::finishShareLink(row(file,{file},{{"state","completed"},{"items",QJsonArray{file,hidden}}})));
  QVERIFY(!HistoryModel::finishShareLink(row(file,{file},{{"state","running"},{"items",QJsonArray{file}}})));
  auto changed=file;changed["shareStatus"]="changed";changed["linkFinishable"]=false;QVERIFY(!HistoryModel::finishShareLink(row(changed,{changed})));
  QCOMPARE(HistoryModel::statusText(row(changed,{changed})),QString("Share link changed"));
  auto noIntent=file;noIntent["shareIntent"]="";noIntent["linkFinishable"]=false;QCOMPARE(HistoryModel::statusText(row(noIntent,{noIntent})),QString("Sharing outcome needs inspection"));
 }
 void offlineSearchUsesOriginalReceiptsAndRemovedLinksAreDisabled() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-operation";op.created="2020-01-01T00:00:00.000Z";op.restore(OperationState::Completed);op.settings=settings.snapshot();OperationItem item;item.artifact.id="old-item";item.artifact.name="old-image.png";item.artifact.mime="image/png";item.artifact.category="images";item.uploadStatus="uploaded";item.remoteStatus="removed";item.receipt.directUrl="https://example.invalid/original.png";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);UiHistoryQuery query;query.search="old-image";model.setQuery(query);PUTGD_TRY_COMPARE(model.rowCount(),1);QVERIFY(!model.linkAvailable(0));
  HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QCOMPARE(source.copies,0);
 }
};QTEST_MAIN(HistoryGui)
#include "test_history_view.moc"
