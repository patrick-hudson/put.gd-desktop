#include <QDateTime>
#include <QLocale>
#include "ui/HistoryModel.h"
#include "ui/HistoryWindow.h"
#include "ui/RedesignTheme.h"
#include "ui/Graphite.h"
#include "fixtures/UiSessionFixture.h"
#include "app/DiagnosticsBundle.h"
#include <QFile>
#include <QFileInfo>
#include <QLayout>
#include <QListView>
#include <QComboBox>
#include <QMenu>
#include <QClipboard>
#include <QScrollArea>
#include <QLineEdit>
#include <QListWidget>
#include "ui/ViewState.h"
#include <QPushButton>
#include <QApplication>
#include <QMessageBox>
#include <QDialog>
#include <QTimer>
#include <QScopeGuard>
#include <utility>
#include <QLabel>
#include <QToolButton>
#include <QImage>
#include <QPlainTextEdit>
#include <QPaintEvent>
#include <QPainter>
#include <QStyledItemDelegate>
#include <QStandardItemModel>
#include <QStyleFactory>
#include <QTextDocument>
#include <QAbstractTextDocumentLayout>
#include <QTextBlock>
#include <QTextLayout>
#include <QScrollBar>
#include <cmath>
#include <QSignalBlocker>
#include <QSqlDatabase>
#include <QSqlQuery>
#include <QUuid>
#include <QElapsedTimer>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QSignalSpy>
#include "fixtures/Waits.h"
using namespace putgd;using namespace putgd::test;
class HistoryPaintObserver final:public QObject {
public:int paints=0;QRegion region;
 bool eventFilter(QObject*,QEvent* event) override {if(event->type()==QEvent::Paint){++paints;region|=static_cast<QPaintEvent*>(event)->region();}return false;}
};
// Leave the action backgrounds transparent: any filename/status ink underneath
// their actual painted bounds is an overlap that an opaque button would hide.
class HistoryActionInkProbe final:public QProxyStyle {
public:
 HistoryActionInkProbe():QProxyStyle(QStyleFactory::create("Fusion")){}
 mutable QList<QRect> buttons;
 void drawControl(ControlElement element,const QStyleOption* option,QPainter* painter,const QWidget* widget=nullptr)const override{
  if(element==CE_PushButton){buttons.append(option->rect);return;}
  QProxyStyle::drawControl(element,option,painter,widget);
 }
 void drawPrimitive(PrimitiveElement element,const QStyleOption* option,QPainter* painter,const QWidget* widget=nullptr)const override{
  if(element==PE_PanelButtonCommand){buttons.append(option->rect);return;}
  QProxyStyle::drawPrimitive(element,option,painter,widget);
 }
};
// A real resident sharing job stays pending while this synthetic provider is
// paused. Reopening its view may inspect the job, but must never call share again.
class PendingSharingProvider final:public StorageProvider {
public:
 OpaqueProvider storage;std::atomic_int mutations{0},inspections{0};std::atomic_bool entered{false},released{false};QString outcome="shared",expiresAt;
 ProviderCapabilities capabilities(const Destination& d)const override{auto c=storage.capabilities(d);c.sharing=true;c.privateUploads=true;return c;}
 QString defaultPublicationMode(const Destination&)const override{return "private";}
 QString serializationKey(const Destination& d,const QJsonObject& ref,const QString& name)const override{return storage.serializationKey(d,ref,name);}
 Error validateSettings(const QJsonObject& s)const override{return storage.validateSettings(s);}
 void checkName(const Destination& d,const QString& name,std::function<void(NameCheck)> reply, std::shared_ptr<std::atomic_bool> cancellation = {})override{storage.checkName(d,name,std::move(reply),cancellation);}
 void upload(const UploadRequest& request,std::function<void(UploadResult)> reply)override{storage.upload(request,[reply](UploadResult result){result.receipt.directUrl.clear();reply(result);});}
 void stat(const StorageReceipt& receipt,std::function<void(StatResult)> reply)override{storage.stat(receipt,std::move(reply));}
 void reconcile(const UploadRequest& request,std::function<void(StatResult)> reply)override{storage.reconcile(request,std::move(reply));}
 void remove(const DeleteRequest& request,DurableCheckpoint checkpoint,std::function<void(DeleteResult)> reply)override{storage.remove(request,std::move(checkpoint),std::move(reply));}
 void share(const ShareRequest& request,DurableCheckpoint checkpoint,std::function<void(ShareResult)> reply)override{
  if(request.action=="inspect"){++inspections;auto receipt=request.receipt;receipt.directUrl=outcome=="shared"?"https://example.invalid/shared#synthetic-key":QString{};reply({outcome,receipt,{}, {},expiresAt});return;}
  const QJsonObject progress{{"schemaVersion",1},{"phase","recorded-intent"}};
  checkpoint(progress,[this,request,progress,reply](Error error){
   if(error){reply({"failed",request.receipt,progress,error});return;}++mutations;entered.store(true);
   while(!released.load()&&!request.cancellation->load())std::this_thread::sleep_for(std::chrono::milliseconds(1));
   auto receipt=request.receipt;receipt.directUrl=outcome=="shared"?"https://example.invalid/shared#synthetic-key":QString{};
   reply({outcome,receipt,progress,outcome=="shared"?Error{}:Error{"synthetic_share_failed","Synthetic sharing failure",{}},expiresAt});
  });
 }
};
class HistoryCopyFailureSource final:public SourceAdapter {
public:quint64 revision=0;bool fail=true;int reads=0,copies=0;
 quint64 clipboardRevision()const override{return revision;}QJsonObject capabilities()const override{return {{"clipboard",true}};}
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++reads;reply({textPayload(),{}});}
 void cancelAcquisition()override{}
 void copy(const CopyPayload&,std::function<void(Error)> reply)override{++copies;if(fail)reply({"clipboard_unavailable","Synthetic clipboard refusal",{}});else{++revision;reply({});}}
};
class HistoryGui:public QObject {
 Q_OBJECT
 static bool sql(const QString& path,const QString& statement){
  const auto connection=QUuid::createUuid().toString();bool ok=false;
  {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);if(db.open()){QSqlQuery query(db);ok=query.exec(statement);}db.close();}
  QSqlDatabase::removeDatabase(connection);return ok;
 }

 // The remote-deletion confirmation, as an agent or test finds it.
 static QDialog* deletionDialog(){for(auto* widget:QApplication::topLevelWidgets())if(auto* dialog=qobject_cast<QDialog*>(widget);dialog&&dialog->isVisible()&&dialog->objectName()=="confirmAction"&&dialog->windowTitle()=="Delete from destination")return dialog;return nullptr;}
 static void selectRow(QListView* view,int row){view->selectionModel()->select(view->model()->index(row,0),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);view->setCurrentIndex(view->model()->index(row,0));}
 static int operationRow(QListView* list,const QString& id){auto* model=qobject_cast<HistoryModel*>(list->model());for(int row=0;row<model->rowCount();++row)if(model->record(row)["operationId"]==id)return row;return -1;}
 static bool rowMenuAction(QListView* list,const QString& text){
  list->window()->activateWindow();list->setFocus();putgd::test::settle(20);auto triggered=std::make_shared<bool>(false);QTimer::singleShot(0,list,[triggered,text]{for(auto* widget:QApplication::topLevelWidgets())if(auto* menu=qobject_cast<QMenu*>(widget);menu&&menu->isVisible()){for(auto* action:menu->actions())if(action->text()==text&&action->isEnabled()){action->trigger();*triggered=true;break;}menu->close();}});list->setFocus();QTest::keyClick(list,Qt::Key_Menu);QCoreApplication::processEvents();return *triggered;
 }
