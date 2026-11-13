// Text recognition in the resident (#1428), through OperationService's real
// journal, log and copy queue with the fake backend in place of the helper.
#include "fixtures/U2Fixtures.h"
#include "fixtures/FakeTextBackend.h"
#include "fixtures/Waits.h"
#include "core/Log.h"
#include <QDirIterator>
#include <QScopeGuard>
#include <QSignalSpy>

using namespace putgd;using namespace putgd::test;

namespace {
// What the screen "showed": must never be found outside the clipboard and an agent's reply.
const char textCanary[]="CANARY-7f3a-recognized-text";
const char codeCanary[]="CANARY-91bc-code-content";
TextResult recognized() {
 TextResult result;result.text=QStringLiteral("Invoice ")+textCanary;result.lineTops={4};result.words={{QString::fromLatin1(textCanary),QRect(1,4,8,8),90}};
 TextCode code;code.format="QRCode";code.text=codeCanary;code.length=qint64(sizeof(codeCanary)-1);code.quad={{0,12},{4,12},{4,15},{0,15}};result.codes={code};
 result.languagesUsed={"eng"};return result;
}
TextService::Request request(bool copy,bool keepResult) {
 TextService::Request request;request.source="monitor";request.copy=copy;request.keepResult=keepResult;
 request.acquire=[](TextService::Deliver deliver){deliver({},[](Error&){QImage image(32,16,QImage::Format_RGB32);image.fill(Qt::white);return image;});};
 return request;
}
// A desktop that cannot capture a window, for a reason of its own, and would
// deliver pixels for anything it is asked all the same.
class LimitedDesktop final:public SourceAdapter {
public:
 int reads=0;
 quint64 clipboardRevision() const override {return 0;}
 QJsonObject capabilities() const override {
  const QJsonObject available{{"available",true}};
  return {{"capture",QJsonObject{{"window",QJsonObject{{"available",false},{"code","capture_cleanup_pending"},{"error","Log out once to finish an earlier capture"}}},{"monitor",available},{"desktop",available}}}};
 }
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply) override {++reads;ClipboardPayload payload;payload.kind="image";payload.image=QImage(16,8,QImage::Format_RGB32);payload.image.fill(Qt::white);reply({payload,{}});}
 void cancelAcquisition() override {}
 void copy(const CopyPayload&,std::function<void(Error)> reply) override {reply({});}
};
// A resident's services over a private data folder, with the fake backend installed.
struct Resident {
 QTemporaryDir dir{QStringLiteral("/tmp/ts-XXXXXX")};ProviderRegistry registry;Source source;SourceAdapter* desktop=&source;
 std::unique_ptr<SettingsService> settings;std::unique_ptr<OperationService> operations;FakeTextBackend* backend=nullptr;
 bool start() {
  settings=std::make_unique<SettingsService>(dir.filePath("config.json"));if(!settings->save(configuration(dir.path(),true),{}))return false;
  operations=std::make_unique<OperationService>(dir.filePath("data"),*settings,registry,*desktop);
  auto fake=std::make_unique<FakeTextBackend>();backend=fake.get();backend->result=recognized();operations->setTextBackend(std::move(fake));
  return waitFor([&]{return operations->ready();});
 }
 // Starts one recognition and returns its operation ID once it was accepted.
 QString recognize(TextService::Request request) {
  QString id;Error refused;operations->text().recognize(std::move(request),[&](Error error,QJsonObject result){refused=error;id=result["operationId"].toString();});
  return waitFor([&]{return !id.isEmpty()||refused;})?id:QString();
 }
 // The same through the session entry point, with the refusal when there is one.
 QString recognizeFrom(const QJsonObject& params,Error& refused) {
  QString id;bool answered=false;operations->recognizeText(params,[&](Error error,QJsonObject result){refused=error;id=result["operationId"].toString();answered=true;});
  return waitFor([&]{return answered;})?id:QString();
 }
 QJsonObject inspect(const QString& id) {QJsonObject record;operations->text().inspect(id,record);return record;}
 bool ended(const QString& id) {const auto state=inspect(id)["state"].toString();return state=="completed"||state=="failed"||state=="cancelled";}
};
bool anyFileHolds(const QString& root,const QByteArray& needle) {
 QDirIterator files(root,QDir::Files|QDir::Hidden,QDirIterator::Subdirectories);
 while(files.hasNext()){QFile file(files.next());if(file.open(QIODevice::ReadOnly)&&file.readAll().contains(needle))return true;}
 return false;
}
}
class TextServiceTest:public QObject {
 Q_OBJECT
private slots:
 // Recognized text or code content written to the journal, the History database
 // or a log would stay on disk unseen; and a much-used action that kept its rows
 // would grow the journal without limit.
 void completedTextLeavesNoJournalRowAndNothingRecognizedOnDisk() {
  Resident resident;logging::open(resident.dir.filePath("state/logs"));logging::setDebug(true);const auto closeLog=qScopeGuard([]{logging::close();});
  QVERIFY(resident.start());QSignalSpy finished(resident.operations.get(),&OperationService::textFinished);
  const auto id=resident.recognize(request(true,true));QVERIFY(!id.isEmpty());
  PUTGD_TRY_VERIFY(resident.ended(id));
  const auto record=resident.inspect(id);QCOMPARE(record["state"].toString(),QString("completed"));QCOMPARE(record["copyStatus"].toString(),QString("copied"));
  // The clipboard got it, and the agent can read it while it is kept: the canary did travel.
  QVERIFY(resident.source.copied.text.contains(textCanary));QVERIFY(resident.source.copied.text.contains(codeCanary));QVERIFY(record["text"].toString().contains(textCanary));
  // The notification says how much was copied, and nothing of what it was.
  QCOMPARE(finished.size(),1);const auto notification=finished.first().at(2).toString();const auto announced=QJsonDocument(finished.first().at(1).toJsonObject()).toJson();
  QVERIFY(!notification.isEmpty());
  for(const auto* canary:{textCanary,codeCanary}){QVERIFY2(!notification.contains(canary),canary);QVERIFY2(!announced.contains(canary),canary);}
  // No journal row is left, in any state.
  PUTGD_TRY_VERIFY(resident.operations->activeObservations().isEmpty());
  {HistoryStore store(resident.operations->databasePath());Error error;store.operation(id,&error);QCOMPARE(error.code,QString("operation_not_found"));QVERIFY(store.recent(100).isEmpty());}
  // A failed one stays, with its error and nothing else.
  resident.backend->error=textjob::failure("text_job_failed");
  const auto failed=resident.recognize(request(true,false));QVERIFY(!failed.isEmpty());PUTGD_TRY_VERIFY(resident.ended(failed));
  PUTGD_TRY_VERIFY(resident.operations->activeObservations().isEmpty());
  {HistoryStore store(resident.operations->databasePath());const auto kept=store.operation(failed);QCOMPARE(kept.state,QString("failed"));QCOMPARE(kept.error.code,QString("text_job_failed"));QVERIFY(kept.items.isEmpty());}
  // Journal, History database, logs (debug on), diagnostics and the saved configuration.
  logging::flush();
  const auto diagnostics=QJsonDocument(resident.operations->diagnostics()).toJson();
  for(const auto* canary:{textCanary,codeCanary}) {
   QVERIFY2(!anyFileHolds(resident.dir.path(),canary),canary);QVERIFY2(!diagnostics.contains(canary),canary);
   QVERIFY2(!QJsonDocument(resident.inspect(failed)).toJson().contains(canary),canary);
  }
  // The scan itself reads those files: the failed operation's ID is in them.
  QVERIFY(anyFileHolds(resident.dir.path(),failed.toUtf8()));
 }
 // What the user copied while recognition ran is theirs: the text must not replace it.
 void newerUserCopyIsNotOverwritten() {
  Resident resident;QVERIFY(resident.start());resident.backend->hold.store(true);
  const auto id=resident.recognize(request(true,false));QVERIFY(!id.isEmpty());
  PUTGD_TRY_VERIFY(resident.backend->running.load());
  ++resident.source.revision; // the user copies something else
  resident.backend->release();PUTGD_TRY_VERIFY(resident.ended(id));
  const auto record=resident.inspect(id);QCOMPARE(record["state"].toString(),QString("completed"));QCOMPARE(record["copyStatus"].toString(),QString("suppressed"));
  QCOMPARE(resident.source.copies,0);
 }
 // A QR request reserves its revision before receipt refresh, not at dispatch.
 void newerUserCopySuppressesQrDispatch() {
  Resident resident;QVERIFY(resident.start());
  const auto ticket=resident.operations->invalidateCopyEligibility();
  const auto revision=resident.operations->clipboardRevision();
  ++resident.source.revision;
  Error error;bool answered=false;
  resident.operations->copyHistoryQr(QByteArray("synthetic PNG"),[&](Error e,QJsonObject){error=e;answered=true;},ticket,revision);
  QVERIFY(answered);QCOMPARE(error.code,QString("copy_suppressed"));QCOMPARE(resident.source.copies,0);
 }
 // A source this desktop cannot read must answer with the desktop's own reason.
 // Reading another source in its place would copy text the user never pointed at.
 void unavailableSourceReportsItsAdaptersError() {
  LimitedDesktop desktop;Resident resident;resident.desktop=&desktop;QVERIFY(resident.start());
  Error refused;QVERIFY(resident.recognizeFrom({{"source","window"}},refused).isEmpty());
  QCOMPARE(refused.code,QString("capture_cleanup_pending"));QCOMPARE(desktop.reads,0);QCOMPARE(resident.backend->calls.load(),0);
  // A region needs the adapter's own text action, which this one does not report.
  QVERIFY(resident.recognizeFrom({{"source","region"}},refused).isEmpty());QCOMPARE(refused.code,QString("source_unsupported"));QCOMPARE(desktop.reads,0);
  // What it can capture is read: the refusals above are not a dead entry point.
  const auto id=resident.recognizeFrom({{"source","desktop"}},refused);QVERIFY2(!refused,qPrintable(refused.code));QVERIFY(!id.isEmpty());
  PUTGD_TRY_VERIFY(resident.ended(id));QCOMPARE(resident.inspect(id)["state"].toString(),QString("completed"));QCOMPARE(desktop.reads,1);QCOMPARE(resident.backend->calls.load(),1);
 }
 // An agent's result is kept in the resident's memory: one that never left
 // would hold megabytes, and screen text, for as long as the resident runs.
 void keptResultExpires() {
  Resident resident;QVERIFY(resident.start());resident.operations->text().setResultLifetimeForTesting(50);
  const auto id=resident.recognize(request(false,true));QVERIFY(!id.isEmpty());PUTGD_TRY_VERIFY(resident.ended(id));
  QVERIFY(resident.operations->text().cachedBytes()>0);QCOMPARE(resident.source.copies,0);
  PUTGD_TRY_VERIFY(resident.inspect(id)["resultExpired"].toBool());
  QCOMPARE(resident.operations->text().cachedBytes(),qsizetype(0));QVERIFY(!resident.inspect(id).contains("text"));QVERIFY(!resident.inspect(id).contains("words"));
  // The next recognition accepted also ends an earlier result's life.
  resident.operations->text().setResultLifetimeForTesting(TextService::resultLifetimeMs);
  const auto kept=resident.recognize(request(false,true));QVERIFY(!kept.isEmpty());PUTGD_TRY_VERIFY(resident.ended(kept));QVERIFY(!resident.inspect(kept)["resultExpired"].toBool());
  const auto next=resident.recognize(request(false,false));QVERIFY(!next.isEmpty());
  QVERIFY(resident.inspect(kept)["resultExpired"].toBool());PUTGD_TRY_VERIFY(resident.ended(next));
 }
};
QTEST_GUILESS_MAIN(TextServiceTest)
#include "test_text_service.moc"