private slots:
 void rowActionsNeverCoverFilenameOrStatus_data(){
  QTest::addColumn<int>("width");QTest::addColumn<QString>("action");QTest::addColumn<bool>("selected");
  for(int width:{300,430,594,840})for(const auto& action:QStringList{"Copy link","Retry local save",""})for(bool selected:{false,true})
   QTest::newRow(qPrintable(QString("%1-%2-%3").arg(width).arg(action.isEmpty()?"menu":action).arg(selected?"selected":"hovered")))<<width<<action<<selected;
 }
 void rowActionsNeverCoverFilenameOrStatus(){
  QFETCH(int,width);QFETCH(QString,action);QFETCH(bool,selected);
  QTemporaryDir dir;SettingsService settings(dir.filePath("config.json"));CredentialStore secrets;UiSessionFixture fixture(settings,secrets);
  HistoryActionInkProbe probe;HistoryWindow window(fixture.session);auto* list=window.findChild<QListView*>("historyList");QVERIFY(list);list->setStyle(&probe);
  QStandardItemModel model(1,1);const auto index=model.index(0,0);
  model.setData(index,QString(160,'W')+".png",Qt::DisplayRole);
  model.setData(index,QString(160,'W'),HistoryModel::SubtitleRole);
  model.setData(index,action,HistoryModel::PrimaryActionRole);
  // The actual viewport drives the delegate's size hint, including its narrow
  // action row. Paint with that same width to test both layout and clipping.
  list->viewport()->resize(width+2*list->spacing(),300);
  QStyleOptionViewItem option;option.initFrom(list);option.font=list->font();
  const auto size=list->itemDelegate()->sizeHint(option,index);QCOMPARE(size.width(),width);option.rect=QRect(QPoint(),size);
  option.state=QStyle::State_Enabled|(selected?QStyle::State_Selected:QStyle::State_MouseOver);
  QImage actual(size,QImage::Format_ARGB32_Premultiplied);actual.fill(Qt::transparent);
  {QPainter painter(&actual);list->itemDelegate()->paint(&painter,option,index);}
  QCOMPARE(probe.buttons.size(),action.isEmpty()?1:2);
  const auto& theme=Theme::current();const auto title=theme.text.rgb(),status=theme.textMuted.rgb();int visibleInk=0;
  for(int y=0;y<actual.height();++y)for(int x=0;x<actual.width();++x){const auto color=actual.pixelColor(x,y).rgb();if(color==title||color==status)++visibleInk;}
  QVERIFY2(visibleInk>20,"Narrow rows must retain visible filename and status text");
  for(const auto& bounds:probe.buttons){
   QVERIFY2(actual.rect().contains(bounds),"Every row action must fit inside its row");int coveredInk=0;
   for(int y=bounds.top();y<=bounds.bottom();++y)for(int x=bounds.left();x<=bounds.right();++x){const auto color=actual.pixelColor(x,y).rgb();if(color==title||color==status)++coveredInk;}
   QVERIFY2(coveredInk==0,qPrintable(QString("Action at %1,%2 %3x%4 covers %5 text pixels").arg(bounds.x()).arg(bounds.y()).arg(bounds.width()).arg(bounds.height()).arg(coveredInk)));
  }
 }
 void preflightStatusDescribesDestinationCheck() {
  const QJsonObject record{{"operation",QJsonObject{{"state","running"},{"phase","preflight"},{"action","clipboard"}}}};
  QCOMPARE(HistoryModel::statusText(record),QString("Checking the destination…"));
 }

 void queuedEditorStatusTakesPrecedence_data(){
  QTest::addColumn<QString>("state");
  for(const auto& state:QStringList{"editing","acquiring","stored","running"})QTest::newRow(qPrintable(state))<<state;
 }
 void queuedEditorStatusTakesPrecedence(){
  QFETCH(QString,state);
  const QJsonObject op{{"action","edit"},{"state",state},{"phase","queued"}};
  QCOMPARE(HistoryModel::statusText({{"operation",op}}),QString("Waiting to upload…"));
  QCOMPARE(HistoryModel::statusText({{"operation",QJsonObject{{"action","edit"},{"state","editing"},{"phase","editing"}}}}),QString("Editing · Upload or cancel in the editor window"));
 }
 void fullFailureTextIsPaintedWithoutClipping_data(){
  QTest::addColumn<bool>("gallery");QTest::addColumn<int>("width");QTest::addColumn<bool>("longToken");
  // 760: the narrowest History page that keeps its sidebar (the main window is at least 1060 wide).
  for(bool gallery:{false,true})for(int width:{1000,760})for(bool token:{false,true})QTest::newRow(qPrintable(QString("%1-%2-%3").arg(gallery?"gallery":"list").arg(width).arg(token?"long-token":"gnome-guide")))<<gallery<<width<<token;
 }
 void fullFailureTextIsPaintedWithoutClipping(){
  QFETCH(bool,gallery);QFETCH(int,width);QFETCH(bool,longToken);
  const QString guide="GNOME capture needs the put.gd Shell extension. If it is missing, install the matching put.gd package or extension; after a new installation, log out and back in first. Then run: gnome-extensions enable putgd@put.gd";
  const QString message=guide+(longToken?"\nLiteral <b>text</b>: "+QString(1800,'W')+"\nEND OF RECOVERY":QString{});
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="long-failed-capture";op.action="window";op.created="2022-01-01T00:00:00.000Z";op.state="failed";op.error={"helper_unavailable",message,{}};op.settings=settings.snapshot();QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);RedesignTheme::apply(window);window.resize(width,700);window.show();auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);
  if(gallery)window.findChild<QToolButton*>("historyGridMode")->click();
  HistoryPaintObserver painted;list->viewport()->installEventFilter(&painted);
  // Resize both directions: cached/fixed item heights must not clip a narrower layout.
  for(int target:{width,width+180,width}){
   window.resize(target,700);
   for(bool selected:{false,true}){
   painted.paints=0;if(selected)selectRow(list,0);else list->clearSelection();list->viewport()->update();
   const auto index=list->model()->index(0,0);QStyleOptionViewItem layoutOption;layoutOption.initFrom(list);layoutOption.font=list->font();
   PUTGD_VERIFY_STEADY(painted.paints>0&&window.width()<=target&&list->selectionModel()->selectedRows().size()==int(selected)&&list->visualRect(index).height()==list->itemDelegate()->sizeHint(layoutOption,index).height(),20);
   const auto row=list->visualRect(index);QVERIFY(row.isValid());
   const auto record=index.data(HistoryModel::RecordRole).toJsonObject();QVERIFY(HistoryModel::storedNothing(record));QVERIFY(index.data(HistoryModel::PrimaryActionRole).toString().isEmpty());QVERIFY(index.data(Qt::AccessibleTextRole).toString().contains(message));
   // The delegate's own failure area (Graphite layout), in 12 px metadata type.
   const auto area=HistoryWindow::failureTextArea(row.size(),gallery,!index.data(HistoryModel::DayRole).toString().isEmpty());const int left=area.left(),top=area.top(),textWidth=area.width();
   QTextDocument expected;expected.setDocumentMargin(0);expected.setDefaultFont(Graphite::font(Graphite::Type::metadata));auto textOption=expected.defaultTextOption();textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);expected.setDefaultTextOption(textOption);expected.setPlainText(message);expected.setTextWidth(textWidth);
   const int height=int(std::ceil(expected.size().height()));QVERIFY2(top+height+8<=row.height()-(gallery?48:0),qPrintable(QString("Complete error needs y%1 + %2px; row%3").arg(top).arg(height).arg(row.height())));
   for(auto block=expected.begin();block.isValid();block=block.next())for(int line=0;line<block.layout()->lineCount();++line)QVERIFY(block.layout()->lineAt(line).naturalTextWidth()<=textWidth+1);
   // Compare the actual delegate's entire painted error rectangle to complete
   // plain text, including the last command/token. A role/string assertion alone
   // would pass the old elided painter and the gallery that omitted errors.
   QImage actual(row.size(),QImage::Format_ARGB32_Premultiplied);actual.fill(Qt::transparent);QStyleOptionViewItem option;option.initFrom(list);option.font=list->font();option.rect=QRect(QPoint(),row.size());option.state=QStyle::State_Enabled|(selected?QStyle::State_Selected:QStyle::State_None);{QPainter painter(&actual);list->itemDelegate()->paint(&painter,option,index);}
   QImage complete(QSize(textWidth,height),QImage::Format_ARGB32_Premultiplied);complete.fill(selected?RedesignTheme::selected():gallery?RedesignTheme::surface():RedesignTheme::bg());{QPainter painter(&complete);QAbstractTextDocumentLayout::PaintContext context;context.palette.setColor(QPalette::Text,RedesignTheme::textSecondary());expected.documentLayout()->draw(&painter,context);}
   QCOMPARE(actual.copy(left,top,textWidth,height),complete);
   // An oversized error remains reachable through the real pixel scroll range.
   if(row.height()>list->viewport()->height()){
    QVERIFY(list->verticalScrollBar()->maximum()>0);painted.paints=0;list->verticalScrollBar()->setValue(list->verticalScrollBar()->maximum());list->viewport()->update();
    PUTGD_VERIFY_STEADY(painted.paints>0&&list->verticalScrollBar()->value()==list->verticalScrollBar()->maximum()&&list->visualRect(index).bottom()<=list->viewport()->height(),20);
   }
   }
  }
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void externalDeletionAndClearRefreshOpenViews() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.copies,1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);const QSignalBlocker settingsSignals(settings),operationSignals(operations);HistoryWindow window(fixture.session);window.show();
  auto* table=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(table->model());PUTGD_TRY_COMPARE(model->rowCount(),1);selectRow(table,0);putgd::test::settle(100);
  DispatchSessionClient external(fixture.server);const auto id=model->record(0)["id"].toString();QJsonObject preview;external.request("delete.prepare",{{"ids",QJsonArray{id}}},this,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());
  bool accepted=false;external.request("delete.confirm",{{"ids",QJsonArray{id}},{"acknowledgement",preview["acknowledgement"]}},this,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});PUTGD_TRY_VERIFY(accepted);PUTGD_TRY_COMPARE(provider->deletes.load(),1);
  PUTGD_TRY_COMPARE(model->record(0)["remoteStatus"].toString(),QString("removed"));PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("Deleted from destination"));
  bool cleared=false;external.request("history.clear",{},this,[&](Error e,QJsonObject){QVERIFY(!e);cleared=true;});PUTGD_TRY_VERIFY(cleared);PUTGD_TRY_COMPARE(model->rowCount(),0);QVERIFY(window.saveViewState()["selectedIds"].toArray().isEmpty());
 }
 // A single file whose headline leads with another outcome (an interrupted
 // upload that saved locally) still states its own upload outcome.
 void singleFileKeepsItsUploadOutcomeVisible(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1024,700);window.show();
  const auto kept=d.filePath("kept.txt");{QFile file(kept);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic");}
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-interrupted";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();
   OperationItem item;item.artifact.id="interrupted-item";item.artifact.name="interrupted.txt";item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="unknown";item.local.status="saved";item.local.path=kept;op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  window.refresh();auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);
  auto* summary=window.findChild<QLabel*>("historySummary");auto* outcome=window.findChild<QLabel*>("historyOutcome");
  PUTGD_TRY_VERIFY((outcome->text()+" "+summary->text()).contains("Upload outcome unknown",Qt::CaseInsensitive));
  // Stated once: the row appears only when the headline does not already say it.
  QVERIFY(!(outcome->text().contains("Upload outcome unknown",Qt::CaseInsensitive)&&summary->text().contains("Upload outcome unknown")));
 }
 // The longest combined heading elides at the minimum width and 150% text;
 // search and the view switch stay whole and inside the header.
 void longHeadingKeepsSearchAndViewsReachable(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  auto font=QApplication::font();const auto original=font;font.setPixelSize(21);QApplication::setFont(font);auto restore=qScopeGuard([&]{QApplication::setFont(original);});
  HistoryWindow window(fixture.session);window.resize(1060-184,600);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  window.findChild<QListWidget*>("historyCategories")->setCurrentRow(2);window.findChild<QToolButton*>("historyFailedOnly")->setChecked(true);putgd::test::settle(50);
  auto* heading=window.findChild<QLabel*>("historyHeading");QCOMPARE(heading->text(),QString("Recordings · failed uploads"));
  for(const char* name:{"historySearch","historyListMode","historyGridMode","historyFiltersExpanded"}){auto* w=window.findChild<QWidget*>(name);QVERIFY2(w&&w->isVisible(),name);const QRect r(w->mapTo(&window,QPoint()),w->size());QVERIFY2(window.rect().contains(r),name);QVERIFY2(!r.intersects(QRect(heading->mapTo(&window,QPoint()),heading->size())),name);}
  QVERIFY(window.findChild<QWidget*>("historySearch")->width()>=160);
 }
 void batchCapturesFoldFilesAndKeepOriginalDetailsRoutedById(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1024,700);window.show();
  auto* list=window.findChild<QListView*>("historyList");auto* inspector=window.findChild<QScrollArea*>("historyInspectorScroll");QVERIFY(list);QVERIFY(inspector);QVERIFY(!inspector->isVisible());auto* empty=window.findChild<QLabel*>("historyEmpty");PUTGD_TRY_VERIFY(empty->isVisible());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-batch-history";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();op.destination.profileId="original-synthetic-destination";
   for(int i=0;i<2;++i){OperationItem item;item.artifact.id=QString("batch-history-item-%1").arg(i);item.artifact.name=QString("synthetic-file-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus=i==1?"failed":"local_only";op.items.append(item);}QVERIFY(!ledger.saveOperation(op));}
  window.refresh();PUTGD_TRY_COMPARE(list->model()->rowCount(),1);QVERIFY(!empty->isVisible());selectRow(list,0);auto* items=window.findChild<QComboBox*>("historyItems");PUTGD_TRY_COMPARE(items->count(),2);QVERIFY(items->isVisible());auto* details=window.findChild<QPlainTextEdit*>("historyDetails");auto* summary=window.findChild<QLabel*>("historySummary");
  for(int item=0;item<items->count();++item){items->setCurrentIndex(item);PUTGD_TRY_COMPARE(QJsonDocument::fromJson(details->toPlainText().toUtf8()).object()["id"].toString(),items->currentData().toString());QVERIFY(summary->text().contains("text/plain"));QVERIFY(!summary->text().contains("original-synthetic-destination"));
   // The line under the title describes the batch; each file shows its own upload state.
   const bool failedItem=items->currentData().toString()=="batch-history-item-1";QVERIFY2(summary->text().contains(failedItem?"Upload failed":"Local capture"),qPrintable(summary->text()));}
  auto* gallery=window.findChild<QToolButton*>("historyGridMode");gallery->click();QCOMPARE(list->viewMode(),QListView::IconMode);QVERIFY(!inspector->isVisible());QCOMPARE(list->selectionModel()->selectedRows().size(),1);const auto saved=window.saveViewState();QVERIFY(saved["gridMode"].toBool());HistoryWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));PUTGD_TRY_COMPARE(reopened.findChild<QListView*>("historyList")->selectionModel()->selectedRows().size(),1);QCOMPARE(reopened.findChild<QListView*>("historyList")->viewMode(),QListView::IconMode);QCOMPARE(reopened.saveViewState()["selectedItemId"],saved["selectedItemId"]);
  window.findChild<QToolButton*>("historyListMode")->click();QVERIFY(inspector->isVisible());for(auto size:{QSize(1024,700),QSize(1280,700),QSize(1024,700)}){window.resize(size);putgd::test::settle(20);PUTGD_TRY_VERIFY(window.width()<=size.width());PUTGD_TRY_VERIFY(window.height()<=size.height());PUTGD_TRY_COMPARE(list->selectionModel()->selectedRows().size(),1);}list->clearSelection();QVERIFY(!inspector->isVisible());QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
  // The heading names what is shown, through the filter and a restored view too.
  {auto* heading=window.findChild<QLabel*>("historyHeading");QVERIFY(heading);QCOMPARE(heading->text(),QString("All items"));auto* state=window.findChild<QLineEdit*>("historyFilteruploadStatus");auto* apply=window.findChild<QPushButton*>("historyApplyFilters");
   state->setText("failed");apply->click();QCOMPARE(heading->text(),QString("Failed uploads"));const auto failedView=window.saveViewState();
   state->clear();apply->click();QCOMPARE(heading->text(),QString("All items"));
   window.findChild<QListWidget*>("historyCategories")->setCurrentRow(1);QCOMPARE(heading->text(),QString("Images"));window.findChild<QListWidget*>("historyCategories")->setCurrentRow(0);
   HistoryWindow restored(fixture.session);QVERIFY(restored.restoreViewState(failedView));QCOMPARE(restored.findChild<QLabel*>("historyHeading")->text(),QString("Failed uploads"));}
 }
 void groupedLocalFailureSurvivesRecentOperationEviction_data(){QTest::addColumn<bool>("evicted");QTest::newRow("recent")<<false;QTest::newRow("evicted")<<true;}
 void groupedLocalFailureSurvivesRecentOperationEviction(){
  QFETCH(bool,evicted);QTemporaryDir d;SettingsService settings(d.filePath("config.json"));auto config=configuration(d.path(),true);auto retention=config["retention"].toObject();retention["recentActivityLimit"]=1;config["retention"]=retention;QVERIFY(settings.save(config,{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore ledger(operations.databasePath());Operation op;op.id="mixed-local-batch";op.created="2020-01-01T00:00:00.000Z";op.state="failed";op.settings=settings.snapshot();
  for(int i=0;i<2;++i){OperationItem item;item.artifact.id=QString("mixed-item-%1").arg(i);item.artifact.name=QString("mixed-file-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";item.local.status=i?"failed":"saved";if(i)item.local.error={"archive_io","Synthetic local failure",{}};op.items.append(item);}QVERIFY(!ledger.saveOperation(op));
  if(evicted){Operation newer;newer.id="newer-local-operation";newer.created="2021-01-01T00:00:00.000Z";newer.state="completed";newer.settings=settings.snapshot();QVERIFY(!ledger.saveOperation(newer));}Error error;const auto recent=ledger.recent(1,&error);QVERIFY(!error);QCOMPARE(recent.first().toObject()["id"].toString(),evicted?QString("newer-local-operation"):op.id);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);model.refresh();PUTGD_TRY_COMPARE(model.rowCount(),1);const auto index=model.index(0);QVERIFY(model.data(index,HistoryModel::FailedRole).toBool());QCOMPARE(model.data(index,HistoryModel::PrimaryActionRole).toString(),QString("Retry local save"));QCOMPARE(HistoryModel::statusText(model.record(0)),QString("Local save failed"));const auto children=model.record(0)["records"].toArray();QCOMPARE(children[0].toObject()["local"].toObject()["status"].toString(),QString("saved"));QCOMPARE(children[1].toObject()["local"].toObject()["status"].toString(),QString("failed"));
  op.items[1].local.status="saved";op.items[1].local.error={};op.state="completed";QVERIFY(!ledger.saveOperation(op));model.refresh();PUTGD_TRY_VERIFY(!model.data(index,HistoryModel::FailedRole).toBool());QCOMPARE(model.data(index,HistoryModel::PrimaryActionRole).toString(),QString("Copy result"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void recreatedHistoryRestoresRawFiltersSelectionAndClearsVanishedIds(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="state-operation";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="state-history-id";item.artifact.name="state.txt";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);QJsonObject state;
  {HistoryWindow window(fixture.session);auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);window.findChild<QLineEdit*>("historySearch")->setText("raw unapplied search");window.findChild<QLineEdit*>("historyFilterfrom")->setText("invalid raw date");state=window.saveViewState();}
  QVERIFY(!validateUiState({{"schemaVersion",1},{"history",state}}));QVERIFY(!QJsonDocument(state).toJson().contains("state.txt"));HistoryWindow restored(fixture.session);QVERIFY(restored.restoreViewState(state));auto* table=restored.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->selectionModel()->selectedRows().size(),1);QCOMPARE(restored.findChild<QLineEdit*>("historySearch")->text(),QString("raw unapplied search"));QCOMPARE(restored.findChild<QLineEdit*>("historyFilterfrom")->text(),QString("invalid raw date"));
  bool cleared=false;history.clear([&](Error e,QJsonObject){QVERIFY(!e);cleared=true;});PUTGD_TRY_VERIFY(cleared);PUTGD_TRY_COMPARE(table->model()->rowCount(),0);QVERIFY(restored.saveViewState()["selectedIds"].toArray().isEmpty());QCOMPARE(source.reads,0);
 }
 void restoringHistoryKeepsActiveDeletionCancellation() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  std::atomic_bool entered{false},released{false};provider->checkpointObserver=[&](const DeleteRequest&){entered.store(true);while(!released.load())std::this_thread::sleep_for(std::chrono::milliseconds(1));};
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.copies,1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);auto original=std::make_unique<HistoryWindow>(fixture.session);auto& window=*original;window.show();auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);
  auto drain=qScopeGuard([&]{released.store(true);});
  QTimer accept;accept.setInterval(10);connect(&accept,&QTimer::timeout,this,[]{if(auto* dialog=deletionDialog())dialog->findChild<QPushButton*>("confirmActionButton")->click();});accept.start();
  window.findChild<QPushButton*>("deleteHistoryRemote")->click();PUTGD_TRY_VERIFY(entered.load());accept.stop();
  const auto saved=window.saveViewState();QVERIFY(!saved["deletionId"].toString().isEmpty());original.reset();HistoryWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(saved));reopened.show();PUTGD_TRY_VERIFY(!reopened.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().isEmpty());QPushButton* cancel=nullptr;for(auto* button:reopened.findChildren<QPushButton*>())if(button->text()=="Cancel deletion")cancel=button;QVERIFY(cancel);
  cancel->click();QLabel* status=nullptr;for(auto* label:reopened.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);PUTGD_TRY_VERIFY(status->text().contains("Cancellation requested"));
  released.store(true);operations.shutdown(); // Drain the delayed worker while its service/fixture still exist.
 }
 void recreatedHistoryResumesSharingInspectionWithoutReplay_data(){
  QTest::addColumn<QString>("outcome");QTest::newRow("shared")<<QString("shared");QTest::newRow("failed")<<QString("failed");
 }
 void recreatedHistoryResumesSharingInspectionWithoutReplay(){
  QFETCH(QString,outcome);QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<PendingSharingProvider>();provider->outcome=outcome;registry.add("opaque",provider);
  // Keep journal directory metadata outside the config directory watcher.
  SettingsService settings(d.filePath("config/config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->storage.uploads.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  auto drain=qScopeGuard([&]{provider->released.store(true);operations.shutdown();});
  auto original=std::make_unique<HistoryWindow>(fixture.session);original->show();auto* table=original->findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);auto* create=original->findChild<QPushButton*>("shareHistoryLink");PUTGD_TRY_VERIFY(create->isVisible()&&create->isEnabled());
  QJsonObject acceptedState;connect(original.get(),&HistoryWindow::viewStateChanged,this,[&]{const auto state=original->saveViewState();if(!state["sharingId"].toString().isEmpty())acceptedState=state;});
  create->click();PUTGD_TRY_VERIFY(provider->entered.load());PUTGD_TRY_VERIFY(!acceptedState["sharingId"].toString().isEmpty());QVERIFY(!validateUiState({{"schemaVersion",1},{"history",acceptedState}}));
  auto legacyState=acceptedState;legacyState.remove("sharingId");QVERIFY(!validateUiState({{"schemaVersion",1},{"history",legacyState}}));auto invalidState=acceptedState;invalidState["sharingId"]=QJsonObject{{"unsupported","value"}};QCOMPARE(validateUiState({{"schemaVersion",1},{"history",invalidState}}).code,QString("ui_state_invalid"));
  // Selection is restored before the asynchronous inspector details arrive.
  fixture.client.replyDelayMs["history.detail"]=350;original.reset();HistoryWindow reopened(fixture.session);QVERIFY(reopened.restoreViewState(acceptedState));reopened.show();QCOMPARE(reopened.saveViewState()["sharingId"].toString(),acceptedState["sharingId"].toString());PUTGD_TRY_COMPARE(reopened.findChild<QListView*>("historyList")->selectionModel()->selectedRows().size(),1);PUTGD_TRY_VERIFY(!reopened.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().isEmpty());QVERIFY(!reopened.findChild<QPushButton*>("shareHistoryLink")->isEnabled());
  provider->released.store(true);QLabel* status=nullptr;for(auto* label:reopened.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);
  PUTGD_TRY_VERIFY(status->text().contains("Link: "+outcome));if(outcome=="failed")QVERIFY(status->text().contains("Synthetic sharing failure"));
  PUTGD_TRY_VERIFY(reopened.findChild<QLabel*>("historySummary")->text().contains(outcome=="shared"?"Shared link":"Sharing failed"));QVERIFY(status->text().contains("Link: "+outcome));
  QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(source.reads,1);QCOMPARE(source.copies,0);
 }
 void linkRowShowsTheConfirmedExpiry(){
  // History's link row says when the link expires, and that it expired once it has (#253).
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<PendingSharingProvider>();provider->released.store(true);const auto future=QDateTime::currentDateTimeUtc().addDays(7);provider->expiresAt=future.toString(Qt::ISODateWithMs);registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->storage.uploads.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);auto drain=qScopeGuard([&]{operations.shutdown();});HistoryWindow window(fixture.session);window.show();
  auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);auto* create=window.findChild<QPushButton*>("shareHistoryLink");auto* inspect=window.findChild<QPushButton*>("inspectHistoryLink");auto* copy=window.findChild<QPushButton*>("copyHistoryLink");auto* expiry=window.findChild<QLabel*>("historyLinkExpiry");QVERIFY(expiry);
  PUTGD_TRY_VERIFY(create->isVisible()&&create->isEnabled());QVERIFY(expiry->isHidden());
  create->click();PUTGD_TRY_COMPARE(expiry->text(),"Link expires on "+QLocale().toString(future.toLocalTime(),QLocale::ShortFormat));QVERIFY(expiry->isVisible());PUTGD_TRY_VERIFY(copy->isEnabled());
  const auto past=QDateTime::currentDateTimeUtc().addSecs(-60);provider->expiresAt=past.toString(Qt::ISODateWithMs);provider->outcome="private";
  PUTGD_TRY_VERIFY(inspect->isEnabled());inspect->click();PUTGD_TRY_COMPARE(expiry->text(),"Link expired on "+QLocale().toString(past.toLocalTime(),QLocale::ShortFormat));QVERIFY(expiry->isVisible());QVERIFY(!copy->isEnabled());
  QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->inspections.load(),1);QCOMPARE(provider->storage.uploads.load(),1);
 }
 void stoppedSharingJournalCanBeInspectedAfterStorageRecovers(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<PendingSharingProvider>();registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->storage.uploads.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);auto drain=qScopeGuard([&]{provider->released.store(true);operations.shutdown();});HistoryWindow window(fixture.session);window.show();
  auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);auto* create=window.findChild<QPushButton*>("shareHistoryLink");auto* inspect=window.findChild<QPushButton*>("inspectHistoryLink");PUTGD_TRY_VERIFY(create->isVisible()&&create->isEnabled());
  create->click();PUTGD_TRY_VERIFY(provider->entered.load());PUTGD_TRY_VERIFY(!window.saveViewState()["sharingId"].toString().isEmpty());const auto stoppedId=window.saveViewState()["sharingId"].toString();
  QVERIFY(sql(operations.databasePath(),"CREATE TRIGGER fail_share BEFORE UPDATE ON sharing_operations BEGIN SELECT RAISE(FAIL,'synthetic journal failure'); END"));provider->released.store(true);
  QLabel* status=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);PUTGD_TRY_VERIFY(status->text().contains("Link: unknown"));QVERIFY(status->text().contains("Persistent journal"));
  HistoryStore store(operations.databasePath());QCOMPARE(store.sharing(stoppedId)["state"].toString(),QString("running"));PUTGD_TRY_VERIFY(inspect->isVisible()&&inspect->isEnabled());QVERIFY(!create->isEnabled());QVERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->inspections.load(),0);
  QVERIFY(sql(operations.databasePath(),"DROP TRIGGER fail_share"));inspect->click();PUTGD_TRY_VERIFY(window.saveViewState()["sharingId"].toString()!=stoppedId);PUTGD_TRY_COMPARE(provider->inspections.load(),1);PUTGD_TRY_VERIFY(status->text().contains("Link: shared"));PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("Shared link"));
  QCOMPARE(store.sharing(stoppedId)["state"].toString(),QString("interrupted"));QCOMPARE(provider->mutations.load(),1);QCOMPARE(provider->storage.uploads.load(),1);QCOMPARE(source.reads,1);QCOMPARE(source.copies,0);
 }
 void imageInspectorAndExpandedControlsFitCompactWindow() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  const auto imagePath=d.filePath("synthetic-preview.png");QImage image(320,180,QImage::Format_RGB32);image.fill(QColor("#a9f1cb"));QVERIFY(image.save(imagePath));
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-image-inspector";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="synthetic-preview-item";item.artifact.name="synthetic-preview.png";item.artifact.mime="image/png";item.artifact.category="images";item.artifact.previewPath=imagePath;item.artifact.bytes=1572864;item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);HistoryPaintObserver paint;window.installEventFilter(&paint);window.resize(1000,700);window.show();auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);auto* preview=window.findChild<QLabel*>("historyPreview");QVERIFY(preview);PUTGD_TRY_VERIFY(!preview->pixmap().isNull());QVERIFY(preview->isVisible());QVERIFY(preview->height()>0);PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("1.5 MiB"));
  QToolButton *filters=nullptr,*manage=nullptr;for(auto* button:window.findChildren<QToolButton*>()){if(button->text()=="Filters")filters=button;if(button->text()=="Manage")manage=button;}QVERIFY(filters);QVERIFY(manage);filters->setChecked(true);manage->setChecked(true);putgd::test::settle(20);
  PUTGD_TRY_VERIFY2(window.width()<=1000&&window.height()<=700,qPrintable(QString("Expanded history grew to %1×%2").arg(window.width()).arg(window.height())));
  // Graphite: an action that cannot apply is hidden, not shown disabled.
  {auto* copy=window.findChild<QPushButton*>("copyHistoryLink");QCOMPARE(copy->isVisibleTo(&window),copy->isEnabled());}
  // A local-only file has no remote target: no Delete from destination.
  QVERIFY(window.findChild<QPushButton*>("deleteHistoryRemote")->isHidden());
  for(const auto& name:QStringList{"historyReceiptExpanded"}){auto* button=window.findChild<QWidget*>(name);window.findChild<QScrollArea*>("historyInspectorScroll")->ensureWidgetVisible(button);QVERIFY(button->isVisible());QVERIFY(window.rect().contains(button->mapTo(&window,button->rect().bottomRight())));}
  QVERIFY(window.rect().contains(manage->mapTo(&window,manage->rect().bottomRight())));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
  for(int cycle=0;cycle<3;++cycle){
   window.hide();QVERIFY2(preview->pixmap().isNull(),"Hidden history must release its decoded thumbnail");PUTGD_TRY_VERIFY_WITH_TIMEOUT(!window.testAttribute(Qt::WA_WState_Created),500);QVERIFY(!window.backingStore());
   QCOMPARE(window.findChild<QListView*>("historyList"),table);QVERIFY(filters->isChecked());QVERIFY(manage->isChecked());QCOMPARE(table->selectionModel()->selectedRows().size(),1);
   paint.paints=0;paint.region=QRegion{};window.show();PUTGD_TRY_VERIFY(window.testAttribute(Qt::WA_WState_Created));PUTGD_TRY_VERIFY(paint.paints>0);QVERIFY(paint.region.boundingRect().contains(window.rect()));PUTGD_TRY_VERIFY(!preview->pixmap().isNull());
  }
  table->clearSelection();QVERIFY(preview->pixmap().isNull());

 }

 // #112: a video's poster shows a play glyph and its length, in the list and
 // in the inspector, and the length comes from the poster the helper wrote.
 void videoPosterCarriesPlayGlyphAndLength() {
  QCOMPARE(HistoryModel::duration(0),QString());QCOMPARE(HistoryModel::duration(400),QString("0:01"));QCOMPARE(HistoryModel::duration(65000),QString("1:05"));QCOMPARE(HistoryModel::duration(3723000),QString("1:02:03"));
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  const auto posterPath=d.filePath("synthetic-poster.png");QImage poster(256,144,QImage::Format_RGB32);poster.fill(QColor("#f0f0f0"));poster.setText(HistoryModel::durationKey,"65000");QVERIFY(poster.save(posterPath));
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-video-inspector";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="synthetic-video-item";item.artifact.name="recording.mp4";item.artifact.mime="video/mp4";item.artifact.category="videos";item.artifact.previewPath=posterPath;item.artifact.bytes=1572864;item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1000,700);window.show();auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);
  const auto index=table->model()->index(0,0);PUTGD_TRY_VERIFY(!qvariant_cast<QImage>(index.data(HistoryModel::ThumbnailRole)).isNull());QCOMPARE(qvariant_cast<QImage>(index.data(HistoryModel::ThumbnailRole)).text(HistoryModel::durationKey),QString("65000"));
  // The list row draws the scrim-backed glyph over the light poster.
  // The light poster gets a scrim-backed glyph: mid-grey pixels no plain poster or dark row has.
  // Only inside the poster's own pixels (a uniform 240 grey), never in the row's text.
  auto scrim=[](const QImage& image){QRect poster;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(qGray(image.pixel(x,y))==240)poster|=QRect(x,y,1,1);
   for(int y=poster.top();y<=poster.bottom();++y)for(int x=poster.left();x<=poster.right();++x){const int g=qGray(image.pixel(x,y));if(g>70&&g<130)return true;}return false;};
  putgd::test::settle(50);PUTGD_TRY_VERIFY(scrim(table->grab(table->visualRect(index)).toImage()));
  selectRow(table,0);auto* preview=window.findChild<QLabel*>("historyPreview");PUTGD_TRY_VERIFY(!preview->pixmap().isNull());PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("1:05"));QVERIFY(scrim(preview->pixmap().toImage()));
 }
 // #190: a selected image or video whose preview is missing is asked for
 // once the selection settles; what the resident makes shows in the inspector
 // and the list. One with nothing retained keeps "Preview unavailable" and is
 // not asked for again.
 void missingPreviewIsMadeOnSelection() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  QImage capture(320,180,QImage::Format_RGB32);capture.fill(QColor("#2a7fd4"));QByteArray png;{QBuffer buffer(&png);buffer.open(QIODevice::WriteOnly);QVERIFY(capture.save(&buffer,"PNG"));}
  const auto imageId=QUuid::createUuid().toString(QUuid::WithoutBraces),videoId=QUuid::createUuid().toString(QUuid::WithoutBraces);
  const auto artifact=d.filePath("data/artifacts/synthetic-image-op/"+imageId);QVERIFY(QDir().mkpath(QFileInfo(artifact).path()));{QFile file(artifact);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(png),png.size());}
  const auto cleaned=d.filePath("data/previews/"+imageId+".png");
  {HistoryStore ledger(operations.databasePath());
   // A stored capture whose preview cleanup removed.
   Operation image;image.id="synthetic-image-op";image.created="2020-01-02T00:00:00.000Z";image.state="completed";image.settings=settings.snapshot();OperationItem stored;stored.artifact.id=imageId;stored.artifact.path=artifact;stored.artifact.name="capture.png";stored.artifact.mime="image/png";stored.artifact.category="images";stored.artifact.bytes=png.size();stored.artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex());stored.artifact.previewPath=cleaned;stored.uploadStatus="local_only";image.items.append(stored);QVERIFY(!ledger.saveOperation(image));
   // A video stored before posters whose file is gone.
   Operation video;video.id="synthetic-video-op";video.created="2020-01-01T00:00:00.000Z";video.state="completed";video.settings=settings.snapshot();OperationItem gone;gone.artifact.id=videoId;gone.artifact.path=d.filePath("data/artifacts/synthetic-video-op/"+videoId);gone.artifact.name="recording.mp4";gone.artifact.mime="video/mp4";gone.artifact.category="videos";gone.artifact.bytes=4096;gone.uploadStatus="uploaded";video.items.append(gone);QVERIFY(!ledger.saveOperation(video));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);QStringList asked;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject& params){if(method=="history.preview")asked.append(params["id"].toString());};
  // Resident changes reload History several times a second during an upload:
  // the wait must survive reloads of the same selection.
  QTimer reloads;QObject::connect(&reloads,&QTimer::timeout,&fixture.session,&UiSession::invalidate);reloads.start(150);
  HistoryWindow window(fixture.session);window.resize(1000,700);window.show();auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),2);auto* preview=window.findChild<QLabel*>("historyPreview");
  QCOMPARE(table->model()->index(0,0).data(HistoryModel::RecordRole).toJsonObject()["id"].toString(),imageId);
  // Passing over the video on the way to the image asks only for the image.
  selectRow(table,1);putgd::test::settle(100);selectRow(table,0);PUTGD_TRY_VERIFY(!preview->pixmap().isNull());QCOMPARE(asked,QStringList{imageId});
  QVERIFY(QFileInfo(cleaned).isFile());PUTGD_TRY_VERIFY(!qvariant_cast<QImage>(table->model()->index(0,0).data(HistoryModel::ThumbnailRole)).isNull());
  // Reloads of the selection (every 150 ms here) keep the shown preview: it
  // never blanks or flashes "Preview unavailable" while they run.
  int blank=0;QElapsedTimer watched;watched.start();while(watched.elapsed()<800){if(preview->pixmap().isNull())++blank;putgd::test::settle(5);}QCOMPARE(blank,0);
  QCOMPARE(preview->pixmap().toImage().pixelColor(preview->pixmap().width()/2,preview->pixmap().height()/2),QColor("#2a7fd4"));
  selectRow(table,1);PUTGD_TRY_COMPARE(asked.size(),2);QCOMPARE(asked.last(),videoId);PUTGD_TRY_COMPARE(preview->text(),QString("Preview unavailable"));QVERIFY(preview->pixmap().isNull());
  selectRow(table,0);PUTGD_TRY_VERIFY(!preview->pixmap().isNull());selectRow(table,1);PUTGD_TRY_COMPARE(preview->text(),QString("Preview unavailable"));putgd::test::settle(600);PUTGD_TRY_COMPARE(asked.size(),2);
 }
 // An idle History window with an unchanged configuration does not reload
 // (#234). Here the journal lives inside the watched configuration folder, so
 // every History read touches it; unchanged rescans must stay silent.
 void idleHistoryDoesNotReload() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-idle";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="idle-item";item.artifact.name="idle.txt";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);QHash<QString,int> calls;fixture.client.beforeDispatch=[&](const QString& method,const QJsonObject&){++calls[method];};
  HistoryWindow window(fixture.session);window.resize(1000,700);window.show();auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);PUTGD_TRY_VERIFY(calls.value("history.detail")>0);putgd::test::settle(500);
  QSignalSpy status(&settings,&SettingsService::statusChanged);const auto before=calls;putgd::test::settle(3000);
  PUTGD_TRY_COMPARE(status.count(),0);
  for(const auto* method:{"ui.snapshot","history.list","operation.recent","history.detail"})QVERIFY2(calls.value(method)==before.value(method),qPrintable(QString("%1: %2 calls while idle").arg(method).arg(calls.value(method)-before.value(method))));
 }
 void openingHistoryDuringInitializationRefreshesWhenReady() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));QVERIFY(QDir().mkpath(d.filePath("data")));
  {HistoryStore ledger(d.filePath("data/history.sqlite"));Operation op;op.id="synthetic-startup";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="startup-item";item.artifact.name="startup.txt";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);QVERIFY(!operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);auto* table=window.findChild<QListView*>("historyList");QLabel* status=nullptr;for(auto* label:window.findChildren<QLabel*>())if(label->accessibleName()=="History status")status=label;QVERIFY(status);QVERIFY(status->text().contains("Waiting for the resident journal"));PUTGD_TRY_VERIFY(operations.ready());PUTGD_TRY_COMPARE(table->model()->rowCount(),1);PUTGD_TRY_VERIFY(!status->text().contains("Waiting for the resident journal"));QVERIFY(status->text().contains("Select a capture"));QCOMPARE(source.reads,0);
 }
 void tenThousandEntryLedgerPagesAndFiltersOffline() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-large-ledger";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();for(int i=0;i<10000;++i){OperationItem item;item.artifact.id=QString("ledger-id-%1").arg(i);item.artifact.name=QString("ledger-name-%1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus=i==1?"failed":"local_only";op.items.append(item);}QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);int ticks=0;QTimer pulse;pulse.setInterval(1);connect(&pulse,&QTimer::timeout,this,[&]{++ticks;});pulse.start();UiHistoryQuery page;page.offset=9950;model.setQuery(page);PUTGD_TRY_COMPARE(model.total(),qint64(10000));QCOMPARE(model.rowCount(),1);QCOMPARE(model.record(0)["records"].toArray().size(),50);QVERIFY(ticks>0);page.offset=0;page.search="ledger-name-9999.txt";model.setQuery(page);PUTGD_TRY_COMPARE(model.rowCount(),1);PUTGD_TRY_COMPARE(model.record(0)["name"].toString(),QString("ledger-name-9999.txt"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void cancellingExactDeletionConfirmationSendsNoDelete() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto s){return registry.validate(id,s);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QString operationId;operations.invoke("clipboard",[&](Error e,QJsonObject data){QVERIFY(!e);operationId=data["operationId"].toString();});PUTGD_TRY_COMPARE(provider->uploads.load(),1);PUTGD_TRY_VERIFY(!operationId.isEmpty());QJsonObject completed;operations.inspect(operationId,[&](Error,QJsonObject data){completed=data;});PUTGD_TRY_VERIFY(!completed.isEmpty());
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);auto* table=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);const auto name=qobject_cast<HistoryModel*>(table->model())->record(0)["name"].toString();QVERIFY(!name.isEmpty());PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("deleteHistoryRemote")->isHidden());bool confirmed=false;QTimer dismiss;dismiss.setInterval(10);connect(&dismiss,&QTimer::timeout,this,[&]{auto* dialog=deletionDialog();if(!dialog)return;
   // The exact target in the list, the full receipt under Technical details,
   // Cancel as the initial and Enter/Escape choice next to the named action.
   QString shown;for(auto* label:dialog->findChild<QWidget*>("deleteConfirmationTargets")->findChildren<QLabel*>())shown+=label->text()+"\n";auto* details=dialog->findChild<QPlainTextEdit*>("deleteConfirmationDetails");auto* cancel=dialog->findChild<QPushButton*>("confirmCancel");
   QStringList missing;if(!shown.contains("Remove only the recorded opaque file revision"))missing<<"effect";if(!shown.contains(name))missing<<"name";if(details->isVisible())missing<<"details shown";if(!details->toPlainText().contains("acknowledgement"))missing<<"receipt";if(!cancel->isDefault())missing<<"default";if(dialog->focusWidget()!=cancel)missing<<"focus";if(dialog->findChild<QPushButton*>("confirmActionButton")->text()!="Delete from destination")missing<<"action";
   dialog->findChild<QToolButton*>("deleteConfirmationDetailsExpanded")->click();if(!details->isVisible())missing<<"disclosure";if(!missing.isEmpty())qWarning("%s",qPrintable(missing.join(", ")));confirmed=missing.isEmpty();QTest::keyClick(dialog,Qt::Key_Escape);});dismiss.start();window.findChild<QPushButton*>("deleteHistoryRemote")->click();PUTGD_TRY_VERIFY(confirmed);PUTGD_TRY_VERIFY(!deletionDialog());QCOMPARE(provider->deletes.load(),0);QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,1);
 }
 void unifiedActivityCancelRetryAndCopyUseResidentActions(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();source.defer=true;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.acquisitions.size(),1);PUTGD_TRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);fixture.client.replyDelayMs["history.list"]=200;window.refresh();putgd::test::settle(30);source.acquisitions.takeFirst()({source.payload,{}});PUTGD_TRY_COMPARE(provider->uploads.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);PUTGD_TRY_COMPARE(list->model()->rowCount(),1);PUTGD_TRY_COMPARE(list->selectionModel()->selectedRows().size(),1);fixture.client.replyDelayMs["history.list"]=0;source.defer=false;auto* retry=window.findChild<QPushButton*>("retryOperation");PUTGD_TRY_VERIFY2(retry->isVisible(),qPrintable(QString("selection=%1 failed=%2 current=%3").arg(list->selectionModel()->selectedRows().size()).arg(list->model()->data(list->model()->index(0,0),HistoryModel::FailedRole).toBool()).arg(list->currentIndex().isValid())));provider->failUpload=false;retry->click();PUTGD_TRY_COMPARE(provider->uploads.load(),2);PUTGD_TRY_COMPARE(source.copies,1);PUTGD_TRY_VERIFY(window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());window.findChild<QPushButton*>("copyHistoryLink")->click();PUTGD_TRY_COMPARE(source.copies,2);QCOMPARE(source.reads,1);
  source.defer=true;operations.invoke("region",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.reads,2);PUTGD_TRY_COMPARE(list->model()->rowCount(),2);selectRow(list,0);auto* cancel=window.findChild<QPushButton*>("cancelOperation");PUTGD_TRY_VERIFY(cancel->isVisible());cancel->click();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);PUTGD_TRY_COMPARE(list->model()->rowCount(),1);auto* show=window.findChild<QToolButton*>("historyShowCancelled");PUTGD_TRY_VERIFY(show->isVisible());show->click();PUTGD_TRY_COMPARE(list->model()->rowCount(),2);QVERIFY(window.saveViewState()["showCancelled"].toBool());QCOMPARE(provider->uploads.load(),2);
 }
 void uploadProgressRendersBeforeCompletionDuringDelayedRecentReplies(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();source.defer=true;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});PUTGD_TRY_COMPARE(source.acquisitions.size(),1);PUTGD_TRY_COMPARE(model->rowCount(),1);PUTGD_TRY_VERIFY(model->data(model->index(0),HistoryModel::ActiveRole).toBool());PUTGD_TRY_VERIFY(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("Reading the clipboard"));selectRow(list,0);
  source.acquisitions.takeFirst()({source.payload,{}});PUTGD_TRY_COMPARE(provider->completions.size(),1);auto report=provider->requests.first().progress;QVERIFY(bool(report));report({"uploading",1048576,2097152});PUTGD_TRY_COMPARE(model->data(model->index(0),HistoryModel::ProgressRole).toInt(),50);QVERIFY(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("50%"));report({"uploading",2097152,2097152});PUTGD_TRY_COMPARE(model->data(model->index(0),HistoryModel::ProgressRole).toInt(),100);QVERIFY(model->data(model->index(0),HistoryModel::ActiveRole).toBool());QCOMPARE(source.copies,0);
  fixture.client.replyDelayMs["operation.recent"]=600;int reports=0;QTimer updates;updates.setInterval(50);connect(&updates,&QTimer::timeout,this,[&]{report({"uploading",1048576+ ++reports,{}});});updates.start();PUTGD_TRY_VERIFY_WITH_TIMEOUT(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("1 MiB transferred"),2500);QVERIFY(reports>10);QCOMPARE(model->data(model->index(0),HistoryModel::ProgressRole).toInt(),-1);QCOMPARE(list->selectionModel()->selectedRows().size(),1);QCOMPARE(model->record(0)["operationId"].toString(),id);QCOMPARE(source.copies,0);
  updates.stop();fixture.client.replyDelayMs["operation.recent"]=0;provider->failUpload=true;provider->completions.takeFirst()();PUTGD_TRY_VERIFY(model->data(model->index(0),HistoryModel::FailedRole).toBool());PUTGD_TRY_VERIFY(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("Synthetic failure"));
 }
 void queuedRetryRetainsOriginalIdentityAndCanBeCancelledFromHistory(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");HistoryStore journal(operations.databasePath());auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);const auto original=journal.operation(id);provider->failUpload=false;operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->completions.size(),1);PUTGD_TRY_VERIFY(operationRow(list,id)>=0);selectRow(list,operationRow(list,id));PUTGD_TRY_VERIFY(window.findChild<QPushButton*>("retryOperation")->isVisible());window.findChild<QPushButton*>("retryOperation")->click();PUTGD_TRY_COMPARE(journal.operation(id).phase,QString("queued"));QCOMPARE(journal.operation(id).destination.toJson(),original.destination.toJson());QCOMPARE(journal.operation(id).items.first().publicationId,original.items.first().publicationId);QCOMPARE(provider->completions.size(),1);
  auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_VERIFY(operationRow(list,id)>=0);PUTGD_TRY_VERIFY(model->data(model->index(operationRow(list,id)),HistoryModel::SubtitleRole).toString().contains("Waiting to upload"));PUTGD_TRY_VERIFY(window.findChild<QPushButton*>("cancelOperation")->isVisible());window.findChild<QPushButton*>("cancelOperation")->click();PUTGD_TRY_COMPARE(journal.operation(id).state,QString("cancelled"));QCOMPARE(provider->completions.size(),1);QCOMPARE(source.reads,2);
 }
 void failedAutomaticCopyCanRetryTheFullResultWithoutAnotherUpload(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));HistoryCopyFailureSource source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),1);selectRow(list,0);PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historyOutcome")->text().contains("Could not copy"));QVERIFY(!model->data(model->index(0)).toString().contains(id));QVERIFY(!model->data(model->index(0),Qt::ToolTipRole).toString().contains(id));QVERIFY(!window.findChild<QLabel*>("historySummary")->text().contains(id));QVERIFY(!window.findChild<QPlainTextEdit*>("historyDetails")->isVisible());source.fail=false;QVERIFY(rowMenuAction(list,"Copy link"));PUTGD_TRY_COMPARE(source.copies,2);PUTGD_TRY_VERIFY(model->data(model->index(0),HistoryModel::SubtitleRole).toString().contains("Link copied"));QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,1);QVERIFY(rowMenuAction(list,"Copy details"));PUTGD_TRY_VERIFY(QApplication::clipboard()->text().contains(id));QVERIFY(!QApplication::clipboard()->text().contains("synthetic source content"));
 }
  // #473: an automatic copy the clipboard change refused says so in the row,
  // the subtitle and the outcome, is not a failure, and Copy link recovers it.
  void suppressedAutomaticCopyNamesTheClipboardChangeAndCopyLinkRecovers(){
   QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();
   QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);
   ++source.revision; // the user copied something else while the upload ran
   provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.copies,0);
   auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),1);
   PUTGD_TRY_COMPARE(model->record(0)["operation"].toObject()["copyStatus"].toString(),QString("suppressed"));const auto row=model->index(0);
   QCOMPARE(HistoryModel::statusText(model->record(0)),QString("Uploaded · Link not copied (clipboard changed). Copy it again."));
   QVERIFY(row.data(HistoryModel::SubtitleRole).toString().contains("Link not copied"));QVERIFY(!row.data(HistoryModel::FailedRole).toBool());QCOMPARE(row.data(HistoryModel::PrimaryActionRole).toString(),QString("Copy link"));
   selectRow(list,0);PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historyOutcome")->text().contains("Link not copied"));
   auto* copy=window.findChild<QPushButton*>("copyHistoryLink");PUTGD_TRY_VERIFY(copy->isEnabled());QCOMPARE(copy->text(),QString("Copy link"));copy->click();
   PUTGD_TRY_COMPARE(source.copies,1);HistoryStore journal(operations.databasePath());QCOMPARE(source.copied.text,journal.operation(id).items.first().receipt.directUrl);QCOMPARE(provider->uploads.load(),1);
   PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("copied"));
   PUTGD_TRY_COMPARE(model->record(0)["operation"].toObject()["copyStatus"].toString(),QString("copied"));
   QCOMPARE(HistoryModel::statusText(model->record(0)),QString("Uploaded · Link copied"));
   QVERIFY(row.data(HistoryModel::SubtitleRole).toString().contains("Link copied"));
   PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historyOutcome")->text().contains("Link copied"));
   QVERIFY(!window.findChild<QLabel*>("historyOutcome")->text().contains("Copy it again"));
  }
 void failedUploadSavesDiagnosticsForThatOperation(){
  // #122: a failed row offers Save diagnostics for this upload, for that one operation.
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});
  auto config=configuration(d.path());QTemporaryDir out;auto preferences=config["preferences"].toObject();preferences["diagnostics"]=QJsonObject{{"debugLogging",false},{"packageFolder",out.path()}};config["preferences"]=preferences;QVERIFY(settings.save(config,{}));
  Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  DiagnosticsBundles bundles(operations,d.filePath("state"));fixture.server.setDiagnosticsBundles(&bundles);QJsonArray notified;fixture.server.setDiagnosticsNotifications([&](const QJsonObject& saved){notified.append(saved);},[](const QString&){return Error{};});
  HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});PUTGD_TRY_COMPARE(provider->uploads.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  PUTGD_TRY_COMPARE(list->model()->rowCount(),1);PUTGD_TRY_VERIFY(list->model()->data(list->model()->index(0,0),HistoryModel::FailedRole).toBool());selectRow(list,0);
  auto* save=window.findChild<QPushButton*>("saveHistoryDiagnostics");QVERIFY(save);PUTGD_TRY_VERIFY(save->isVisible());QCOMPARE(save->text(),QString("Save diagnostics for this upload"));
  save->click();PUTGD_TRY_COMPARE(notified.size(),1);
  const auto path=notified[0].toObject()["path"].toString();QCOMPARE(QFileInfo(path).absolutePath(),QFileInfo(out.path()).absoluteFilePath());QVERIFY(QFileInfo(path).isFile());QCOMPARE(notified[0].toObject()["operations"].toInt(),1);
  // The UI-driving path (ui.historySaveDiagnostics) selects the row by operation ID.
  bool done=false;Error error;QJsonObject result;
  fixture.server.setUiCommand([&](const QString& name,const QJsonObject& params,JsonReply reply){QCOMPARE(name,QString("historySaveDiagnostics"));window.saveDiagnosticsFor(params["id"].toString(),std::move(reply));});
  putgd::test::settle(1100); // a second package in the same second gets a -2 suffix; either is fine
  fixture.server.dispatch({{"schemaVersion",1},{"requestId","ui"},{"session",fixture.server.socketPath()},{"method","ui.historySaveDiagnostics"},{"params",QJsonObject{{"id",id}}}},[&](Error e,QJsonObject r){error=e;result=r;done=true;});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(done,15000);QVERIFY2(!error,qPrintable(error.message));QVERIFY(QFileInfo(result["path"].toString()).isFile());QCOMPARE(notified.size(),2);
  done=false;window.saveDiagnosticsFor("missing-operation",[&](Error e,QJsonObject){error=e;done=true;});QVERIFY(done);QCOMPARE(error.code,QString("history_row_not_found"));
 }
 void localSaveRetryUsesRetainedArtifactWithoutRecaptureOrUpload(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto config=configuration(d.path());const auto blocked=d.filePath("blocked-archive");QVERIFY(!putgd::test::fixture(d.path(),"blocked-archive","synthetic blocker").isEmpty());auto archive=config["archive"].toObject();archive["root"]=blocked;archive["keepUploads"]=true;config["archive"]=archive;QVERIFY(settings.save(config,{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});PUTGD_TRY_COMPARE(provider->uploads.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("Local save failed"));QVERIFY(QFile::remove(blocked));QVERIFY(QDir().mkpath(blocked));QVERIFY(rowMenuAction(list,"Retry local save"));PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("Saved locally"));HistoryStore ledger(operations.databasePath());QVERIFY(QFileInfo::exists(ledger.operation(id).items.first().local.path));QCOMPARE(source.reads,1);QCOMPARE(provider->uploads.load(),1);
 }
 void localOnlyFailureRetriesLocalSaveAndRecoveredResultCanBeCopied(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));auto config=configuration(d.path(),true);const auto blocked=d.filePath("blocked-local-archive");QVERIFY(!putgd::test::fixture(d.path(),"blocked-local-archive","synthetic blocker").isEmpty());auto archive=config["archive"].toObject();archive["root"]=blocked;config["archive"]=archive;QVERIFY(settings.save(config,{}));ProviderRegistry registry;Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();QString id;operations.invoke("clipboard",[&](Error e,QJsonObject value){QVERIFY(!e);id=value["operationId"].toString();});PUTGD_TRY_COMPARE(source.reads,1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),1);selectRow(list,0);PUTGD_TRY_COMPARE(model->data(model->index(0),HistoryModel::PrimaryActionRole).toString(),QString("Retry local save"));auto* retry=window.findChild<QPushButton*>("retryOperation");PUTGD_TRY_COMPARE(retry->text(),QString("Retry local save"));QVERIFY(QFile::remove(blocked));QVERIFY(QDir().mkpath(blocked));retry->click();HistoryStore ledger(operations.databasePath());PUTGD_TRY_COMPARE(ledger.operation(id).items.first().local.status,QString("saved"));QVERIFY(QFileInfo::exists(ledger.operation(id).items.first().local.path));PUTGD_TRY_VERIFY(!model->data(model->index(0),HistoryModel::FailedRole).toBool());PUTGD_TRY_COMPARE(model->data(model->index(0),HistoryModel::PrimaryActionRole).toString(),QString("Copy result"));PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historyOutcome")->text().contains("Saved locally"));auto* copy=window.findChild<QPushButton*>("copyHistoryLink");PUTGD_TRY_COMPARE(copy->text(),QString("Copy result"));PUTGD_TRY_VERIFY(copy->isEnabled());copy->click();PUTGD_TRY_COMPARE(source.copies,1);QCOMPARE(source.copied.kind,QString("text"));QCOMPARE(source.copied.text,QString::fromUtf8(source.payload.bytes));QCOMPARE(source.reads,1);
 }
 void originalImageCopyUsesResidentOriginalNotPreview(){
  // The copy decodes the original in the putgd-ui image worker beside this test. Without it the
  // copy fails once the worker can't start, and copies stays 1 (#284).
  const QFileInfo worker(QCoreApplication::applicationDirPath()+"/putgd-ui");
  QVERIFY2(worker.isExecutable(),"Copy image needs the putgd-ui image worker next to this test: build the putgd-ui target");
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload.kind="image";source.payload.image=QImage(91,57,QImage::Format_RGB32);source.payload.image.fill(Qt::magenta);OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.reads,1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);auto* copy=window.findChild<QPushButton*>("copyHistoryImage");PUTGD_TRY_VERIFY(copy->isEnabled());PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("91×57"));PUTGD_TRY_COMPARE(source.copies,1);QApplication::clipboard()->setText("helper clipboard sentinel");copy->click();PUTGD_TRY_COMPARE(source.copies,2);QCOMPARE(source.copied.kind,QString("image"));const auto original=QImage::fromData(source.copied.encodedImage,"PNG");QCOMPARE(original.size(),QSize(91,57));QCOMPARE(original.pixelColor(0,0),QColor(Qt::magenta));QCOMPARE(QApplication::clipboard()->text(),QString("helper clipboard sentinel"));window.close();QCOMPARE(source.copied.encodedImage.isEmpty(),false);QCOMPARE(source.reads,1);
 }
 void editAgainOpensTheRetainedLocalImageAndHeaderMenuOffersOtherSources(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;source.payload.kind="image";source.payload.image=QImage(40,30,QImage::Format_RGB32);source.payload.image.fill(Qt::cyan);
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();
  QStringList requests;connect(&window,&HistoryWindow::editRequested,this,[&](const QString& from,const QString& path){requests.append(from+"|"+path);});int chooser=0;connect(&window,&HistoryWindow::chooseImageRequested,this,[&]{++chooser;});
  // The other edit sources moved to the main window's Capture region & edit menu (Graphite).
  QVERIFY(!window.findChild<QPushButton*>("historyEditImage"));Q_UNUSED(chooser);
  auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);selectRow(list,0);auto* again=window.findChild<QPushButton*>("editHistoryImage");QVERIFY(again);PUTGD_TRY_VERIFY(again->isEnabled());
  again->click();QCOMPARE(requests.size(),1);QVERIFY(requests.first().startsWith("file|"));const auto path=requests.first().section('|',1);QVERIFY(QFileInfo(path).isAbsolute());
  const QImage retained(path);QCOMPARE(retained.size(),QSize(40,30));QCOMPARE(retained.pixelColor(0,0),QColor(Qt::cyan));
  // Without the retained file the button explains why instead of opening the editor.
  QVERIFY(QFile::remove(path));list->clearSelection();PUTGD_TRY_VERIFY(!again->isEnabled());selectRow(list,0);putgd::test::settle(300);PUTGD_TRY_VERIFY(!again->isEnabled());PUTGD_TRY_VERIFY(again->toolTip().contains("local copy"));
 }
 void failureThatStoredNothingOffersNoRetry(){
  // An edit refused before anything was stored (here, not an image) has no file
  // to upload again. History names the failure and offers no retry.
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  const auto text=d.filePath("notes.txt");{QFile file(text);QVERIFY(file.open(QIODevice::WriteOnly));file.write("not an image");}
  operations.invoke("edit",[](Error,QJsonObject){},{{"source","file"},{"path",text}});PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.show();
  auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),1);
  const auto row=model->index(0);const auto record=model->record(0);
  QVERIFY(row.data(HistoryModel::FailedRole).toBool());QVERIFY(HistoryModel::storedNothing(record));
  QCOMPARE(HistoryModel::failureLabel(record),QString("Edit failed"));QVERIFY(row.data(HistoryModel::PrimaryActionRole).toString().isEmpty());
  QVERIFY(!HistoryModel::statusText(record).contains("Upload"));QVERIFY(HistoryModel::statusText(record).contains("PNG or JPEG"));
  selectRow(list,0);putgd::test::settle(200);PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("retryOperation")->isVisible());
  // Disabled row-menu entries must look disabled in the app theme.
  RedesignTheme::apply(window);QMenu menu(&window);auto* enabled=menu.addAction("Copy details");auto* disabled=menu.addAction("Retry upload");disabled->setEnabled(false);
  menu.popup(window.mapToGlobal(QPoint(20,20)));PUTGD_TRY_VERIFY(menu.isVisible());const auto image=menu.grab().toImage();
  auto brightest=[&](QAction* action){int best=0;const auto r=menu.actionGeometry(action);for(int y=r.top();y<=r.bottom();++y)for(int x=r.left();x<=r.right();++x)best=std::max(best,qGray(image.pixel(x,y)));return best;};
  QVERIFY2(brightest(disabled)+40<brightest(enabled),qPrintable(QString("%1 vs %2").arg(brightest(disabled)).arg(brightest(enabled))));menu.close();
 }
 void offlineSearchUsesOriginalReceiptsAndRemovedLinksAreDisabled() {
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-operation";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="old-item";item.artifact.name="old-image.png";item.artifact.bytes=2147483649LL;item.artifact.mime="image/png";item.artifact.category="images";item.uploadStatus="uploaded";item.remoteStatus="removed";item.receipt.directUrl="https://example.invalid/original.png";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryModel model(fixture.session);UiHistoryQuery query;query.search="old-image";model.setQuery(query);PUTGD_TRY_COMPARE(model.rowCount(),1);QCOMPARE(model.record(0)["receipt"].toObject()["directUrl"].toString(),QString("https://example.invalid/original.png"));QVERIFY(!model.linkAvailable(0));
  HistoryWindow window(fixture.session);window.resize(1000,700);window.show();if(window.width()>1000)for(auto* child:window.findChildren<QWidget*>())if(child->minimumSizeHint().width()>200)qInfo()<<child->metaObject()->className()<<child->objectName()<<child->minimumSizeHint();QVERIFY2(window.height()<=700,"History must preserve a 700px logical-pixel height");QVERIFY2(window.width()<=1000,qPrintable(QString("History controls expanded a 1000px window to %1px").arg(window.width())));auto* table=window.findChild<QListView*>("historyList");QVERIFY(table);PUTGD_TRY_COMPARE(table->model()->rowCount(),1);selectRow(table,0);PUTGD_TRY_VERIFY(!window.findChild<QPushButton*>("copyHistoryLink")->isEnabled());PUTGD_TRY_VERIFY(window.findChild<QLabel*>("historySummary")->text().contains("2 GiB"));QCOMPARE(QJsonDocument::fromJson(window.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().toUtf8()).object()["bytes"].toInteger(),qint64(2147483649LL));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void openLinkUsesTheResidentOpenerForAvailableLinks() {
  // #326: history.openLink opens a stored link as the link-ready notification does.
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="synthetic-links";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();
   for(const auto& [id,remote]:{std::pair{QString("live-item"),QString("present")},std::pair{QString("removed-item"),QString("removed")}}){OperationItem item;item.artifact.id=id;item.artifact.name=id+".png";item.artifact.bytes=10;item.artifact.mime="image/png";item.artifact.category="images";item.uploadStatus="uploaded";item.remoteStatus=remote;item.receipt.directUrl="https://example.invalid/"+id+".png";op.items.append(item);}
   QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);
  auto call=[&](const QString& id)->std::pair<Error,QJsonObject>{Error error;QJsonObject result;bool done=false;fixture.server.dispatch({{"schemaVersion",1},{"requestId","open"},{"session",fixture.server.socketPath()},{"method","history.openLink"},{"params",QJsonObject{{"id",id}}}},[&](Error e,QJsonObject r){error=e;result=r;done=true;});if(!putgd::test::waitFor([&]{return done;},5000))return {{"timed_out","history.openLink did not reply",{}},{}};return {error,result};};
  QCOMPARE(call("live-item").first.code,QString("ui_unavailable"));
  QList<QUrl> opened;bool opens=true;
  fixture.server.setLinkOpener([&](const QUrl& link,std::function<void(const QJsonObject&)> done){opened.append(link);done({{"route",opens?"portal":"launcher"},{"opened",opens}});});
  auto [error,result]=call("live-item");QVERIFY2(!error,qPrintable(error.code));QCOMPARE(result["route"].toString(),QString("portal"));
  QCOMPARE(opened,QList<QUrl>{QUrl("https://example.invalid/live-item.png")});
  QCOMPARE(call("removed-item").first.code,QString("link_unavailable"));QCOMPARE(opened.size(),1);
  opens=false;QCOMPARE(call("live-item").first.code,QString("link_not_opened"));
  QCOMPARE(call("missing-item").first.code.isEmpty(),false);
 }

 // Graphite design pass (2026-10-26): the selected row leaves its primary
 // action to the inspector beside it; without that inspector (gallery,
 // several rows) the row keeps it.
 void selectedRowLeavesItsPrimaryActionToTheInspector(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  for(int i=0;i<2;++i){operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->uploads.load(),i+1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryActionInkProbe probe;HistoryWindow window(fixture.session);window.resize(Graphite::scaled(Graphite::Metric::historySidebar)+Graphite::scaled(700)+300,700);window.show();
  auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),2);list->setStyle(&probe);QCOMPARE(model->data(model->index(0),HistoryModel::PrimaryActionRole).toString(),QString("Copy link"));
  auto painted=[&](int row,QStyle::State state){probe.buttons.clear();QStyleOptionViewItem option;option.initFrom(list);option.rect=list->visualRect(model->index(row));option.state=QStyle::State_Enabled|state;QImage image(option.rect.size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);{QPainter painter(&image);painter.translate(-option.rect.topLeft());list->itemDelegate()->paint(&painter,option,model->index(row));}return probe.buttons.size();};
  selectRow(list,0);PUTGD_TRY_VERIFY(window.findChild<QScrollArea*>("historyInspectorScroll")->isVisible());PUTGD_TRY_VERIFY(window.findChild<QPushButton*>("copyHistoryLink")->isVisible());
  // Rows are laid out again for the narrower list beside the inspector.
  PUTGD_VERIFY_STEADY(list->viewport()->width()<window.width()-Graphite::scaled(Graphite::Metric::historySidebar)-Graphite::scaled(200)&&list->visualRect(model->index(0)).width()==list->viewport()->width()-2*list->spacing()&&list->visualRect(model->index(1)).width()==list->viewport()->width()-2*list->spacing(),20);
  QCOMPARE(painted(0,QStyle::State_Selected|QStyle::State_MouseOver),1); // the row menu only
  QCOMPARE(painted(0,QStyle::State_Selected),0); // its columns, not actions
  // A click where the inline action would be does not copy from the selected
  // row; the same place in an unselected row does.
  QCOMPARE(painted(1,QStyle::State_MouseOver),2);const auto inline1=probe.buttons.first().center()-list->visualRect(model->index(1)).topLeft();
  auto clickAction=[&](int row){QStyleOptionViewItem option;option.initFrom(list);option.rect=list->visualRect(model->index(row));const int heading=model->data(model->index(row),HistoryModel::DayRole).toString().isEmpty()?0:Graphite::scaled(32);const QPointF at=option.rect.topLeft()+inline1+QPoint(0,heading-(model->data(model->index(1),HistoryModel::DayRole).toString().isEmpty()?0:Graphite::scaled(32)));QMouseEvent click(QEvent::MouseButtonRelease,at,list->viewport()->mapToGlobal(at),Qt::LeftButton,Qt::NoButton,Qt::NoModifier);return list->itemDelegate()->editorEvent(&click,model,option,model->index(row));};
  {const int copies=source.copies;QVERIFY(!clickAction(0));putgd::test::settle(50);QCOMPARE(source.copies,copies);QVERIFY(clickAction(1));PUTGD_TRY_COMPARE(source.copies,copies+1);selectRow(list,0);}
  window.findChild<QToolButton*>("historyGridMode")->click();QCOMPARE(painted(0,QStyle::State_Selected),2);window.findChild<QToolButton*>("historyListMode")->click();
  list->selectionModel()->select(QItemSelection(model->index(0),model->index(1)),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);PUTGD_TRY_VERIFY(!window.findChild<QScrollArea*>("historyInspectorScroll")->isVisible());QCOMPARE(painted(0,QStyle::State_Selected),2);
 }
 // Collapsed filters still say what restricts the list, and Clear filters,
 // the empty view's reset and ui.history clear them through the same query.
 void collapsedFiltersShowActiveRestrictionsAndClear(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="filter-operation";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="filter-item";item.artifact.name="filtered.txt";item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1100,700);window.show();auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);
  auto* toggle=window.findChild<QToolButton*>("historyFiltersExpanded");auto* clear=window.findChild<QToolButton*>("historyClearFilters");auto* failed=window.findChild<QToolButton*>("historyFailedOnly");QVERIFY(clear);
  QCOMPARE(toggle->text(),QString("Filters"));QVERIFY(clear->isHidden());QVERIFY(toggle->accessibleDescription().contains("no active filters"));
  failed->click();PUTGD_TRY_COMPARE(toggle->text(),QString("Filters · 1"));QVERIFY(!toggle->isChecked());QVERIFY(clear->isVisible());QVERIFY(toggle->accessibleDescription().startsWith("Collapsed"));QVERIFY(toggle->accessibleDescription().contains("Upload state: failed"));
  // Typed but not applied restricts nothing yet.
  window.findChild<QLineEdit*>("historyFiltermime")->setText("image/png");QCOMPARE(toggle->text(),QString("Filters · 1"));window.findChild<QPushButton*>("historyApplyFilters")->click();QCOMPARE(toggle->text(),QString("Filters · 2"));QVERIFY(toggle->toolTip().contains("Content type: image/png"));
  toggle->click();QVERIFY(toggle->accessibleDescription().startsWith("Expanded"));QCOMPARE(toggle->text(),QString("Filters · 2"));toggle->click();
  clear->click();QCOMPARE(toggle->text(),QString("Filters"));QVERIFY(clear->isHidden());QVERIFY(!failed->isChecked());QCOMPARE(window.findChild<QLabel*>("historyHeading")->text(),QString("All items"));
  {const auto query=window.saveViewState()["query"].toObject();QVERIFY(query["mime"].toString().isEmpty());QVERIFY(query["uploadStatus"].toString().isEmpty());}PUTGD_TRY_COMPARE(list->model()->rowCount(),1);
  // A search with no match says so and offers its reset; an empty history does not.
  auto* search=window.findChild<QLineEdit*>("historySearch");search->setText("no such capture");QTest::keyClick(search,Qt::Key_Return);auto* empty=window.findChild<QLabel*>("historyEmpty");auto* reset=window.findChild<QPushButton*>("historyResetView");
  PUTGD_TRY_VERIFY(empty->isVisible());QCOMPARE(empty->text(),QString("No matching items"));QVERIFY(reset->isVisible());reset->click();QVERIFY(search->text().isEmpty());PUTGD_TRY_COMPARE(list->model()->rowCount(),1);QVERIFY(list->isVisible());
  // The agent path: ui.history through the session, as Clear filters and the reset do.
  fixture.server.setUiCommand([&](const QString& name,const QJsonObject& params,JsonReply reply){QCOMPARE(name,QString("history"));window.command(params,std::move(reply));});
  auto drive=[&](const QJsonObject& params){Error error;QJsonObject result;bool done=false;fixture.server.dispatch({{"schemaVersion",1},{"requestId","ui"},{"session",fixture.server.socketPath()},{"method","ui.history"},{"params",params}},[&](Error e,QJsonObject r){error=e;result=r;done=true;});if(!putgd::test::waitFor([&]{return done;},5000))error={"timeout","ui.history did not answer",{}};return std::pair{error,result};};
  window.findChild<QLineEdit*>("historyFilterremoteStatus")->setText("removed");window.findChild<QPushButton*>("historyApplyFilters")->click();search->setText("filtered");QTest::keyClick(search,Qt::Key_Return);window.findChild<QListWidget*>("historyCategories")->setCurrentRow(1);
  {auto [error,state]=drive({{"action","inspect"}});QVERIFY(!error);QCOMPARE(state["filtersLabel"].toString(),QString("Filters · 1"));QCOMPARE(state["activeFilters"].toArray().size(),1);}
  {auto [error,state]=drive({{"action","clearFilters"}});QVERIFY(!error);QVERIFY(state["activeFilters"].toArray().isEmpty());QCOMPARE(state["query"].toObject()["search"].toString(),QString("filtered"));QCOMPARE(state["query"].toObject()["category"].toString(),QString("images"));QVERIFY(clear->isHidden());}
  {auto [error,state]=drive({{"action","clearFilters"},{"scope","all"}});QVERIFY(!error);QVERIFY(state["query"].toObject()["search"].toString().isEmpty());QVERIFY(state["query"].toObject()["category"].toString().isEmpty());QVERIFY(search->text().isEmpty());QCOMPARE(window.findChild<QListWidget*>("historyCategories")->currentRow(),0);}
  QCOMPARE(drive({{"action","select"}}).first.code,QString("invalid_request"));QCOMPARE(drive({{"action","clearSelection"},{"scope","all"}}).first.code,QString("invalid_request"));
  {auto [error,state]=drive({{"action","select"},{"ids",QJsonArray{"filter-item"}}});QVERIFY(!error);QCOMPARE(state["selectedRows"].toInt(),1);QCOMPARE(list->selectionModel()->selectedRows().size(),1);}
  QCOMPARE(drive({{"action","select"},{"ids",QJsonArray{"missing-item"}}}).first.code,QString("history_row_not_found"));
  {auto [error,state]=drive({{"action","clearSelection"}});QVERIFY(!error);QCOMPARE(state["selectedRows"].toInt(),0);}
  QCOMPARE(source.reads,0);
 }
 // Several selected rows keep their selection when a row menu opens, and the
 // selection strip copies their links and deletes exactly those files.
 void multiSelectionOffersBatchActionsWithExactIds(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  for(int i=0;i<3;++i){operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->uploads.load(),i+1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1100,700);window.show();
  auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),3);auto* bar=window.findChild<QWidget*>("historySelectionBar");QVERIFY(bar->isHidden());
  const QStringList chosen{model->record(0)["id"].toString(),model->record(1)["id"].toString()};const auto kept=model->record(2)["id"].toString();QStringList links;for(int row:{0,1})links<<model->record(row)["receipt"].toObject()["directUrl"].toString();links.sort();QVERIFY(!links.first().isEmpty());
  list->selectionModel()->select(QItemSelection(model->index(0),model->index(1)),QItemSelectionModel::ClearAndSelect|QItemSelectionModel::Rows);list->selectionModel()->setCurrentIndex(model->index(1),QItemSelectionModel::NoUpdate);
  PUTGD_TRY_VERIFY(bar->isVisible());QCOMPARE(window.findChild<QLabel*>("historySelectionCount")->text(),QString("2 selected"));QVERIFY(!window.findChild<QScrollArea*>("historyInspectorScroll")->isVisible());
  for(const char* name:{"historyCopyLinks","historyDeleteSelected","historyClearSelection"})QVERIFY2(window.findChild<QPushButton*>(name)->isVisible(),name);QVERIFY(!window.findChild<QPushButton*>("historyShowDetails")->isVisible());
  // A row menu on one of them keeps both selected and offers the batch.
  QStringList offered;int selectedInMenu=0;QTimer::singleShot(0,list,[&]{for(auto* widget:QApplication::topLevelWidgets())if(auto* menu=qobject_cast<QMenu*>(widget);menu&&menu->isVisible()){for(auto* action:menu->actions())if(!action->isSeparator())offered<<action->text();selectedInMenu=list->selectionModel()->selectedRows().size();menu->close();}});
  emit list->customContextMenuRequested(list->visualRect(model->index(1)).center());
  QVERIFY2(offered.contains("Copy links")&&offered.contains("Delete from destination…")&&offered.contains("Clear selection")&&!offered.contains("Copy details"),qPrintable(offered.join("|")));QCOMPARE(selectedInMenu,2);QCOMPARE(list->selectionModel()->selectedRows().size(),2);
  const int copies=source.copies;window.findChild<QPushButton*>("historyCopyLinks")->click();PUTGD_TRY_COMPARE(source.copies,copies+1);{auto copied=source.copied.text.split('\n');copied.sort();QCOMPARE(copied,links);}
  {DispatchSessionClient external(fixture.server);Error refused;bool done=false;external.request("history.copyLinks",{{"ids",QJsonArray{chosen[0],chosen[0]}}},this,[&](Error e,QJsonObject){refused=e;done=true;});PUTGD_TRY_VERIFY(done);QCOMPARE(refused.code,QString("invalid_request"));}
  QStringList shown;QTimer accept;accept.setInterval(10);connect(&accept,&QTimer::timeout,this,[&]{if(auto* dialog=deletionDialog()){for(auto* label:dialog->findChildren<QLabel*>())shown<<label->text();QVERIFY(dialog->findChild<QPlainTextEdit*>("deleteConfirmationDetails")->toPlainText().contains(chosen[1]));dialog->findChild<QPushButton*>("confirmActionButton")->click();}});accept.start();
  window.findChild<QPushButton*>("historyDeleteSelected")->click();PUTGD_TRY_COMPARE(provider->deletes.load(),2);accept.stop();QVERIFY2(shown.contains("Delete 2 files from their destinations?"),qPrintable(shown.join("|")));
  auto remote=[&](const QString& id){for(int row=0;row<model->rowCount();++row)if(model->record(row)["id"]==id)return model->record(row)["remoteStatus"].toString();return QString{};};
  PUTGD_TRY_VERIFY(remote(chosen[0])=="removed"&&remote(chosen[1])=="removed");QVERIFY(remote(kept)!="removed");
  window.findChild<QPushButton*>("historyClearSelection")->click();QVERIFY(list->selectionModel()->selectedRows().isEmpty());QVERIFY(bar->isHidden());QCOMPARE(source.reads,3);
 }
 // The row menu lists what the row can do now, from the same state as its
 // inline action and the inspector, and never a command the resident refuses.
 void rowMenuOffersOnlyApplicableCommands(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  QString uploaded,failed;operations.invoke("clipboard",[&](Error e,QJsonObject v){QVERIFY(!e);uploaded=v["operationId"].toString();});PUTGD_TRY_COMPARE(provider->uploads.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  provider->failUpload=true;operations.invoke("clipboard",[&](Error e,QJsonObject v){QVERIFY(!e);failed=v["operationId"].toString();});PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);PUTGD_TRY_VERIFY(provider->uploads.load()>=1);
  const auto kept=d.filePath("kept.txt");{QFile file(kept);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic");}
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="menu-local";op.created="2020-01-01T00:00:00.000Z";op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="menu-local-item";item.artifact.name="local.txt";item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";item.local.status="saved";item.local.path=kept;op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1100,700);window.show();auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),3);auto* model=qobject_cast<HistoryModel*>(list->model());
  auto menuFor=[&](const QString& id){selectRow(list,operationRow(list,id));window.activateWindow();list->setFocus();putgd::test::settle(20);QStringList texts;QTimer::singleShot(0,list,[&]{for(auto* widget:QApplication::topLevelWidgets())if(auto* menu=qobject_cast<QMenu*>(widget);menu&&menu->isVisible()){for(auto* action:menu->actions())if(!action->isSeparator()){texts<<action->text();QVERIFY2(action->isEnabled(),qPrintable(action->text()));}menu->close();}});QTest::keyClick(list,Qt::Key_Menu);QCoreApplication::processEvents();return texts;};
  const QStringList never{"Remove from history","Cancel operation"};
  {const auto texts=menuFor(uploaded);QCOMPARE(model->data(model->index(operationRow(list,uploaded)),HistoryModel::PrimaryActionRole).toString(),QString("Copy link"));
   for(const auto& t:QStringList{"Copy link","Open in browser","Copy details","Delete from destination…"})QVERIFY2(texts.contains(t),qPrintable(t+" in "+texts.join("|")));
   for(const auto& t:never+QStringList{"Retry upload","Retry local save","Copy result","Save diagnostics for this upload"})QVERIFY2(!texts.contains(t),qPrintable(t+" in "+texts.join("|")));}
  {const auto texts=menuFor(failed);QCOMPARE(model->data(model->index(operationRow(list,failed)),HistoryModel::PrimaryActionRole).toString(),QString("Retry upload"));
   for(const auto& t:QStringList{"Retry upload","Save diagnostics for this upload","Copy details"})QVERIFY2(texts.contains(t),qPrintable(t+" in "+texts.join("|")));
   for(const auto& t:never+QStringList{"Copy link","Open in browser","Copy result","Retry local save","Delete from destination…"})QVERIFY2(!texts.contains(t),qPrintable(t+" in "+texts.join("|")));
   PUTGD_TRY_VERIFY(window.findChild<QPushButton*>("retryOperation")->isVisible());QCOMPARE(window.findChild<QPushButton*>("retryOperation")->text(),QString("Retry upload"));}
  {const auto texts=menuFor("menu-local");QCOMPARE(model->data(model->index(operationRow(list,"menu-local")),HistoryModel::PrimaryActionRole).toString(),QString("Copy result"));
   QVERIFY2(texts.contains("Copy result"),qPrintable(texts.join("|")));for(const auto& t:never+QStringList{"Retry upload","Retry local save","Copy link","Delete from destination…"})QVERIFY2(!texts.contains(t),qPrintable(t+" in "+texts.join("|")));
   PUTGD_TRY_VERIFY(!window.findChild<QPlainTextEdit*>("historyDetails")->toPlainText().isEmpty());QVERIFY(window.findChild<QPushButton*>("deleteHistoryRemote")->isHidden());}
 }
 // Gallery cards measure their text: the actions follow a complete failure
 // after a 12 px gap, and the preferred width grows with the text size.
 void galleryCardsReserveTextBeforeTheirActions(){
  const QString message="Synthetic upload failure. Check the destination's credentials, then retry the upload from History. "+QString("detail ").repeated(30)+"END";
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());Operation op;op.id="gallery-failed";op.action="region";op.created="2020-01-01T00:00:00.000Z";op.state="failed";op.settings=settings.snapshot();OperationItem item;item.artifact.id="gallery-failed-item";item.artifact.name=QString("A long capture name for the gallery card %1.png").arg(1);item.artifact.mime="image/png";item.artifact.category="images";item.uploadStatus="failed";item.error={"synthetic_failed",message,{}};op.items.append(item);QVERIFY(!ledger.saveOperation(op));}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryActionInkProbe probe;HistoryWindow window(fixture.session);window.resize(1300,800);window.show();auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);
  window.findChild<QToolButton*>("historyGridMode")->click();list->setStyle(&probe);const auto index=list->model()->index(0,0);QCOMPARE(index.data(HistoryModel::PrimaryActionRole).toString(),QString("Retry upload"));
  QStyleOptionViewItem option;option.initFrom(list);const auto size=list->itemDelegate()->sizeHint(option,index);
  // Cards share each row: no wider than the viewport allows, never narrower than the scaled preference allows.
  const int available=list->viewport()->width()-2*list->spacing();QVERIFY2(size.width()>=qMin(Graphite::scaled(248),available-list->spacing())-1,qPrintable(QString("card %1 of %2").arg(size.width()).arg(available)));QVERIFY(available/(size.width()+list->spacing())<=qMax(1,available/Graphite::scaled(248)));
  option.rect=QRect(QPoint(),size);option.state=QStyle::State_Enabled|QStyle::State_MouseOver;QImage image(size,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);{QPainter painter(&image);list->itemDelegate()->paint(&painter,option,index);}
  const auto area=HistoryWindow::failureTextArea(size,true,false);QTextDocument expected;expected.setDocumentMargin(0);expected.setDefaultFont(Graphite::font(Graphite::Type::metadata));auto textOption=expected.defaultTextOption();textOption.setWrapMode(QTextOption::WrapAtWordBoundaryOrAnywhere);expected.setDefaultTextOption(textOption);expected.setPlainText(index.data(Qt::AccessibleTextRole).toString().section(". ",1));expected.setTextWidth(area.width());
  const int textBottom=area.top()+int(std::ceil(expected.size().height()));QCOMPARE(probe.buttons.size(),2);
  for(const auto& button:probe.buttons){QVERIFY2(button.top()>=textBottom+12,qPrintable(QString("action at y%1, text ends y%2").arg(button.top()).arg(textBottom)));QVERIFY(QRect(QPoint(),size).contains(button));QVERIFY(button.height()>=QFontMetrics(Graphite::font()).height()+8);}
 }
 // Column headings and row values share one geometry: at every width, with
 // a scrollbar, each heading is painted where its values start, and a
 // dropped column loses its heading too.
 void columnHeadingsMatchRowColumns(){
  QTemporaryDir d;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));ProviderRegistry registry;Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  {HistoryStore ledger(operations.databasePath());for(int i=0;i<14;++i){Operation op;op.id=QString("column-operation-%1").arg(i);op.created=QString("2020-01-01T00:%1:00.000Z").arg(i,2,10,QChar('0'));op.state="completed";op.settings=settings.snapshot();OperationItem item;item.artifact.id=QString("column-item-%1").arg(i);item.artifact.name=QString("Column %1.txt").arg(i);item.artifact.mime="text/plain";item.artifact.category="text";item.uploadStatus="local_only";op.items.append(item);QVERIFY(!ledger.saveOperation(op));}}
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);window.resize(1000,640);window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));auto* list=window.findChild<QListView*>("historyList");auto* model=qobject_cast<HistoryModel*>(list->model());PUTGD_TRY_COMPARE(model->rowCount(),14);
  auto* header=window.findChild<QWidget*>("historyColumns");QVERIFY(header);QCOMPARE(model->data(model->index(1),HistoryModel::DayRole).toString(),QString());
  auto clusters=[](QList<int> xs){QList<std::pair<int,int>> out;std::sort(xs.begin(),xs.end());for(int x:xs){if(out.isEmpty()||x-out.last().second>Graphite::scaled(10))out.append({x,x});else out.last().second=x;}return out;};
  // Ink against the fill just left of the text body, up to the band's end.
  auto ink=[](const QImage& image,const QRect& band,int from){QList<int> xs;const auto ref=image.pixelColor(from-4,band.top());for(int x=from;x<=band.right();++x)for(int y=band.top();y<=band.bottom();++y){const auto c=image.pixelColor(x,y);if(std::abs(c.red()-ref.red())+std::abs(c.green()-ref.green())+std::abs(c.blue()-ref.blue())>60){xs.append(x);break;}}return xs;};
  int seen[3]={0,0,0};
  for(int width=Graphite::scaled(Graphite::Metric::historySidebar)+360;width<=1500;width+=37){
   window.resize(width,640);PUTGD_TRY_VERIFY(list->verticalScrollBar()->isVisible()&&list->visualRect(model->index(1)).width()==list->viewport()->width()-2*list->spacing());putgd::test::settle(30);
   const auto shot=window.grab().toImage();const QRect head(header->mapTo(&window,QPoint()),header->size());const auto row=list->visualRect(model->index(1)).translated(list->viewport()->mapTo(&window,QPoint()));
   const int bodyLeft=row.left()+6+12+Graphite::Metric::thumbWidth+16;const int lineTop=row.top()+2+Graphite::scaled(14);
   const auto headings=clusters(ink(shot,QRect(head.left(),head.top()+2,row.right()-head.left()-8,head.height()-5),bodyLeft));const auto values=clusters(ink(shot,QRect(row.left(),lineTop,row.width()-8,Graphite::scaled(22)),bodyLeft));
   if(values.size()!=headings.size()+1&&!qEnvironmentVariable("PUTGD_HISTORY_COLUMN_SHOT").isEmpty())shot.save(qEnvironmentVariable("PUTGD_HISTORY_COLUMN_SHOT"));
   QVERIFY2(values.size()==headings.size()+1,qPrintable(QString("width %1: %2 headings, %3 row clusters").arg(width).arg(headings.size()).arg(values.size())));++seen[headings.size()];
   for(int k=0;k<headings.size();++k)QVERIFY2(std::abs(headings[k].first-values[k+1].first)<=1,qPrintable(QString("width %1: heading %2 at x%3, values at x%4").arg(width).arg(k).arg(headings[k].first).arg(values[k+1].first)));
  }
  QVERIFY2(seen[2]>0&&seen[1]+seen[0]>0,"The sweep crosses the column breakpoints");
 }
 // Below the width both panes need at this text size, the inspector opens
 // over the list on request and returns to it, by keyboard and ui.history.
 void compactLayoutOpensDetailsOverTheList(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->uploads.load(),1);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  // About the height the main window's minimum size leaves History.
  HistoryService history(operations);UiSessionFixture fixture(settings,operations,history);HistoryWindow window(fixture.session);const int both=Graphite::scaled(Graphite::Metric::historySidebar)+Graphite::scaled(700);window.resize(both-80,qMax(460,Graphite::scaled(330)));window.show();QVERIFY(putgd::test::waitForWindowExposed(&window));
  auto* list=window.findChild<QListView*>("historyList");PUTGD_TRY_COMPARE(list->model()->rowCount(),1);auto* inspector=window.findChild<QScrollArea*>("historyInspectorScroll");auto* bar=window.findChild<QWidget*>("historySelectionBar");auto* back=window.findChild<QToolButton*>("historyDetailsBack");
  selectRow(list,0);PUTGD_TRY_VERIFY(bar->isVisible());QVERIFY(!inspector->isVisible());QCOMPARE(window.historyState()["layout"].toString(),QString("compact"));QVERIFY(window.findChild<QPushButton*>("historyShowDetails")->isVisible());QCOMPARE(list->model()->index(0,0).data(HistoryModel::PrimaryActionRole).toString(),QString("Copy link"));
  window.activateWindow();list->setFocus();putgd::test::settle(20);QTest::keyClick(list,Qt::Key_Return);
  PUTGD_TRY_VERIFY(inspector->isVisible());QVERIFY(!list->isVisible());QVERIFY(back->isVisible());QVERIFY(!bar->isVisible());QVERIFY(window.historyState()["detailsOpen"].toBool());
  // The compact preview keeps the primary action in view without scrolling.
  auto* copy=window.findChild<QPushButton*>("copyHistoryLink");PUTGD_TRY_VERIFY(copy->isVisible());const QRect inView(copy->mapTo(inspector->viewport(),QPoint()),copy->size());QVERIFY2(inspector->viewport()->rect().contains(inView),qPrintable(QString("Copy link at %1,%2 %3x%4 in %5x%6").arg(inView.x()).arg(inView.y()).arg(inView.width()).arg(inView.height()).arg(inspector->viewport()->width()).arg(inspector->viewport()->height())));
  back->setFocus();QTest::keyClick(back,Qt::Key_Escape);PUTGD_TRY_VERIFY(list->isVisible());QVERIFY(!inspector->isVisible());QCOMPARE(list->selectionModel()->selectedRows().size(),1);
  bool done=false;QJsonObject state;window.command({{"action","openDetails"}},[&](Error e,QJsonObject r){QVERIFY(!e);state=r;done=true;});QVERIFY(done);QVERIFY(state["detailsOpen"].toBool());QVERIFY(inspector->isVisible());
  done=false;window.command({{"action","closeDetails"}},[&](Error e,QJsonObject r){QVERIFY(!e);state=r;done=true;});QVERIFY(done);QVERIFY(!state["detailsOpen"].toBool());QVERIFY(list->isVisible());
  // Wide enough for both panes: side by side again, with no Show details.
  window.resize(both+240,qMax(460,Graphite::scaled(330)));PUTGD_TRY_VERIFY(inspector->isVisible()&&list->isVisible());QVERIFY(!bar->isVisible());QCOMPARE(window.historyState()["layout"].toString(),QString("split"));
 }
};QTEST_MAIN(HistoryGui)
#include "test_history_view.moc"
