#include "fixtures/U2Fixtures.h"
#include "core/Log.h"
#include <QScopeGuard>
#include <utility>
#include <QSignalSpy>
#include <QRegularExpression>
#include <QSqlQuery>
#include <QUuid>
#include <QBuffer>
#include <QElapsedTimer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QSemaphore>
#include <atomic>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include "fixtures/Waits.h"
#endif
using namespace putgd;using namespace putgd::test;
class DeferredClipboardSource final:public SourceAdapter {
public:
 quint64 revision=0;std::optional<quint64> copiedRevision;int reads=0,copies=0;
 QString current="synthetic initial clipboard";CopyPayload proposed;ClipboardPayload payload=textPayload();
 bool workerReply=false,holdImports=false;QList<std::function<void(PayloadResult)>> imports;
 std::function<void(Error)> pending;
 quint64 clipboardRevision()const override{return revision;}
 std::optional<quint64> lastCopyRevision()const override{return copiedRevision;}
 QJsonObject capabilities()const override{return {};}
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override {
  ++reads;if(holdImports)imports.append(std::move(reply));else reply({payload,{}});
 }
 void cancelAcquisition()override{}
 void copy(const CopyPayload& payload,std::function<void(Error)> reply)override {
  QVERIFY2(!pending,"Two native clipboard commits overlapped");++copies;proposed=payload;pending=std::move(reply);
 }
 void acknowledge(Error error={}) {
  QVERIFY(pending);if(!error){current=proposed.text;copiedRevision=++revision;}
  auto reply=std::exchange(pending,{});
  if(workerReply){auto thread=QThread::create([reply,error]{reply(error);});thread->start();thread->wait();delete thread;}else reply(error);
 }
 void invalidateCopyEligibility()override {
  if(pending)std::exchange(pending,{})({"copy_suppressed","Synthetic unsent native commit cancelled",{}});
 }
};
// A destination whose link comes from a separate sharing step after the
// transfer, as Proton Drive's does (#478): uploads of `shareLater` names (or
// all of them) return no link, and share() waits until the test releases it.
class LateLinkProvider final:public StorageProvider {
public:
 OpaqueProvider storage;bool shareAll=false;QSet<QString> shareLater;QObject* dispatcher=nullptr;QList<std::function<void()>> shares;
 ProviderCapabilities capabilities(const Destination& d) const override {auto c=storage.capabilities(d);c.sharing=true;return c;}
 int maximumConcurrentRequests(const Destination& d) const override {return storage.maximumConcurrentRequests(d);}
 QString serializationKey(const Destination& d,const QJsonObject& o,const QString& n) const override {return storage.serializationKey(d,o,n);}
 Error validateSettings(const QJsonObject& s) const override {return storage.validateSettings(s);}
 void checkName(const Destination& d,const QString& n,std::function<void(NameCheck)> cb) override {storage.checkName(d,n,cb);}
 void upload(const UploadRequest& r,std::function<void(UploadResult)> cb) override {
  const bool late=shareAll||shareLater.contains(r.candidate);
  storage.upload(r,[late,cb](UploadResult result){if(late)result.receipt.directUrl.clear();cb(result);});
 }
 void stat(const StorageReceipt& r,std::function<void(StatResult)> cb) override {storage.stat(r,cb);}
 void reconcile(const UploadRequest& r,std::function<void(StatResult)> cb) override {storage.reconcile(r,cb);}
 void remove(const DeleteRequest& r,DurableCheckpoint d,std::function<void(DeleteResult)> cb) override {storage.remove(r,d,cb);}
 void share(const ShareRequest& r,DurableCheckpoint durable,std::function<void(ShareResult)> cb) override {
  durable({{"schemaVersion",1},{"phase","intent"}},[this,r,cb](Error e){
   if(e){cb({"failed",r.receipt,{},e});return;}
   auto complete=[r,cb]{auto receipt=r.receipt;receipt.directUrl="https://example.invalid/shared#"+r.receipt.publicationId;cb({"shared",receipt,{},{}});};
   QMetaObject::invokeMethod(dispatcher,[this,complete]{shares.append(complete);},Qt::QueuedConnection);
  });
 }
};
class OperationsTest:public QObject {
 Q_OBJECT
private slots:
 void historyImageCopiesSelectedUploadedOriginalAndRejectsChangedFiles(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config/config.json"));Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());Operation op;op.id="image-copy-operation";op.state="completed";
  for(int n=0;n<2;++n){QImage image(391,257,QImage::Format_RGB32);image.fill(n?Qt::cyan:Qt::magenta);QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"BMP"));const auto path=d.filePath(QString("original-%1.bmp").arg(n));QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),bytes.size());file.close();OperationItem item;item.artifact.id=QString("image-%1").arg(n);item.artifact.name=QString("original-%1.bmp").arg(n);item.artifact.mime="image/bmp";item.artifact.category="images";item.artifact.originalFile=true;item.artifact.bytes=bytes.size();item.artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());item.local.status="saved";item.local.path=path;item.uploadStatus="uploaded";item.receipt.directUrl="https://example.invalid/original.bmp";op.items.append(item);}
  QVERIFY(!journal.saveOperation(op));const auto before=journal.operation(op.id).toJson();HistoryService history(operations);bool done=false;Error error;history.copyImage("image-1",[&](Error e,QJsonObject result){error=e;QVERIFY(result["copied"].toBool());done=true;});PUTGD_TRY_VERIFY(done);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(source.copies,1);QCOMPARE(source.copied.kind,QString("image"));const auto copied=QImage::fromData(source.copied.encodedImage,"PNG");QCOMPARE(copied.size(),QSize(391,257));QCOMPARE(copied.pixelColor(0,0),QColor(Qt::cyan));QCOMPARE(source.reads,0);QCOMPARE(journal.operation(op.id).toJson(),before);
  auto rejected=[&](QString expected){done=false;history.copyImage("image-1",[&](Error e,QJsonObject){error=e;done=true;});PUTGD_TRY_VERIFY(done);QCOMPARE(error.code,expected);QCOMPARE(source.copies,1);};
  QFile changed(op.items[1].local.path);QVERIFY(changed.open(QIODevice::WriteOnly));QCOMPARE(changed.write(QByteArray(op.items[1].artifact.bytes,'x')),op.items[1].artifact.bytes);changed.close();rejected("artifact_changed");QVERIFY(changed.open(QIODevice::WriteOnly));QVERIFY(changed.write("changed")>0);changed.close();rejected("artifact_changed");QVERIFY(changed.open(QIODevice::WriteOnly));QVERIFY(changed.resize(32*1024*1024+1));changed.close();rejected("image_limit");QVERIFY(changed.remove());rejected("artifact_missing");
#ifdef Q_OS_UNIX
  QVERIFY(QFile::link(op.items[0].local.path,changed.fileName()));rejected("artifact_missing");QVERIFY(QFile::remove(changed.fileName()));QVERIFY(::mkfifo(QFile::encodeName(changed.fileName()).constData(),0600)==0);
  const auto fifo=QFile::encodeName(changed.fileName());std::thread rescue([fifo]{std::this_thread::sleep_for(std::chrono::milliseconds(1000));const int fd=::open(fifo.constData(),O_WRONLY|O_NONBLOCK);if(fd>=0)::close(fd);});QElapsedTimer elapsed;elapsed.start();rejected("artifact_missing");const auto milliseconds=elapsed.elapsed();rescue.join();QVERIFY(milliseconds<750);QVERIFY(QFile::remove(changed.fileName()));
#endif
 }
 void historyImageCopiesSixteenBitOriginalAboveFourBytePixelBudget(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config/config.json"));Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  QImage original(3001,3001,QImage::Format_RGBA64);const auto color=QColor::fromRgba64(12345,23456,34567,45678);original.fill(color);QVERIFY(original.sizeInBytes()>64*1024*1024);QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(original.save(&buffer,"PNG"));QVERIFY(bytes.size()<32*1024*1024);QFile file(d.filePath("original.png"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),bytes.size());file.close();
  Operation op;op.id="sixteen-bit-original";OperationItem item;item.artifact.id="image";item.artifact.mime="image/png";item.artifact.bytes=bytes.size();item.artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());item.local.status="saved";item.local.path=file.fileName();op.items={item};HistoryStore journal(operations.databasePath());QVERIFY(!journal.saveOperation(op));bool done=false;Error error;operations.copyHistoryImage("image",[&](Error e,QJsonObject){error=e;done=true;});PUTGD_TRY_VERIFY_WITH_TIMEOUT(done,15000);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(source.copies,1);const auto copied=QImage::fromData(source.copied.encodedImage,"PNG");QCOMPARE(copied.size(),original.size());QCOMPARE(copied.depth(),64);QCOMPARE(copied.pixelColor(0,0).rgba64(),color.rgba64());QCOMPARE(copied.pixelColor(3000,3000).rgba64(),color.rgba64());
 }
 void historyImagePreparationHonorsClipboardAndInvocationChanges_data(){QTest::addColumn<bool>("newCopy");QTest::newRow("clipboard-owner")<<false;QTest::newRow("newer-copy")<<true;}
 void historyImagePreparationHonorsClipboardAndInvocationChanges(){
  QFETCH(bool,newCopy);QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config/config.json"));Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QImage image(31,23,QImage::Format_RGB32);image.fill(Qt::blue);QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"PNG"));QFile file(d.filePath("original.png"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),bytes.size());file.close();Operation op;op.id="superseded-image";OperationItem item;item.artifact.id="image";item.artifact.mime="image/png";item.artifact.bytes=bytes.size();item.artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());item.local.status="saved";item.local.path=file.fileName();op.items={item};HistoryStore journal(operations.databasePath());QVERIFY(!journal.saveOperation(op));bool done=false;Error error;operations.copyHistoryImage("image",[&](Error e,QJsonObject){error=e;done=true;});bool newerDone=false;if(newCopy)operations.copyHistoryUrl("https://example.invalid/newer",[&](Error e,QJsonObject){QVERIFY(!e);newerDone=true;});else ++source.revision;PUTGD_TRY_VERIFY(done);QCOMPARE(error.code,QString("copy_suppressed"));if(newCopy){PUTGD_TRY_VERIFY(newerDone);QCOMPARE(source.copied.text,QString("https://example.invalid/newer"));}QCOMPARE(source.copies,newCopy?1:0);
 }
 void historyImageQueuedAfterCommittedCopyRebasesOnlyResidentAcknowledgements_data(){QTest::addColumn<bool>("externalChange");QTest::newRow("resident-ack")<<false;QTest::newRow("external-owner-after-ack")<<true;}
 void historyImageQueuedAfterCommittedCopyRebasesOnlyResidentAcknowledgements(){
  QFETCH(bool,externalChange);class CommittedSource final:public SourceAdapter {public:
   quint64 revision=0;std::optional<quint64> copiedRevision;int copies=0,reads=0;CopyPayload proposed;std::function<void(Error)> pending;
   quint64 clipboardRevision()const override{return revision;}std::optional<quint64> lastCopyRevision()const override{return copiedRevision;}QJsonObject capabilities()const override{return {};}
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override{++reads;}void cancelAcquisition()override{}
   void copy(const CopyPayload& payload,std::function<void(Error)> reply)override{QVERIFY(!pending);++copies;proposed=payload;pending=std::move(reply);}
   void acknowledge(){QVERIFY(pending);copiedRevision=++revision;std::exchange(pending,{})({});}
  } source;
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config/config.json"));OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QImage image(31,23,QImage::Format_RGB32);image.fill(Qt::cyan);QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"PNG"));QFile file(d.filePath("original.png"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),bytes.size());file.close();Operation op;op.id="queued-image";OperationItem item;item.artifact.id="image";item.artifact.mime="image/png";item.artifact.bytes=bytes.size();item.artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());item.local.status="saved";item.local.path=file.fileName();op.items={item};HistoryStore journal(operations.databasePath());QVERIFY(!journal.saveOperation(op));
  bool olderDone=false,imageDone=false;Error imageError;operations.copyHistoryUrl("https://example.invalid/older",[&](Error e,QJsonObject){QVERIFY(!e);olderDone=true;});QCOMPARE(source.copies,1);QVERIFY(source.pending);operations.copyHistoryImage("image",[&](Error e,QJsonObject){imageError=e;imageDone=true;});source.acknowledge();if(externalChange)++source.revision;PUTGD_TRY_VERIFY(olderDone);
  if(externalChange){PUTGD_TRY_VERIFY(imageDone);QCOMPARE(imageError.code,QString("copy_suppressed"));QCOMPARE(source.copies,1);}else{PUTGD_TRY_COMPARE(source.copies,2);QVERIFY(source.pending);QCOMPARE(source.proposed.kind,QString("image"));const auto copied=QImage::fromData(source.proposed.encodedImage,"PNG");QCOMPARE(copied.size(),QSize(31,23));QCOMPARE(copied.pixelColor(0,0),QColor(Qt::cyan));source.acknowledge();PUTGD_TRY_VERIFY(imageDone);QVERIFY2(!imageError,qPrintable(imageError.code));}QCOMPARE(source.reads,0);
 }
 void historyImageSupersededWorkerReleasesNewerCopy(){
#ifndef PUTGD_IMAGE_JOB_FIXTURE
  QSKIP("The controlled Unix image-worker fixture is unavailable on this platform");
#else
  if(qEnvironmentVariable("PUTGD_TEST_IMAGE_COPY_CHILD")!="1"){
   QTemporaryDir stage;QVERIFY(stage.isValid());const auto probe=stage.filePath("copy-probe"),worker=stage.filePath("putgd-ui"),record=stage.filePath("worker.json");QVERIFY(QFile::copy(QCoreApplication::applicationFilePath(),probe));QVERIFY(QFile::copy(QString::fromUtf8(PUTGD_IMAGE_JOB_FIXTURE),worker));QVERIFY(QFile::setPermissions(probe,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));QVERIFY(QFile::setPermissions(worker,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));QProcess child;auto environment=QProcessEnvironment::systemEnvironment();environment.insert("PUTGD_TEST_IMAGE_COPY_CHILD","1");environment.insert("PUTGD_TEST_IMAGE_JOB_MODE","cancel-stall");environment.insert("PUTGD_TEST_IMAGE_JOB_RECORD",record);child.setProcessEnvironment(environment);child.start(probe,{"historyImageSupersededWorkerReleasesNewerCopy"});PUTGD_TRY_COMPARE_WITH_TIMEOUT(child.state(),QProcess::NotRunning,20000);QVERIFY2(child.exitStatus()==QProcess::NormalExit&&child.exitCode()==0,qPrintable(QString::fromUtf8(child.readAllStandardOutput()+child.readAllStandardError())));return;
  }
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config/config.json"));Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QImage image(31,23,QImage::Format_RGB32);image.fill(Qt::cyan);QByteArray bytes;QBuffer buffer(&bytes);QVERIFY(buffer.open(QIODevice::WriteOnly));QVERIFY(image.save(&buffer,"PNG"));QFile file(d.filePath("original.png"));QVERIFY(file.open(QIODevice::WriteOnly));QCOMPARE(file.write(bytes),bytes.size());file.close();Operation op;op.id="cancelled-image";OperationItem item;item.artifact.id="image";item.artifact.mime="image/png";item.artifact.bytes=bytes.size();item.artifact.sha256=QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());item.local.status="saved";item.local.path=file.fileName();op.items={item};HistoryStore journal(operations.databasePath());QVERIFY(!journal.saveOperation(op));bool imageDone=false,newerDone=false;Error imageError;operations.copyHistoryImage("image",[&](Error e,QJsonObject){imageError=e;imageDone=true;});const auto recordPath=qEnvironmentVariable("PUTGD_TEST_IMAGE_JOB_RECORD");PUTGD_TRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(recordPath),5000);QFile recordFile(recordPath);QVERIFY(recordFile.open(QIODevice::ReadOnly));const auto record=QJsonDocument::fromJson(recordFile.readAll()).object();QVERIFY(record["jobDirectory"].toString().startsWith("/tmp/pgi-"));QElapsedTimer elapsed;elapsed.start();operations.copyHistoryUrl("https://example.invalid/newer",[&](Error e,QJsonObject){QVERIFY(!e);newerDone=true;});PUTGD_TRY_VERIFY_WITH_TIMEOUT(newerDone,3000);QVERIFY(elapsed.elapsed()<2500);PUTGD_TRY_VERIFY(imageDone);QCOMPARE(imageError.code,QString("copy_suppressed"));QCOMPARE(source.copies,1);QCOMPARE(source.copied.text,QString("https://example.invalid/newer"));QCOMPARE(source.reads,0);QVERIFY(!QFileInfo::exists(record["jobDirectory"].toString()));QVERIFY(!QFileInfo::exists("/proc/"+QString::number(record["pid"].toInteger())));
#endif
 }
 void destinationAndLocalCopiesAreIndependent_data() {
  QTest::addColumn<bool>("destination");QTest::addColumn<bool>("localCopy");QTest::addColumn<bool>("inherit");QTest::addColumn<bool>("unconfiguredUpload");
  for(const bool inherit:{false,true})for(const bool destination:{false,true})for(const bool localCopy:{false,true})QTest::newRow(qPrintable(QString("destination-%1-local-%2-inherit-%3").arg(destination).arg(localCopy).arg(inherit)))<<destination<<localCopy<<inherit<<false;
  for(const bool localCopy:{false,true})QTest::newRow(localCopy?"unconfigured-with-local":"unconfigured-without-local")<<false<<localCopy<<false<<true;
 }
 void destinationAndLocalCopiesAreIndependent() {
  QFETCH(bool,destination);QFETCH(bool,localCopy);QFETCH(bool,inherit);QFETCH(bool,unconfiguredUpload);
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(d.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=localCopy;doc["archive"]=archive;
  QJsonObject output{{"destination",destination||unconfiguredUpload?"upload":"local"},{"profileId",destination?QJsonValue("test"):QJsonValue(QJsonValue::Null)},{"publicationMode","default"}};
  auto actions=doc["actions"].toObject();for(const auto& name:Settings::actionNames()){auto action=actions[name].toObject();action["destination"]=inherit?QJsonValue("default"):output["destination"];action["profileId"]=inherit?QJsonValue(QJsonValue::Null):output["profileId"];actions[name]=action;}doc["actions"]=actions;if(inherit)doc["captureDefaults"]=output;QVERIFY(settings.save(doc,{}));
  Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QString id;Error error;bool replied=false;operations.invoke("clipboard",[&](Error e,QJsonObject result){error=e;id=result["operationId"].toString();replied=true;});PUTGD_TRY_VERIFY(replied);
  if(!destination&&!localCopy){QCOMPARE(error.code,QString("output_required"));QVERIFY(id.isEmpty());QCOMPARE(source.reads,0);QCOMPARE(provider->uploads.load(),0);QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),0);return;}
  QVERIFY2(!error,qPrintable(error.message));QVERIFY(!id.isEmpty());HistoryStore journal(operations.databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);const auto op=journal.operation(id);QCOMPARE(op.items.size(),1);const auto item=op.items.first();QCOMPARE(provider->uploads.load(),destination?1:0);QCOMPARE(item.uploadStatus,destination?QString("uploaded"):QString("local_only"));QCOMPARE(item.local.status=="saved",localCopy);if(localCopy){QFile file(item.local.path);QVERIFY(file.open(QIODevice::ReadOnly));QCOMPARE(file.readAll(),source.payload.bytes);}
 }
 void pendingUploadsRetainTheirConfiguredClipboardMode_data() {QTest::addColumn<QString>("firstMode");QTest::newRow("each-then-list")<<QString("each_link");QTest::newRow("list-then-each")<<QString("list");}
 void pendingUploadsRetainTheirConfiguredClipboardMode() {
  QFETCH(QString,firstMode);const QString secondMode=firstMode=="list"?"each_link":"list";QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]=firstMode;doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));Source source;source.payload.kind="files";for(int i=0;i<2;++i)source.payload.files.append(fixture(d.path(),QString("mode-%1.txt").arg(i),"synthetic batch"));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});QString first,second;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);
  prefs["uploadClipboardBehavior"]=secondMode;doc["preferences"]=prefs;QVERIFY(settings.save(doc,settings.revision()));operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.reads,2);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(provider->completions.size(),1);if(firstMode=="each_link")PUTGD_TRY_COMPARE(source.copies,1);else QCOMPARE(source.copies,0);
  provider->completions.takeFirst()();const int firstCopies=firstMode=="each_link"?2:1;PUTGD_TRY_COMPARE(journal.operation(first).copyStatus,QString("copied"));QCOMPARE(source.copies,firstCopies);
  PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(provider->completions.size(),1);if(secondMode=="each_link")PUTGD_TRY_COMPARE(source.copies,firstCopies+1);else QCOMPARE(source.copies,firstCopies);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.copies,3);QCOMPARE(provider->uploads.load(),4);QCOMPARE(journal.operation(first).settings.uploadClipboardBehavior,firstMode);QCOMPARE(journal.operation(second).settings.uploadClipboardBehavior,secondMode);
 }
 void cancellingBeforeNativeCommitDoesNotWriteTheLink() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));DeferredClipboardSource source;
  source.payload.kind="files";for(int i=0;i<2;++i)source.payload.files.append(fixture(d.path(),QString("cancel-%1.txt").arg(i),"synthetic batch"));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();if(source.pending)source.acknowledge();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);PUTGD_TRY_COMPARE(provider->completions.size(),1);
  operations.cancel(id,[](Error e,QJsonObject){QVERIFY(!e);});if(source.pending)source.acknowledge();provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.current,QString("synthetic initial clipboard"));QCOMPARE(source.revision,quint64(0));QCOMPARE(journal.operation(id).state,QString("cancelled"));QCOMPARE(journal.operation(id).copyStatus,QString("no_result"));QVERIFY(journal.operation(id).items.first().linkAvailable());
 }
 void cancelledListMemberIsRemovedFromAnUnsentNativeCommit() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]="list";doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));DeferredClipboardSource source;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();if(source.pending)source.acknowledge();});QStringList ids;
  for(int i=0;i<2;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  for(int i=0;i<2;++i){PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();}PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);
  operations.cancel(ids.first(),[](Error e,QJsonObject){QVERIFY(!e);});if(source.pending)source.acknowledge();
  PUTGD_TRY_VERIFY(source.copies==2||operations.diagnostics()["activeOperations"].toInt()==0);if(source.pending)source.acknowledge();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.current,journal.operation(ids[1]).items.first().receipt.directUrl);QCOMPARE(journal.operation(ids[0]).copyStatus,QString("no_result"));QCOMPARE(journal.operation(ids[1]).copyStatus,QString("copied"));QCOMPARE(source.revision,quint64(1));
 }
 void historyLinkCopyOnlyRecordsSuccessfulSingleItemRecovery_data(){
  QTest::addColumn<int>("items");QTest::addColumn<QString>("status");QTest::addColumn<QString>("errorCode");
  QTest::newRow("suppressed")<<1<<QString("suppressed")<<QString{};
  QTest::newRow("failed")<<1<<QString("failed")<<QString{};
  QTest::newRow("native-failure")<<1<<QString("failed")<<QString("clipboard_unavailable");
  QTest::newRow("native-suppression")<<1<<QString("suppressed")<<QString("copy_suppressed");
  QTest::newRow("batch-subset")<<2<<QString("partial")<<QString{};
 }
 void historyLinkCopyOnlyRecordsSuccessfulSingleItemRecovery(){
  QFETCH(int,items);QFETCH(QString,status);QFETCH(QString,errorCode);
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));DeferredClipboardSource source;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);HistoryStore journal(operations.databasePath());
  Operation op;op.id="stored-link";op.state="completed";op.copyStatus=status;
  for(int i=0;i<items;++i){OperationItem item;item.artifact.id=QString("link-%1").arg(i);item.uploadStatus="uploaded";item.receipt.directUrl=QString("https://example.invalid/link-%1").arg(i);op.items.append(item);}
  QVERIFY(!journal.saveOperation(op));const auto original=journal.operation(op.id).toJson();const auto detail=journal.detail("link-0");
  bool done=false;Error error;QJsonObject result;history.copyLink("link-0",[&](Error e,QJsonObject value){error=e;result=value;done=true;});
  PUTGD_TRY_VERIFY(bool(source.pending));QCOMPARE(journal.operation(op.id).copyStatus,status);
  source.acknowledge(errorCode.isEmpty()?Error{}:Error{errorCode,"Synthetic copy refusal",{}});PUTGD_TRY_VERIFY(done);
  QCOMPARE(error.code,errorCode);QCOMPARE(result["copied"].toBool(),errorCode.isEmpty());
  auto expected=original;if(items==1&&errorCode.isEmpty())expected["copyStatus"]="copied";
  QCOMPARE(journal.operation(op.id).toJson(),expected);QCOMPARE(journal.detail("link-0"),detail);
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,1);
 }
 void resumedCopiesRecheckDurableRemoval_data() {QTest::addColumn<QString>("mode");QTest::newRow("each-link")<<QString("each_link");QTest::newRow("list")<<QString("list");}
 void resumedCopiesRecheckDurableRemoval() {
  QFETCH(QString,mode);QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]=mode;doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));DeferredClipboardSource source;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryService history(operations);HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();for(auto imported:source.imports)imported({textPayload(),{}});source.imports.clear();if(source.pending)source.acknowledge();});QStringList ids;
  for(int i=0;i<2;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  for(int i=0;i<2;++i){PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();}PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);PUTGD_TRY_COMPARE(journal.operation(ids[1]).state,QString("completed"));
  source.holdImports=true;QString third;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);third=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.imports.size(),1);QVERIFY(!source.pending);
  const QStringList removed{journal.operation(ids[0]).items.first().artifact.id};QJsonObject preview;history.prepareDelete(removed,[&](Error e,QJsonObject r){QVERIFY(!e);preview=r;});PUTGD_TRY_VERIFY(!preview.isEmpty());QVERIFY(!preview["acknowledgement"].toString().isEmpty());history.deleteConfirmed(removed,preview["acknowledgement"].toString(),[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(journal.detail(removed.first())["remoteStatus"].toString(),QString("removed"));
  source.imports.takeFirst()({textPayload(),{}});PUTGD_TRY_COMPARE(source.copies,2);QVERIFY(source.pending);QCOMPARE(source.proposed.text,journal.operation(ids[1]).items.first().receipt.directUrl);source.acknowledge();
  PUTGD_TRY_COMPARE(journal.operation(ids[0]).copyStatus,QString("no_result"));PUTGD_TRY_COMPARE(journal.operation(ids[1]).copyStatus,QString("copied"));PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(source.copies,3);source.acknowledge();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.current,journal.operation(third).items.first().receipt.directUrl);QCOMPARE(source.revision,quint64(2));QCOMPARE(provider->deletes.load(),1);
 }
 void laterCopyFailureRetainsPublishedLinksAndRecordsPartialStatus() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));DeferredClipboardSource source;
  source.payload.kind="files";for(int i=0;i<2;++i)source.payload.files.append(fixture(d.path(),QString("copy-error-%1.txt").arg(i),"synthetic batch"));OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());auto drain=qScopeGuard([&]{if(source.pending)source.acknowledge();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);source.acknowledge();PUTGD_TRY_COMPARE(source.copies,2);source.acknowledge({"clipboard_write_failed","Synthetic native copy failure",{}});PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  const auto op=journal.operation(id);QCOMPARE(op.copyStatus,QString("partial"));QCOMPARE(op.error.code,QString("clipboard_write_failed"));QCOMPARE(op.state,QString("completed"));QCOMPARE(op.items.size(),2);for(const auto& item:op.items)QVERIFY(item.linkAvailable());QCOMPARE(provider->uploads.load(),2);QCOMPARE(source.current,op.items.first().receipt.directUrl);
 }
 void deferredCopiesSerializeAndKeepPostCommitUserEdits_data() {
  QTest::addColumn<bool>("userChanged");QTest::addColumn<bool>("offThread");QTest::newRow("own-copies")<<false<<false;QTest::newRow("user-edit-before-callback-dispatch")<<true<<false;QTest::newRow("worker-reply")<<false<<true;QTest::newRow("worker-reply-with-user-edit")<<true<<true;
 }
 void deferredCopiesSerializeAndKeepPostCommitUserEdits() {
  QFETCH(bool,userChanged);QFETCH(bool,offThread);QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));DeferredClipboardSource source;source.workerReply=offThread;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();if(source.pending)source.acknowledge();});QStringList ids;
  for(int i=0;i<2;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  // The uploads share one name and its object lock, which either operation may take first
  // (#289): follow the operations in the order their uploads finish.
  QStringList finished;
  for(int i=0;i<2;++i) {
   PUTGD_TRY_COMPARE(provider->completions.size(),1);
   QString id;for(const auto& candidate:ids)if(provider->requests.last().artifact.path.contains(candidate))id=candidate;
   QVERIFY(!id.isEmpty());QVERIFY(!finished.contains(id));finished.append(id);provider->completions.takeFirst()();
   if(i==0){PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);}
   else {PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));QCOMPARE(source.copies,1);}
  }
  source.acknowledge();
  if(userChanged){++source.revision;source.current="synthetic user edit";}
  else {PUTGD_TRY_COMPARE(source.copies,2);source.acknowledge();}
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(journal.operation(finished[0]).copyStatus,QString("copied"));QCOMPARE(journal.operation(finished[1]).copyStatus,QString(userChanged?"suppressed":"copied"));
  QCOMPARE(source.copies,userChanged?1:2);QCOMPARE(source.current,userChanged?QString("synthetic user edit"):journal.operation(finished[1]).items.first().receipt.directUrl);
 }
 void newImportPausesAndResumesAnUnsentLinkCopy() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));DeferredClipboardSource source;
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();if(source.pending)source.acknowledge();});QString first,second;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(source.copies,1);QVERIFY(source.pending);
  source.holdImports=true;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.imports.size(),1);
  QCOMPARE(source.current,QString("synthetic initial clipboard"));QVERIFY(!source.pending);QCOMPARE(source.copies,1);
  source.imports.takeFirst()({textPayload(),{}});PUTGD_TRY_COMPARE(source.copies,2);source.acknowledge();PUTGD_TRY_COMPARE(journal.operation(first).copyStatus,QString("copied"));
  PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(source.copies,3);source.acknowledge();
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(journal.operation(second).copyStatus,QString("copied"));QCOMPARE(provider->uploads.load(),2);
 }
 // #473: a refused automatic copy leaves one putgd.clipboard record naming the
 // guard and the revisions it compared, never the link or the clipboard text;
 // the explicit copy afterwards is recorded too.
 void suppressedAutomaticCopyIsLoggedWithItsGuard() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  logging::open(d.filePath("state/logs"));auto closeLog=qScopeGuard([]{logging::close();});
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);
  ++source.revision; // the user copied something else while the upload ran
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("suppressed"));PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.copies,0);
  const auto records=[&](const QString& message){logging::flush();QList<QJsonObject> found;const QDir logs(d.filePath("state/logs"));for(const auto& name:logs.entryList({"putgd-*.jsonl"},QDir::Files)){QFile f(logs.filePath(name));if(!f.open(QIODevice::ReadOnly))continue;for(const auto& line:f.readAll().split('\n')){const auto object=QJsonDocument::fromJson(line).object();if(object["category"]=="putgd.clipboard"&&object["operationId"]==id&&object["message"]==message)found.append(object);}}return found;};
  auto automatic=records("Automatic copy finished");QCOMPARE(automatic.size(),1);const auto record=automatic.first();
  QCOMPARE(record["level"].toString(),QString("info"));const auto fields=record["fields"].toObject();
  QCOMPARE(fields["copyStatus"].toString(),QString("suppressed"));QVERIFY(fields["automatic"].toBool());QCOMPARE(fields["links"].toInt(),1);QCOMPARE(fields["behavior"].toString(),QString("each_link"));QVERIFY(!fields.contains("error"));
  const auto suppression=fields["suppression"].toObject();QCOMPARE(suppression["guard"].toString(),QString("clipboard_changed"));QCOMPARE(suppression["revisionAtInvocation"].toInt(),0);QCOMPARE(suppression["revisionAtDispatch"].toInt(),1);QCOMPARE(suppression["epochAtInvocation"].toInt(),suppression["epochAtDispatch"].toInt());
  const auto text=QString::fromUtf8(QJsonDocument(record).toJson());QVERIFY(!text.contains(journal.operation(id).items.first().receipt.directUrl));QVERIFY(!text.contains("synthetic source content"));
  bool copied=false;operations.copyResult(id,[&](Error e,QJsonObject){QVERIFY2(!e,qPrintable(e.code));copied=true;});PUTGD_TRY_VERIFY(copied);QCOMPARE(source.copies,1);PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("copied"));
  const auto explicitCopies=records("Copy finished");QCOMPARE(explicitCopies.size(),1);QCOMPARE(explicitCopies.first()["fields"].toObject()["copyStatus"].toString(),QString("copied"));QVERIFY(!explicitCopies.first()["fields"].toObject()["automatic"].toBool());
 }
 void explicitHistoryCopySupersedesPendingUploadList() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]="list";doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});QStringList ids;
  for(int i=0;i<2;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  // Either operation may take the shared name's object lock first (as in queuedUploadsCopyEveryLink).
  PUTGD_TRY_COMPARE(provider->completions.size(),1);const auto owner=provider->requests.last().artifact.path.contains(ids[1])?ids[1]:ids[0];QVERIFY(provider->requests.last().artifact.path.contains(owner));
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(journal.operation(owner).state,QString("completed"));QCOMPARE(source.copies,0);
  bool replied=false;operations.copyHistoryUrl("https://example.invalid/explicit-history",[&](Error e,QJsonObject){QVERIFY(!e);replied=true;});PUTGD_TRY_VERIFY(replied);
  PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.copies,1);QCOMPARE(source.copied.text,QString("https://example.invalid/explicit-history"));for(const auto& id:ids){QCOMPARE(journal.operation(id).copyStatus,QString("suppressed"));QVERIFY(journal.operation(id).items.first().linkAvailable());}
 }
 void explicitActivityCopyCanOverrideACompletedListMember() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]="list";doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});QStringList ids;
  for(int i=0;i<2;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  // The uploads share one name and its object lock, which either operation may
  // take first (as in queuedUploadsCopyEveryLink): the member that completes
  // first is the one copied explicitly while the other is still uploading.
  PUTGD_TRY_COMPARE(provider->completions.size(),1);const bool secondFirst=provider->requests.last().artifact.path.contains(ids[1]);
  QVERIFY(secondFirst||provider->requests.last().artifact.path.contains(ids[0]));if(secondFirst)ids.swapItemsAt(0,1);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(journal.operation(ids[0]).state,QString("completed"));QCOMPARE(source.copies,0);
  // The upload worker journals `completed` (and a coalesced observation may
  // report it) before finish() hands the member to its list. Until then the
  // service still owns the operation and refuses without side effects, so ask
  // the service itself rather than treating the journal state as settled.
  // PUTGD_TRY_VERIFY evaluates its expression again after the loop; latch acceptance
  // so exactly one request reaches the service.
  bool replied=false,accepted=false;Error copyError;
  auto requestCopy=[&]{if(!accepted){replied=false;copyError={};operations.copyResult(ids[0],[&](Error e,QJsonObject){copyError=e;replied=true;});accepted=!replied||copyError.code!="operation_busy";}return accepted;};
  PUTGD_TRY_VERIFY(requestCopy());PUTGD_TRY_VERIFY(replied);QVERIFY2(!copyError,qPrintable(copyError.code));
  const auto firstUrl=journal.operation(ids[0]).items.first().receipt.directUrl;QCOMPARE(source.copied.text,firstUrl);
  PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.copies,1);QCOMPARE(source.copied.text,firstUrl);QCOMPARE(journal.operation(ids[0]).copyStatus,QString("copied"));QCOMPARE(journal.operation(ids[1]).copyStatus,QString("suppressed"));QCOMPARE(provider->uploads.load(),2);
 }
 void mixedResultsAndCancellationKeepUnrelatedLinks_data() {
  QTest::addColumn<QString>("mode");QTest::newRow("each-link")<<QString("each_link");QTest::newRow("list")<<QString("list");
 }
 void mixedResultsAndCancellationKeepUnrelatedLinks() {
  QFETCH(QString,mode);QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]=mode;doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});QStringList ids;
  for(int i=0;i<3;++i){QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);}
  operations.cancel(ids[2],[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(journal.operation(ids[2]).state,QString("cancelled"));
  // The two uploads share one slot, which either operation may store first and
  // take (as in queuedUploadsCopyEveryLink): the first operation's upload
  // succeeds and the second's fails, in whichever order they reach it.
  for(int i=0;i<2;++i){
   PUTGD_TRY_COMPARE(provider->completions.size(),1);const bool second=provider->requests.last().artifact.path.contains(ids[1]);QVERIFY(second||provider->requests.last().artifact.path.contains(ids[0]));
   provider->failUpload=second;provider->completions.takeFirst()();PUTGD_TRY_COMPARE(journal.operation(ids[second?1:0]).state,QString(second?"partial":"completed"));
  }
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.copies,1);QCOMPARE(source.copied.text,journal.operation(ids[0]).items.first().receipt.directUrl);QCOMPARE(journal.operation(ids[0]).copyStatus,QString("copied"));
  QCOMPARE(journal.operation(ids[1]).state,QString("partial"));QCOMPARE(journal.operation(ids[1]).copyStatus,QString("no_result"));QCOMPARE(journal.operation(ids[2]).copyStatus,QString("no_result"));QCOMPARE(provider->uploads.load(),2);
 }
 void queuedUploadsCopyEveryLink_data() {
  QTest::addColumn<int>("count");QTest::addColumn<bool>("userChanged");QTest::addColumn<QString>("mode");
  for(const auto& mode:QStringList{"each_link","list"}) {
   for(int count:{1,2,3})QTest::newRow(qPrintable(mode+QString::number(count)))<<count<<false<<mode;
   QTest::newRow(qPrintable(mode+"-user-change"))<<3<<true<<mode;
  }
 }
 void queuedUploadsCopyEveryLink() {
  QFETCH(int,count);QFETCH(bool,userChanged);QFETCH(QString,mode);
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();
  provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});
  auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]=mode;doc["preferences"]=prefs;
  QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();source.copied.kind="text";source.copied.text="synthetic initial clipboard";
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});
  QStringList ids,urls;
  for(int i=0;i<count;++i) {
   QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
   PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.reads,i+1);ids.append(id);
  }
  PUTGD_TRY_COMPARE(provider->completions.size(),1);QCOMPARE(source.revision,quint64(0));QCOMPARE(source.copies,0);
  if(userChanged){++source.revision;source.copied.text="synthetic user edit";}
  // The uploads share one name and its object lock, which any queued operation may take
  // first: complete whichever upload is pending and follow the operation it belongs to.
  QMap<QString,QString> linkOf;QString last;
  for(int i=0;i<count;++i) {
   PUTGD_TRY_COMPARE(provider->completions.size(),1);
   QString id;for(const auto& candidate:ids)if(provider->requests.last().artifact.path.contains(candidate))id=candidate;
   QVERIFY(!id.isEmpty());QVERIFY(!linkOf.contains(id));provider->completions.takeFirst()();
   PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));
   if(mode=="each_link"&&!userChanged)PUTGD_TRY_COMPARE(source.copies,i+1);
   else if(i<count-1)QCOMPARE(source.copies,0);
   const auto op=journal.operation(id);QCOMPARE(op.items.size(),1);QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QVERIFY(op.items[0].linkAvailable());
   last=linkOf[id]=op.items[0].receipt.directUrl;
   if(mode=="each_link"&&!userChanged)QCOMPARE(source.copied.text,last);
  }
  // A list holds the links in invocation order, whatever order the uploads finished in.
  for(const auto& id:ids)urls.append(linkOf.value(id));
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.copies,userChanged?0:(mode=="list"?1:count));QCOMPARE(provider->uploads.load(),count);
  QCOMPARE(source.copied.text,userChanged?QString("synthetic user edit"):(mode=="list"?urls.join('\n'):last));
  for(const auto& id:ids)QCOMPARE(journal.operation(id).copyStatus,QString(userChanged?"suppressed":"copied"));
  bool replied=false;operations.copyResult(ids.first(),[&](Error e,QJsonObject){QVERIFY(!e);replied=true;});PUTGD_TRY_VERIFY(replied);
  QCOMPARE(journal.operation(ids.first()).copyStatus,QString("copied"));QCOMPARE(source.copied.text,urls.first());QCOMPARE(provider->uploads.load(),count);
 }
 void multiFileUploadsHonorClipboardMode_data() {
  QTest::addColumn<int>("count");QTest::addColumn<QString>("mode");
  for(const auto& mode:QStringList{"each_link","list"})for(int count:{2,3})QTest::newRow(qPrintable(mode+QString::number(count)))<<count<<mode;
 }
 void multiFileUploadsHonorClipboardMode() {
  QFETCH(int,count);QFETCH(QString,mode);QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();
  provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto prefs=doc["preferences"].toObject();prefs["uploadClipboardBehavior"]=mode;doc["preferences"]=prefs;QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";for(int i=0;i<count;++i)source.payload.files.append(fixture(d.path(),QString("synthetic-%1.txt").arg(i),QByteArray("synthetic batch ")+QByteArray::number(i)));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  QStringList urls;
  for(int i=0;i<count;++i) {
   PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();
   PUTGD_TRY_COMPARE(journal.operation(id).items[i].uploadStatus,QString("uploaded"));urls.append(journal.operation(id).items[i].receipt.directUrl);
   if(mode=="each_link"){PUTGD_TRY_COMPARE(source.copies,i+1);QCOMPARE(source.copied.text,urls.last());}
   else if(i<count-1)QCOMPARE(source.copies,0);
  }
  PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("copied"));PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QCOMPARE(source.copies,mode=="list"?1:count);QCOMPARE(source.copied.text,mode=="list"?urls.join('\n'):urls.last());QCOMPARE(provider->uploads.load(),count);
 }
 void midflightPhaseReachesInspectionAndChangeSignal(){
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());int changes=0;connect(&operations,&OperationService::changed,this,[&]{++changes;});
  auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});QString id;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);
  QJsonObject inspected;bool replied=false;operations.inspect(id,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;replied=true;});PUTGD_TRY_VERIFY(replied);
  QCOMPARE(inspected["phase"].toString(),QString("uploading"));QCOMPARE(inspected["items"].toArray()[0].toObject()["phase"].toString(),QString("uploading"));PUTGD_TRY_VERIFY(changes>1);
  const auto request=provider->requests.first();QVERIFY(bool(request.progress));request.progress({"uploading",1024,2048});
  replied=false;operations.inspect(id,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;replied=true;});PUTGD_TRY_VERIFY(replied);
  QCOMPARE(inspected["items"].toArray()[0].toObject()["uploadProgress"].toObject()["bytesTransferred"].toInteger(),1024);
  request.progress({"uploading",512,{}});replied=false;operations.recent([&](Error e,QJsonObject r){QVERIFY(!e);inspected=r["operations"].toArray()[0].toObject();replied=true;});PUTGD_TRY_VERIFY(replied);
  const auto rollback=inspected["items"].toArray()[0].toObject()["uploadProgress"].toObject();QCOMPARE(rollback["bytesTransferred"].toInteger(),512);QVERIFY(!rollback.contains("bytesTotal"));
  HistoryStore journal(operations.databasePath());QVERIFY(!journal.operation(id).items.first().uploadProgress); // No per-byte durable checkpoints.
  putgd::test::settle(300);const auto before=changes;for(int i=0;i<10000;++i)request.progress({"uploading",i,{}});putgd::test::settle(300);PUTGD_TRY_VERIFY(changes-before<=2);
  const auto afterBurst=changes;for(int i=0;i<100;++i)request.progress({"uploading",9999,{}});putgd::test::settle(300);PUTGD_TRY_COMPARE(changes,afterBurst);
  provider->failUpload=true;provider->completions.takeFirst()();PUTGD_TRY_COMPARE(journal.operation(id).state,QString("partial"));PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  request.progress({"uploading",999999,{}});replied=false;operations.inspect(id,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;replied=true;});PUTGD_TRY_VERIFY(replied);
  QCOMPARE(inspected["state"].toString(),QString("partial"));QCOMPARE(inspected["items"].toArray()[0].toObject()["uploadProgress"].toObject()["bytesTransferred"].toInteger(),9999);QCOMPARE(inspected["items"].toArray()[0].toObject()["error"].toObject()["code"].toString(),QString("fixture_failed"));
  provider->failUpload=false;bool retried=false;operations.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});PUTGD_TRY_VERIFY(retried);PUTGD_TRY_COMPARE(provider->completions.size(),1);
  request.progress({"uploading",999999,{}});provider->requests.last().progress({"uploading",7,{}});replied=false;operations.inspect(id,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;replied=true;});PUTGD_TRY_VERIFY(replied);
  QCOMPARE(inspected["items"].toArray()[0].toObject()["uploadProgress"].toObject()["bytesTransferred"].toInteger(),7);
 }
 // #112: a video's poster is made after its operation ends. The upload and the
 // copied link never wait for it, a later poster reaches the history record
 // and the change signal, and a failed one leaves the record as it was.
 void videoPostersFollowTheResultWithoutDelayingIt_data(){QTest::addColumn<QByteArray>("mode");QTest::newRow("slow")<<QByteArray("slow");QTest::newRow("failed")<<QByteArray("fail");}
 void videoPostersFollowTheResultWithoutDelayingIt(){
#ifndef PUTGD_POSTER_FIXTURE
  QSKIP("The poster helper fixture is Linux-only");
#else
  QFETCH(QByteArray,mode);qputenv("PUTGD_TEST_POSTER_MODE",mode);ArtifactStore::setPosterHelperForTesting(QStringLiteral(PUTGD_POSTER_FIXTURE));auto restore=qScopeGuard([]{qunsetenv("PUTGD_TEST_POSTER_MODE");ArtifactStore::setPosterHelperForTesting({});});
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"clip.mp4",QByteArray("\0\0\0\x18" "ftypisom\0\0\x02\0isomiso2",24)+QByteArray(4096,'\x11'))};
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QStringList changes;connect(&operations,&OperationService::changed,this,[&](const QString& id){changes.append(id);});
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  HistoryStore journal(operations.databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));PUTGD_TRY_COMPARE(source.copies,1);
  auto op=journal.operation(id);QCOMPARE(op.items.size(),1);QCOMPARE(op.items[0].artifact.category,QString("videos"));QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));
  // The fixture takes 2 s, so the link was copied before any poster existed.
  QVERIFY(op.items[0].artifact.previewPath.isEmpty());
  if(mode=="fail"){putgd::test::settle(1500);PUTGD_TRY_VERIFY(journal.operation(id).items[0].artifact.previewPath.isEmpty());PUTGD_TRY_VERIFY(journal.detail(op.items[0].artifact.id)["previewPath"].toString().isEmpty());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));return;}
  const auto before=changes.count(id);PUTGD_TRY_VERIFY_WITH_TIMEOUT(!journal.operation(id).items[0].artifact.previewPath.isEmpty(),10000);
  op=journal.operation(id);const auto preview=op.items[0].artifact.previewPath;QCOMPARE(QFileInfo(preview).fileName(),op.items[0].artifact.id+".png");QVERIFY(QFileInfo(preview).isFile());
  QCOMPARE(journal.detail(op.items[0].artifact.id)["previewPath"].toString(),preview);QCOMPARE(op.state,QString("completed"));QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QCOMPARE(source.copies,1);PUTGD_TRY_VERIFY(changes.count(id)>before);
#endif
 }
 // #190: a video whose poster was never made, failed or was cleaned up gets
 // one when History asks: from the retained artifact, else from a local copy
 // that is still the same file. A refused item is answered from memory.
 void missingPreviewsAreMadeOnDemand(){
#ifndef PUTGD_POSTER_FIXTURE
  QSKIP("The poster helper fixture is Linux-only");
#else
  qputenv("PUTGD_TEST_POSTER_MODE","fail");ArtifactStore::setPosterHelperForTesting(QStringLiteral(PUTGD_POSTER_FIXTURE));auto restore=qScopeGuard([]{qunsetenv("PUTGD_TEST_POSTER_MODE");ArtifactStore::setPosterHelperForTesting({});});
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});
  auto doc=configuration(d.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"clip.mp4",QByteArray("\0\0\0\x18" "ftypisom\0\0\x02\0isomiso2",24)+QByteArray(4096,'\x11'))};
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QStringList changes;connect(&operations,&OperationService::changed,this,[&](const QString& id){changes.append(id);});
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  HistoryStore journal(operations.databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));
  auto op=journal.operation(id);const auto item=op.items[0].artifact.id;QCOMPARE(op.items[0].local.status,QString("saved"));
  // The poster made after the operation failed; it is not remembered as refused.
  putgd::test::settle(1000);PUTGD_TRY_VERIFY(journal.operation(id).items[0].artifact.previewPath.isEmpty());qunsetenv("PUTGD_TEST_POSTER_MODE");
  auto ask=[&](const QString& historyId){Error error;QJsonObject result;bool done=false;operations.makePreview(historyId,[&](Error e,QJsonObject r){error=e;result=r;done=true;});[&]{PUTGD_TRY_VERIFY_WITH_TIMEOUT(done,15000);}();return std::pair{error,result};};
  auto [error,result]=ask(item);QVERIFY2(!error,qPrintable(error.code));
  QCOMPARE(result["state"].toString(),QString("made"));QCOMPARE(result["source"].toString(),QString("artifact"));QCOMPARE(result["operationId"].toString(),id);
  const auto preview=result["previewPath"].toString();QVERIFY(QFileInfo(preview).isFile());QCOMPARE(QFileInfo(preview).fileName(),item+".png");
  QCOMPARE(journal.operation(id).items[0].artifact.previewPath,preview);QCOMPARE(journal.detail(item)["previewPath"].toString(),preview);QVERIFY(changes.contains(id));
  QCOMPARE(QImage(preview).text("putgd-duration-ms"),QString("4200"));
  std::tie(error,result)=ask(item);QVERIFY(!error);QCOMPARE(result["state"].toString(),QString("ready"));QCOMPARE(result["previewPath"].toString(),preview);
  // Cleanup removed the preview and the artifact: the local copy stands in.
  QVERIFY(QFile::remove(preview));QVERIFY(QFile::remove(op.items[0].artifact.path));
  std::tie(error,result)=ask(item);QVERIFY2(!error,qPrintable(error.code));QCOMPARE(result["state"].toString(),QString("made"));QCOMPARE(result["source"].toString(),QString("local"));QVERIFY(QFileInfo(preview).isFile());
  // A local copy that is no longer the stored file is refused, and the refusal is remembered.
  QVERIFY(QFile::remove(preview));QFile local(op.items[0].local.path);QVERIFY(local.open(QIODevice::ReadWrite));QVERIFY(local.seek(100));QVERIFY(local.write("x",1)==1);local.close();
  std::tie(error,result)=ask(item);QCOMPARE(error.code,QString("preview_source_changed"));QVERIFY(!QFileInfo(preview).exists());
  QVERIFY(QFile::remove(op.items[0].local.path));std::tie(error,result)=ask(item);QCOMPARE(error.code,QString("preview_source_changed"));
  std::tie(error,result)=ask("missing-history-id");QCOMPARE(error.code,QString("history_not_found"));
  doc=settings.document();auto retention=doc["retention"].toObject();retention["previewCacheBytes"]=0;doc["retention"]=retention;QVERIFY(settings.save(doc,settings.revision()));
  std::tie(error,result)=ask(item);QCOMPARE(error.code,QString("previews_disabled"));
#endif
 }
 // Preview requests answer in History's reply deadline: several selections
 // queue ahead of after-operation posters, and more are refused as busy.
 void onDemandPreviewsAreBounded(){
#ifndef PUTGD_POSTER_FIXTURE
  QSKIP("The poster helper fixture is Linux-only");
#else
  qputenv("PUTGD_TEST_POSTER_MODE","fail");ArtifactStore::setPosterHelperForTesting(QStringLiteral(PUTGD_POSTER_FIXTURE));auto restore=qScopeGuard([]{qunsetenv("PUTGD_TEST_POSTER_MODE");ArtifactStore::setPosterHelperForTesting({});});
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload.kind="files";for(int i=0;i<5;++i)source.payload.files.append(fixture(d.path(),QString("clip-%1.mp4").arg(i),QByteArray("\0\0\0\x18" "ftypisom\0\0\x02\0isomiso2",24)+QByteArray(4096+i,'\x11')));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
  HistoryStore journal(operations.databasePath());PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));
  // The five after-operation posters fail first; a selection made while one
  // still runs would queue behind it and turn a fourth request away.
  PUTGD_TRY_VERIFY(([&]{const auto previews=operations.diagnostics()["previews"].toObject();return !previews["running"].toBool()&&previews["queued"].toInt()==0;})());
  const auto items=journal.operation(id).items;QCOMPARE(items.size(),5);qputenv("PUTGD_TEST_POSTER_MODE","slow");
  QHash<QString,QString> outcomes;for(const auto& item:items){const auto historyId=item.artifact.id;operations.makePreview(historyId,[&,historyId](Error e,QJsonObject r){outcomes[historyId]=e?e.code:r["state"].toString();});}
  // The same selection again shares the first request's answer.
  operations.makePreview(items[1].artifact.id,[&](Error e,QJsonObject r){outcomes["again"]=e?e.code:r["state"].toString();});
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(outcomes.size()==6,25000);
  // One decodes while three wait; a fifth would wait past the deadline.
  for(int i=0;i<4;++i)QCOMPARE(outcomes.value(items[i].artifact.id),QString("made"));QCOMPARE(outcomes.value("again"),QString("made"));
  QCOMPARE(outcomes.value(items[4].artifact.id),QString("preview_busy"));
#endif
 }
 void inactiveCancellationNormalizesPhaseWithoutChangingItemOutcome(){
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  Operation prior;prior.id="synthetic-inactive";prior.state="partial";prior.phase="uploading";prior.settings=settings.snapshot();OperationItem item;item.artifact.id="synthetic-inactive-item";item.uploadStatus="unknown";item.phase="uploading";prior.items.append(item);QVERIFY(!journal.saveOperation(prior));
  bool replied=false;operations.cancel(prior.id,[&](Error e,QJsonObject value){QVERIFY(!e);QCOMPARE(value["phase"].toString(),QString("cancelled"));replied=true;});PUTGD_TRY_VERIFY(replied);const auto saved=journal.operation(prior.id);QCOMPARE(saved.phase,QString("cancelled"));QCOMPARE(saved.items.first().phase,QString("unknown"));QCOMPARE(saved.items.first().uploadStatus,QString("unknown"));QCOMPARE(source.reads,0);
 }

 void optionalProgressRoundTripsAndLegacyOperationsRemainReadable(){
  OperationItem item;item.uploadStatus="uploading";item.phase="uploading";item.uploadProgress=UploadProgress{"uploading",10,20};auto value=item.toJson();auto restored=OperationItem::fromJson(value);QVERIFY(restored.uploadProgress);QCOMPARE(restored.uploadProgress->bytesTransferred.value(),10);QCOMPARE(restored.uploadProgress->bytesTotal.value(),20);
  value.remove("phase");value.remove("uploadProgress");auto legacy=OperationItem::fromJson(value);QCOMPARE(legacy.phase,QString("uploading"));QVERIFY(!legacy.uploadProgress);
  for(const auto& invalid:QList<QJsonObject>{{{"phase","uploading"},{"bytesTransferred",-1}},{{"phase","uploading"},{"bytesTransferred",1.5}},{{"phase","uploading"},{"bytesTransferred",21},{"bytesTotal",20}},{{"phase","uploading"},{"bytesTotal",20}},{{"phase","uploading"},{"bytesTransferred",9007199254740992.0}},{{"phase","uploading"},{"bytesTransferred",1},{"url","https://example.invalid"}}})QVERIFY(!UploadProgress::fromJson(invalid));
 }

 void cleanupBlocksRetriesUntilItsSnapshotFinishes() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  bool cleaned=false;operations.cleanup([&](Error e,QJsonObject){QVERIFY(!e);cleaned=true;});
  // No event dispatch between the snapshot and retry admission: even a fast
  // worker cannot deliver its session-thread completion yet.
  Error retryError,archiveError,secondCleanupError;operations.retry("synthetic-old-operation",[&](Error e,QJsonObject){retryError=e;});operations.retryLocalSave("synthetic-old-operation",[&](Error e,QJsonObject){archiveError=e;});operations.cleanup([&](Error e,QJsonObject){secondCleanupError=e;});
  QCOMPARE(retryError.code,QString("cleanup_busy"));QCOMPARE(archiveError.code,QString("cleanup_busy"));QCOMPARE(secondCleanupError.code,QString("cleanup_busy"));PUTGD_TRY_VERIFY(cleaned);
  bool retried=false;operations.retry("synthetic-old-operation",[&](Error e,QJsonObject){QCOMPARE(e.code,QString("operation_not_found"));retried=true;});PUTGD_TRY_VERIFY(retried);
 }
 void batchItemCheckpointsHaveLinearLedgerWrites() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.payload.kind="files";
  constexpr int count=100;for(int i=0;i<count;++i)source.payload.files.append(fixture(d.path(),QString("batch-%1.txt").arg(i),"synthetic batch content"));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());const auto connection=QUuid::createUuid().toString();
  {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(operations.databasePath());QVERIFY(db.open());QSqlQuery q(db);QVERIFY(q.exec("CREATE TABLE checkpoint_count(value INTEGER NOT NULL)"));QVERIFY(q.exec("INSERT INTO checkpoint_count VALUES(0)"));
   for(const auto* verb:{"INSERT","UPDATE"})QVERIFY(q.exec(QString("CREATE TRIGGER count_%1 AFTER %1 ON history BEGIN UPDATE checkpoint_count SET value=value+1; END").arg(verb)));
   QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());HistoryStore store(operations.databasePath());PUTGD_TRY_COMPARE_WITH_TIMEOUT(store.operation(id).copyStatus,QString("copied"),30000);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
   const auto op=store.operation(id);QCOMPARE(op.items.size(),count);for(const auto& item:op.items){QCOMPARE(item.local.status,QString("saved"));QCOMPARE(item.uploadStatus,QString("local_only"));QVERIFY(QFile::exists(item.local.path));}
   QVERIFY(q.exec("SELECT value FROM checkpoint_count"));QVERIFY(q.next());const auto writes=q.value(0).toInt();QVERIFY2(writes<=5*count,qPrintable(QString("Expected linear ledger writes; %1 files caused %2 writes").arg(count).arg(writes)));q.finish();db.close();
  }QSqlDatabase::removeDatabase(connection);
 }
 void startupHeapCleanupRunsOnceAfterSuccessfulInitialization() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,registry,source);QSignalSpy initialized(&operations,&OperationService::initialized);
  PUTGD_TRY_COMPARE(initialized.count(),1);QVERIFY(operations.ready());
  const auto count=[&]{return operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt();};
  QVERIFY(operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());QCOMPARE(count(),0);
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(count(),1,5000);
  QVERIFY(!operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  putgd::test::settle(2200);PUTGD_TRY_COMPARE(count(),1);PUTGD_TRY_COMPARE(initialized.count(),1);
  PUTGD_TRY_COMPARE(source.reads,0);PUTGD_TRY_COMPARE(source.copies,0);
#endif
 }
 void failedInitializationDoesNotRequestHeapCleanup() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));Source source;
  const auto blocked=fixture(directory.path(),"not-a-directory","synthetic obstruction");QVERIFY(!blocked.isEmpty());
  OperationService operations(blocked,settings,registry,source);QSignalSpy initialized(&operations,&OperationService::initialized);
  PUTGD_TRY_COMPARE(initialized.count(),1);QVERIFY(!operations.ready());
  QString code;operations.invoke("clipboard",[&](Error error,QJsonObject){code=error.code;});QCOMPARE(code,QString("history_io"));
  QVERIFY(!operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  putgd::test::settle(2200);PUTGD_TRY_COMPARE(operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt(),0);
  PUTGD_TRY_COMPARE(source.reads,0);PUTGD_TRY_COMPARE(source.copies,0);
#endif
 }
 void shutdownCancelsPendingStartupHeapCleanup() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  QVERIFY(operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  operations.shutdown();putgd::test::settle(2200);
  PUTGD_TRY_COMPARE(operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt(),0);
  PUTGD_TRY_COMPARE(source.reads,0);PUTGD_TRY_COMPARE(source.copies,0);
#endif
 }
 void startupHeapCleanupWaitsForBackgroundWorker() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  auto barrier=std::make_shared<std::promise<void>>();auto future=barrier->get_future().share();bool released=false,replied=false;
  auto release=qScopeGuard([&]{if(!released)barrier->set_value();});
  operations.background([future](Error&){future.wait();return QJsonObject{};},[&](Error error,QJsonObject){QVERIFY(!error);replied=true;});
  PUTGD_TRY_VERIFY(operations.ready());
  const auto count=[&]{return operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt();};
  QVERIFY(operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  putgd::test::settle(2200);PUTGD_TRY_COMPARE(count(),0);PUTGD_TRY_VERIFY(!replied);
  barrier->set_value();released=true;PUTGD_TRY_VERIFY(replied);PUTGD_TRY_COMPARE_WITH_TIMEOUT(count(),1,5000);
  QVERIFY(!operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
#endif
 }
 void queuedCaptureNameUsesOriginalActionTime() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->completions.size(),1);
  source.payload.kind="image";source.payload.bytes.clear();source.payload.image=QImage(2,2,QImage::Format_ARGB32);source.payload.image.fill(Qt::blue);QString queued;
  operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);queued=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.reads,2);HistoryStore journal(operations.databasePath());
  const auto accepted=QDateTime::fromString(journal.operation(queued).created,Qt::ISODateWithMs).toLocalTime();QVERIFY(accepted.isValid());
  putgd::test::settle(1100); // Force queue persistence to cross the one-second naming boundary.
  auto complete=provider->completions.takeFirst();complete();PUTGD_TRY_COMPARE(provider->completions.size(),1);
  QCOMPARE(journal.operation(queued).items.first().artifact.name,QString("Screen-Capture-")+accepted.toString("yyyy-MM-dd-HH-mm-ss")+".png");
 }
 void queuedImagesRespectByteBudgetAndCancellationReleasesCapacity() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["pendingImageBytes"]=256;limits["concurrentUploads"]=1;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->completions.size(),1);
  source.payload.kind="image";source.payload.bytes.clear();source.payload.image=QImage(8,8,QImage::Format_ARGB32);source.payload.image.fill(Qt::blue);QString queued;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);queued=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.reads,2);putgd::test::settle(20);
  source.payload.image=QImage(8,8,QImage::Format_ARGB32);source.payload.image.fill(Qt::red);QString rejected;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);rejected=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.reads,3);HistoryStore journal(operations.databasePath());
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(journal.operation(rejected).error.code,QString("image_memory_limit"),1000);QCOMPARE(provider->uploads.load(),1);QVERIFY(journal.operation(rejected).items.isEmpty());
  operations.cancel(queued,[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(journal.operation(queued).state,QString("cancelled"));QString admitted;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);admitted=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.reads,4);putgd::test::settle(20);PUTGD_TRY_COMPARE(journal.operation(admitted).error.code,QString{});
  operations.cancel(admitted,[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(journal.operation(admitted).state,QString("cancelled"));QCOMPARE(provider->uploads.load(),1);
 }
 void decodedImagesReleaseBeforeBlockedUploadAndQueuedCancel() {
  class MovingSource final:public SourceAdapter {
  public:
   ClipboardPayload payload;int reads=0;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++reads;reply({std::exchange(payload,{}),{}});}
   void cancelAcquisition()override{}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{reply({});}
  } source;
  struct Buffer {uchar* data;std::atomic_int* freed;};
  std::atomic_int freed{0};
  auto image=[&] {auto* buffer=new Buffer{new uchar[256],&freed};std::fill_n(buffer->data,256,uchar(42));return QImage(buffer->data,8,8,32,QImage::Format_ARGB32,[](void* context){auto* b=static_cast<Buffer*>(context);delete[] b->data;++*b->freed;delete b;},buffer);};
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=1;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  auto drain=qScopeGuard([&]{for(auto complete:provider->completions)complete();provider->completions.clear();});
  source.payload.kind="image";source.payload.image=image();QString first;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);
  QCOMPARE(freed.load(),1); // The provider is still blocked: disk artifact, not pixels, owns the upload.
#ifdef __GLIBC__
  const auto reclamations=[&]{return operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt();};
  QVERIFY(operations.diagnostics()["memory"].toObject()["allocatorReclamationSupported"].toBool());
  putgd::test::settle(2200);
  PUTGD_TRY_COMPARE(reclamations(),0); // Released pixels must not trigger cleanup during the blocked upload.
#endif
  HistoryStore journal(operations.databasePath());const auto artifact=journal.operation(first).items.first().artifact;QVERIFY(!ArtifactStore(d.filePath("data/artifacts")).verify(artifact));
  source.payload.kind="image";source.payload.image=image();QString second;
  operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.reads,2);putgd::test::settle(20);PUTGD_TRY_COMPARE(freed.load(),1);
  bool cancelled=false;operations.cancel(second,[&](Error e,QJsonObject){QVERIFY(!e);cancelled=true;});PUTGD_TRY_VERIFY(cancelled);PUTGD_TRY_COMPARE(freed.load(),2);
  PUTGD_TRY_COMPARE(journal.operation(second).state,QString("cancelled"));QCOMPARE(provider->uploads.load(),1);
  provider->failUpload=true;auto complete=provider->completions.takeFirst();complete();PUTGD_TRY_COMPARE(journal.operation(first).state,QString("partial"));
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  provider->failUpload=false;bool retry=false;operations.retry(first,[&](Error e,QJsonObject){QVERIFY(!e);retry=true;});PUTGD_TRY_VERIFY(retry);PUTGD_TRY_COMPARE(provider->completions.size(),1);
  QCOMPARE(provider->requests.last().artifact.path,artifact.path);QCOMPARE(provider->requests.last().artifact.sha256,artifact.sha256);QCOMPARE(source.reads,2);QCOMPARE(freed.load(),2);
  complete=provider->completions.takeFirst();complete();PUTGD_TRY_COMPARE(journal.operation(first).state,QString("completed"));
#ifdef __GLIBC__
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(reclamations(),1,5000);
  QVERIFY(!operations.diagnostics()["memory"].toObject()["allocatorReclamationPending"].toBool());
  putgd::test::settle(2200);
  PUTGD_TRY_COMPARE(reclamations(),1); // One quiet-period cleanup, with no repeating idle timer.
#endif
 }
 void imageHeapCleanupWaitsForSelectionAndBackgroundWork() {
#ifndef __GLIBC__
  QSKIP("Allocator reclamation uses glibc on supported Linux builds");
#else
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));Source source;
  source.payload.kind="image";source.payload.image=QImage(16,16,QImage::Format_ARGB32);source.payload.image.fill(Qt::cyan);
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  const auto count=[&]{return operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt();};
  operations.invoke("clipboard",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.copies,1);
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  source.defer=true;operations.invoke("region",[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
  putgd::test::settle(2200);PUTGD_TRY_COMPARE(count(),0); // A selector can be active with no operation worker thread.
  auto barrier=std::make_shared<std::promise<void>>();auto future=barrier->get_future().share();bool released=false;
  auto release=qScopeGuard([&]{if(!released)barrier->set_value();});
  operations.background([future](Error&){future.wait();return QJsonObject{};},[](Error,QJsonObject){});
  source.acquisitions.takeFirst()({{}, {"cancelled","Selection cancelled",{}}});
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  putgd::test::settle(2200);PUTGD_TRY_COMPARE(count(),0); // Unrelated file/history work must also drain.
  barrier->set_value();released=true;PUTGD_TRY_COMPARE_WITH_TIMEOUT(count(),1,5000);
#endif
 }
 void capturesReceiveStableTimestampNamesBeforeLocalArchive() {
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  auto document=configuration(directory.path(),true);auto naming=document["naming"].toObject();naming["mode"]="original";document["naming"]=naming;QVERIFY(settings.save(document,{}));
  Source source;source.payload.kind="image";source.payload.image=QImage(3,2,QImage::Format_ARGB32);source.payload.image.fill(Qt::cyan);
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QString id;
  operations.invoke("region",[&](Error error,QJsonObject response){QVERIFY(!error);id=response["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);
  HistoryStore journal(operations.databasePath());auto operation=journal.operation(id);QCOMPARE(operation.items.size(),1);
  const auto artifact=operation.items[0].artifact;
  QVERIFY(QRegularExpression("^Screen-Capture-\\d{4}-\\d{2}-\\d{2}-\\d{2}-\\d{2}-\\d{2}\\.png$").match(artifact.name).hasMatch());
  QCOMPARE(QFileInfo(operation.items[0].local.path).fileName(),artifact.name);
  ArtifactStore spool(directory.filePath("data/artifacts"));auto recovered=spool.recover(id);QVERIFY(!recovered.error);QCOMPARE(recovered.artifacts[0].name,artifact.name);
 }
 void newerInvocationInvalidatesDeferredNativeClipboardCommit() {
  class DeferredSource final:public SourceAdapter {
  public:
   int invalidations=0;std::function<void(Error)> pending;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override{}
   void cancelAcquisition()override{}
   void invalidateCopyEligibility()override{++invalidations;if(pending){auto callback=std::move(pending);callback({"copy_suppressed","Newer action superseded native commit",{}});}}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{pending=std::move(reply);}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  bool completed=false;Error error;
  operations.copyHistoryUrl("https://example.invalid/synthetic",[&](Error e,QJsonObject){error=e;completed=true;});
  QVERIFY(bool(source.pending));
  operations.invalidateCopyEligibility();
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(completed,500);
  QCOMPARE(error.code,QString("copy_suppressed"));
  QCOMPARE(source.invalidations,2);
 }
 void failedAcceptanceReleasesPreparedRequestWithoutAcquiring() {
  class PreparedSource final:public SourceAdapter {
  public:
   QString path;std::atomic_int retained{0};int acquired=0;bool injected=false;ResourceLimits preparedLimits;
   quint64 clipboardRevision() const override{return 0;}
   QJsonObject capabilities() const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits& limits) override {
    preparedLimits=limits;
    auto lifetime=std::shared_ptr<int>(new int,[this](int* value){delete value;--retained;});++retained;
    // Inject a write failure after startup recovery, before durable acceptance.
    const auto connection=QUuid::createUuid().toString();
    {auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(path);if(db.open()){QSqlQuery query(db);injected=query.exec("DROP TABLE operations");}db.close();}
    QSqlDatabase::removeDatabase(connection);
    return [this,lifetime](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquired;reply({{}, {"cancelled","Synthetic cancellation",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override{++acquired;}
   void cancelAcquisition() override{}
   void copy(const CopyPayload&,std::function<void(Error)> reply) override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());source.path=operations.databasePath();
  int replies=0;Error error;operations.invoke("region",[&](Error value,QJsonObject){error=value;++replies;});
  PUTGD_TRY_COMPARE(replies,1);QVERIFY(source.injected);QCOMPARE(error.code,QString("history_io"));PUTGD_TRY_COMPARE(source.retained.load(),0);
  QCOMPARE(source.acquired,0);QCOMPARE(source.preparedLimits.imagePixels,settings.snapshot().limits.imagePixels);
  operations.shutdown();QCOMPARE(replies,1);
 }
 void cancellationDuringAcquiringCommitNeverBeginsPreparedSource() {
  class PreparedSource final:public SourceAdapter {
  public:
   std::atomic_int retained{0};int acquired=0,cancels=0;
   quint64 clipboardRevision() const override{return 0;}
   QJsonObject capabilities() const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&) override {
    auto lifetime=std::shared_ptr<int>(new int,[this](int* value){delete value;--retained;});++retained;
    return [this,lifetime](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquired;reply({{}, {"cancelled","Synthetic cancellation",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>) override{++acquired;}
   void cancelAcquisition() override{++cancels;}
   void copy(const CopyPayload&,std::function<void(Error)> reply) override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  const auto connection=QUuid::createUuid().toString();
  {
   auto blocker=QSqlDatabase::addDatabase("QSQLITE",connection);blocker.setDatabaseName(operations.databasePath());QVERIFY(blocker.open());
   QString id;bool requested=false;
   operations.invoke("region",[&](Error error,QJsonObject result){
    QVERIFY(!error);id=result["operationId"].toString();QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));
    QTimer::singleShot(30,&operations,[&]{
     operations.cancel(id,[&](Error error,QJsonObject){QVERIFY(!error);requested=true;});
     QSqlQuery unlock(blocker);QVERIFY(unlock.exec("ROLLBACK"));
    });
   });
   PUTGD_TRY_VERIFY(requested);PUTGD_TRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state=="cancelled";})());
   PUTGD_TRY_COMPARE(source.retained.load(),0);QCOMPARE(source.acquired,0);QCOMPARE(source.cancels,1);operations.shutdown();blocker.close();
  }
  QSqlDatabase::removeDatabase(connection);
 }
 void earlySelectionTerminalNeverBeginsPixels_data() {
  QTest::addColumn<QString>("code");QTest::addColumn<bool>("afterAcceptance");
  QTest::newRow("escape-before-acceptance")<<QString("cancelled")<<false;
  QTest::newRow("failure-before-acceptance")<<QString("display_changed")<<false;
  QTest::newRow("escape-during-acquiring")<<QString("cancelled")<<true;
  QTest::newRow("failure-during-acquiring")<<QString("display_changed")<<true;
 }
 void earlySelectionTerminalNeverBeginsPixels() {
  QFETCH(QString,code);QFETCH(bool,afterAcceptance);
  class PreviewSource final:public SourceAdapter {
  public:
   std::function<void(Error)> terminal;int acquired=0,previews=0;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> notify)override {
    ++previews;terminal=std::move(notify);
    return [this](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquired;reply({{}, {"cancelled","Unexpected acquisition",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++acquired;reply({{}, {"cancelled","Unexpected legacy acquisition",{}}});}
   void cancelAcquisition()override{}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  const auto connection=QUuid::createUuid().toString();
  {
   auto blocker=QSqlDatabase::addDatabase("QSQLITE",connection);blocker.setDatabaseName(operations.databasePath());QVERIFY(blocker.open());
   if(!afterAcceptance){QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));}
   QString id;bool accepted=false;
   operations.invoke("region",[&](Error error,QJsonObject result){
    QVERIFY(!error);id=result["operationId"].toString();accepted=true;
    if(afterAcceptance){QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));QTimer::singleShot(30,&operations,[&]{source.terminal({code,"Synthetic early terminal",{}});QSqlQuery unlock(blocker);QVERIFY(unlock.exec("ROLLBACK"));});}
   });
   QCOMPARE(source.previews,1);QVERIFY(bool(source.terminal));
   if(!afterAcceptance){source.terminal({code,"Synthetic early terminal",{}});QVERIFY(!accepted);QSqlQuery unlock(blocker);QVERIFY(unlock.exec("ROLLBACK"));}
   PUTGD_TRY_VERIFY(accepted);PUTGD_TRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state==(code=="cancelled"?"cancelled":"failed");})());
   HistoryStore store(operations.databasePath());const auto op=store.operation(id);QCOMPARE(op.error.code,code);QVERIFY(op.items.isEmpty());QCOMPARE(source.acquired,0);
   operations.shutdown();blocker.close();
  }
  QSqlDatabase::removeDatabase(connection);
 }
 void explicitCancelOverridesEarlyDisplayErrorWhileAcquiringIsBlocked() {
  class PreviewSource final:public SourceAdapter {
  public:
   std::function<void(Error)> terminal;int acquired=0,cancels=0,copies=0;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> notify)override {
    terminal=std::move(notify);
    return [this](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquired;reply({{}, {"capture_failed","Unexpected pixel acquisition",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override{++acquired;reply({{}, {"capture_failed","Unexpected legacy acquisition",{}}});}
   void cancelAcquisition()override{++cancels;}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{++copies;reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  const auto connection=QUuid::createUuid().toString();
  {
   auto blocker=QSqlDatabase::addDatabase("QSQLITE",connection);blocker.setDatabaseName(operations.databasePath());QVERIFY(blocker.open());
   bool lockHeld=false,injected=false,cancelReplied=false;int acceptanceReplies=0;QString id;
   // An assertion must not leave the worker blocked while shutdown joins it.
   const auto release=qScopeGuard([&]{if(lockHeld){QSqlQuery unlock(blocker);unlock.exec("ROLLBACK");}});
   operations.invoke("region",[&](Error error,QJsonObject result){
    QVERIFY(!error);++acceptanceReplies;id=result["operationId"].toString();QVERIFY(!id.isEmpty());
    QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));lockHeld=true;
    // Let the queued acquiring continuation run against the held writer lock.
    QTimer::singleShot(30,&operations,[&]{
     QSqlQuery checkpoint(blocker);checkpoint.prepare("SELECT json_extract(data,'$.state') FROM operations WHERE id=?");checkpoint.addBindValue(id);
     QVERIFY(checkpoint.exec());QVERIFY(checkpoint.next());QCOMPARE(checkpoint.value(0).toString(),QString("accepted"));checkpoint.finish();
     QVERIFY(bool(source.terminal));source.terminal({"display_changed","Synthetic early display error",{}});injected=true;
     operations.cancel(id,[&](Error cancelError,QJsonObject){QVERIFY(!cancelError);cancelReplied=true;});
     QSqlQuery unlock(blocker);QVERIFY(unlock.exec("ROLLBACK"));lockHeld=false;
    });
   });
   PUTGD_TRY_VERIFY(injected&&cancelReplied);QCOMPARE(acceptanceReplies,1);
   PUTGD_TRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state=="cancelled";})());
   HistoryStore store(operations.databasePath());const auto operation=store.operation(id);
   QVERIFY(operation.cancelled);QVERIFY(!operation.error);QVERIFY(operation.items.isEmpty());
   QCOMPARE(source.acquired,0);QCOMPARE(source.cancels,1);QCOMPARE(source.copies,0);
   PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
   operations.shutdown();blocker.close();
  }
  QSqlDatabase::removeDatabase(connection);
 }

 void drainedEarlySelectionAllowsNextRequestBeforeJournalCompletes() {
  class PreviewSource final:public SourceAdapter {
  public:
   QList<std::function<void(Error)>> terminals;QList<std::function<void()>> releases;int acquisitions=0;
   quint64 clipboardRevision()const override{return 0;}QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> terminal,std::function<void()> released)override {
    terminals.append(std::move(terminal));releases.append(std::move(released));
    auto release=releases.last();
    return [this,release](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquisitions;release();reply({{}, {"cancelled","Synthetic cancellation",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override{QFAIL("Unexpected legacy acquisition");}
   void cancelAcquisition()override{}void copy(const CopyPayload&,std::function<void(Error)> reply)override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());const auto connection=QUuid::createUuid().toString();
  {
   auto blocker=QSqlDatabase::addDatabase("QSQLITE",connection);blocker.setDatabaseName(operations.databasePath());QVERIFY(blocker.open());bool locked=true;
   const auto unlock=qScopeGuard([&]{if(locked){QSqlQuery q(blocker);q.exec("ROLLBACK");}});QSqlQuery lock(blocker);QVERIFY(lock.exec("BEGIN IMMEDIATE"));
   QString first,second;int replies=0;
   operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();++replies;});QCOMPARE(source.terminals.size(),1);
   source.terminals[0]({"cancelled","Early Escape",{}});QCOMPARE(replies,0);QCOMPARE(source.acquisitions,0);
   // Error metadata alone must not release an undrained selector/helper.
   QString busy;operations.invoke("region",[&](Error e,QJsonObject){busy=e.code;});QCOMPARE(busy,QString("busy_selecting"));
   source.releases[0]();operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();++replies;});
   QCOMPARE(source.terminals.size(),2);QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),2);QCOMPARE(replies,0);QCOMPARE(source.acquisitions,0);
   // Late/stale release belongs only to the old operation.
   source.releases[0]();busy.clear();operations.invoke("region",[&](Error e,QJsonObject){busy=e.code;});QCOMPARE(busy,QString("busy_selecting"));
   QSqlQuery release(blocker);QVERIFY(release.exec("ROLLBACK"));locked=false;
   PUTGD_TRY_COMPARE(replies,2);PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.acquisitions,1);
   HistoryStore store(operations.databasePath());QCOMPARE(store.operation(first).state,QString("cancelled"));QCOMPARE(store.operation(second).state,QString("cancelled"));
   operations.shutdown();blocker.close();
  }
  QSqlDatabase::removeDatabase(connection);
 }
 void durableEarlyCancellationKeepsReservationUntilSourceDrains() {
  class PreviewSource final:public SourceAdapter {
  public:
   std::function<void(Error)> terminal;std::function<void()> released;int acquisitions=0;
   quint64 clipboardRevision()const override{return 0;}QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> notify,std::function<void()> release)override {
    terminal=std::move(notify);released=std::move(release);
    return [this](const ResourceLimits&,std::function<void(PayloadResult)> reply){++acquisitions;released();reply({{}, {"cancelled","Synthetic cancellation",{}}});};
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override{QFAIL("Unexpected legacy acquisition");}
   void cancelAcquisition()override{}void copy(const CopyPayload&,std::function<void(Error)> reply)override{reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());QString id;
  operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});QVERIFY(bool(source.terminal));source.terminal({"cancelled","Early Escape",{}});
  PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state=="cancelled";})());
  QCOMPARE(operations.diagnostics()["activeOperations"].toInt(),1);QCOMPARE(source.acquisitions,0);
  QString busy;operations.invoke("region",[&](Error e,QJsonObject){busy=e.code;});QCOMPARE(busy,QString("busy_selecting"));
  // The journal commit can precede its queued completion callback. Both the
  // durable result and source drain must release the reservation.
  source.released();PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  QString next;operations.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);next=r["operationId"].toString();});
  PUTGD_TRY_VERIFY(!next.isEmpty());PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(source.acquisitions,1);operations.shutdown();
 }
 void previewCheckpointFailuresReleasePreparationWithoutPixels_data() {
  QTest::addColumn<QString>("checkpoint");
  QTest::newRow("accepted-insert")<<QString("accepted");
  QTest::newRow("acquiring-update")<<QString("acquiring");
 }
 void previewCheckpointFailuresReleasePreparationWithoutPixels() {
  QFETCH(QString,checkpoint);
  class PreparedSource final:public SourceAdapter {
  public:
   std::function<void(Error)> terminal;
   std::atomic_int retained{0},released{0};
   int prepared=0,pixels=0,legacyAcquisitions=0,ordinaryReplies=0,copies=0;
   quint64 clipboardRevision()const override{return 0;}
   QJsonObject capabilities()const override{return {};}
   PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)> notify)override {
    ++prepared;terminal=std::move(notify);++retained;
    auto lifetime=std::shared_ptr<int>(new int,[this](int* value){delete value;--retained;++released;});
    return [this,lifetime](const ResourceLimits&,std::function<void(PayloadResult)> reply){
     ++pixels;++ordinaryReplies;reply({{}, {"capture_failed","Unexpected pixel acquisition",{}}});
    };
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply)override {
    ++legacyAcquisitions;++ordinaryReplies;reply({{}, {"capture_failed","Unexpected legacy acquisition",{}}});
   }
   void cancelAcquisition()override{}
   void copy(const CopyPayload&,std::function<void(Error)> reply)override{++copies;reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  const auto connection=QUuid::createUuid().toString();
  {
   auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(operations.databasePath());QVERIFY(db.open());
   {
    QSqlQuery inject(db);
    const auto sql=checkpoint=="accepted"
     ? QString("CREATE TRIGGER synthetic_checkpoint_failure BEFORE INSERT ON operations WHEN json_extract(NEW.data,'$.state')='accepted' BEGIN SELECT RAISE(ABORT,'synthetic accepted checkpoint failure'); END")
     : QString("CREATE TRIGGER synthetic_checkpoint_failure BEFORE UPDATE ON operations WHEN json_extract(NEW.data,'$.state')='acquiring' BEGIN SELECT RAISE(ABORT,'synthetic acquiring checkpoint failure'); END");
    QVERIFY(inject.exec(sql));
   }
   db.close();
  }
  QSqlDatabase::removeDatabase(connection);
  int acceptanceReplies=0;Error acceptanceError;QJsonObject acceptanceResult;
  operations.invoke("region",[&](Error error,QJsonObject result){++acceptanceReplies;acceptanceError=error;acceptanceResult=std::move(result);});
  QCOMPARE(source.prepared,1);QVERIFY(bool(source.terminal));
  PUTGD_TRY_COMPARE(acceptanceReplies,1);
  if(checkpoint=="accepted") {
   QCOMPARE(acceptanceError.code,QString("history_io"));QVERIFY(acceptanceResult.isEmpty());
   {
    auto db=QSqlDatabase::addDatabase("QSQLITE",connection);db.setDatabaseName(operations.databasePath());QVERIFY(db.open());
    {QSqlQuery rows(db);QVERIFY(rows.exec("SELECT count(*) FROM operations"));QVERIFY(rows.next());QCOMPARE(rows.value(0).toInt(),0);}
    db.close();
   }
   QSqlDatabase::removeDatabase(connection);
  } else {
   QVERIFY(!acceptanceError);const auto id=acceptanceResult["operationId"].toString();QVERIFY(!id.isEmpty());
   QCOMPARE(acceptanceResult["operation"].toObject()["state"].toString(),QString("accepted"));
   PUTGD_TRY_VERIFY(([&]{HistoryStore store(operations.databasePath());return store.operation(id).state=="failed";})());
   HistoryStore store(operations.databasePath());Error readError;const auto operation=store.operation(id,&readError);
   QVERIFY(!readError);QCOMPARE(operation.error.code,QString("history_io"));QVERIFY(!operation.cancelled);
   QVERIFY(operation.items.isEmpty());QCOMPARE(operation.copyStatus,QString("no_result"));
  }
  {HistoryStore store(operations.databasePath());const auto history=store.list({});QVERIFY(!history.error);QCOMPARE(history.total,qint64(0));QVERIFY(history.records.isEmpty());}
  PUTGD_TRY_COMPARE(source.retained.load(),0);QCOMPARE(source.released.load(),1);
  QCOMPARE(source.pixels,0);QCOMPARE(source.legacyAcquisitions,0);QCOMPARE(source.ordinaryReplies,0);QCOMPARE(source.copies,0);
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  operations.shutdown();QCOMPARE(acceptanceReplies,1);QCOMPARE(source.prepared,1);
 }

 void captureTargetIsPreparedBeforeAsynchronousAcceptance() {
  class FocusSource final : public SourceAdapter {
  public:
   QString focus="external-window-at-invocation",copied;
   int preparations=0,legacyReads=0,copies=0;
   quint64 clipboardRevision() const override {return 0;}
   QJsonObject capabilities() const override {return {{"window",true}};}
   PreparedAcquisition prepareAcquisition(const QString&) override {
    ++preparations;
    const auto frozen=focus;
    return [frozen](const ResourceLimits&,std::function<void(PayloadResult)> reply) {
     ClipboardPayload payload;payload.kind="text";payload.bytes=frozen.toUtf8();reply({payload,{}});
    };
   }
   void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)> reply) override {
    ++legacyReads;ClipboardPayload payload;payload.kind="text";payload.bytes=focus.toUtf8();reply({payload,{}});
   }
   void cancelAcquisition() override {}
   void copy(const CopyPayload& payload,std::function<void(Error)> reply) override {++copies;copied=payload.text;reply({});}
  } source;
  QTemporaryDir directory;ProviderRegistry registry;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  OperationService operations(directory.filePath("data"),settings,registry,source);
  PUTGD_TRY_VERIFY(operations.ready());
  operations.invoke("window",[](Error error,QJsonObject) {QVERIFY(!error);});
  source.focus="different-window-after-invocation";
  PUTGD_TRY_COMPARE(source.copies,1);
  QCOMPARE(source.preparations,1);
  QCOMPARE(source.legacyReads,0);
  QCOMPARE(source.copied,QString("external-window-at-invocation"));
 }
 void imageResultSuppliesEncodedPngBeforeClipboardOwnership() {
  QTemporaryDir directory;ProviderRegistry registry;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  Source source;source.payload.kind="image";source.payload.image=QImage(3,2,QImage::Format_ARGB32);source.payload.image.fill(Qt::magenta);
  OperationService operations(directory.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  operations.invoke("region",[](Error error,QJsonObject){QVERIFY(!error);});PUTGD_TRY_COMPARE(source.copies,1);
  QCOMPARE(source.copied.kind,QString("image"));
  QVERIFY(source.copied.encodedImage.startsWith(QByteArray::fromHex("89504e470d0a1a0a")));
  QCOMPARE(QImage::fromData(source.copied.encodedImage),source.payload.image);
 }
 void selectorCancellationIsTerminalWithoutPublication() {
  QTemporaryDir directory;ProviderRegistry registry;
  auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;source.defer=true;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  PUTGD_TRY_VERIFY(operations.ready());QString id;
  operations.invoke("region",[&](Error error,QJsonObject response){QVERIFY(!error);id=response["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
  source.acquisitions.takeFirst()({{}, {"cancelled","Selection cancelled",{}}});
  HistoryStore journal(operations.databasePath());
  PUTGD_TRY_COMPARE(journal.operation(id).state,QString("cancelled"));
  QVERIFY(journal.operation(id).cancelled);
  QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.copies,0);
#ifdef __GLIBC__
  PUTGD_TRY_COMPARE_WITH_TIMEOUT(operations.diagnostics()["memory"].toObject()["allocatorReclamationCount"].toInt(),1,5000);
#endif
 }
 void acquisitionFailureSettlesWithoutPendingCopy() {
  QTemporaryDir directory;ProviderRegistry registry;
  SettingsService settings(directory.filePath("config.json"));
  QVERIFY(settings.save(configuration(directory.path(),true),{}));
  Source source;source.defer=true;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  PUTGD_TRY_VERIFY(operations.ready());QString id;
  operations.invoke("clipboard",[&](Error error,QJsonObject response){QVERIFY(!error);id=response["operationId"].toString();});
  PUTGD_TRY_COMPARE(source.acquisitions.size(),1);
  source.acquisitions.takeFirst()({{}, {"source_unsupported","No supported source",{}}});
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  HistoryStore journal(operations.databasePath());const auto operation=journal.operation(id);
  QCOMPARE(operation.state,QString("failed"));
  QCOMPARE(operation.copyStatus,QString("no_result"));
  QCOMPARE(source.copies,0);QVERIFY(operation.items.isEmpty());
 }
 void objectLocksRetainOnlyActiveOwners() {
  QTemporaryDir directory;
  ProviderRegistry registry;
  registry.add("opaque",std::make_shared<OpaqueProvider>());
  SettingsService settings(directory.filePath("config.json"));
  Source source;
  OperationService operations(directory.filePath("data"),settings,registry,source);
  PUTGD_TRY_VERIFY(operations.ready());
  Destination original{"opaque","first","revision-1",{{"workspace","fixture"}}, {}};
  auto first=operations.objectLock(original,{},"same-object");
  auto alias=original;
  alias.profileId="second";
  alias.profileRevision="revision-2";
  auto second=operations.objectLock(alias,{},"same-object");
  QVERIFY(first==second);
  std::weak_ptr<std::mutex> lifetime=first;
  first.reset();
  QVERIFY(!lifetime.expired());
  second.reset();
  QVERIFY(lifetime.expired());
 }
 void reconciliationCannotReplaceAcceptedDestination() {
  QTemporaryDir directory;
  ProviderRegistry registry;
  auto provider=std::make_shared<OpaqueProvider>();
  provider->loseReply=true;
  registry.add("opaque",provider);
  SettingsService settings(directory.filePath("config.json"),[&](auto id,auto value) {
   return registry.validate(id,value);
  });
  QVERIFY(settings.save(configuration(directory.path()),{}));
  Source source;
  source.payload=textPayload();
  OperationService operations(directory.filePath("data"),settings,registry,source);
  PUTGD_TRY_VERIFY(operations.ready());
  QString id;
  operations.invoke("clipboard",[&](Error error,QJsonObject response) {
   QVERIFY(!error);
   id=response["operationId"].toString();
  });
  PUTGD_TRY_VERIFY(!id.isEmpty());
  HistoryStore journal(operations.databasePath());
  PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("no_result"));
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  const auto candidate=journal.operation(id).items[0].candidate;
  {
   QMutexLocker guard(&provider->mutex);
   QCOMPARE(provider->objects.size(),1);
   provider->objects.begin().value().destination.settings["workspace"]="different-destination";
  }
  bool accepted=false;
  operations.retry(id,[&](Error error,QJsonObject) {
   QVERIFY(!error);
   accepted=true;
  });
  PUTGD_TRY_VERIFY(accepted);
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  const auto item=journal.operation(id).items[0];
  QCOMPARE(item.uploadStatus,QString("unknown"));
  QCOMPARE(item.candidate,candidate);
  QVERIFY(item.receipt.directUrl.isEmpty());
  QCOMPARE(provider->uploads.load(),1);
  QCOMPARE(provider->deletes.load(),0);
 }
 void localActionsAndUnavailableBatch() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);id=result["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_COMPARE(source.copies,1);QCOMPARE(source.copied.kind,QString("text"));QCOMPARE(source.copied.text,QString("synthetic source content"));QCOMPARE(provider->uploads.load(),0);
  auto doc=configuration(d.path());QVERIFY(settings.save(doc,settings.revision()));source.payload.kind="files";source.payload.files={fixture(d.path(),"ok.txt","synthetic"),d.filePath("missing")};QString batch;ops.invoke("clipboard",[&](Error e,QJsonObject result){QVERIFY(!e);batch=result["operationId"].toString();});PUTGD_TRY_VERIFY(!batch.isEmpty());QJsonObject inspected;ops.inspect(batch,[&](Error e,QJsonObject r){QVERIFY(!e);inspected=r;});PUTGD_TRY_VERIFY(!inspected.isEmpty());
  PUTGD_TRY_VERIFY_WITH_TIMEOUT(([&]{HistoryStore store(ops.databasePath());return store.operation(batch).state=="failed";})(),5000);HistoryStore store(ops.databasePath());QCOMPARE(store.operation(batch).error.code,QString("source_unavailable"));QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.copies,1);
 }
 void reverseCompletionCopiesBothLinksAndUserChangesRemainProtected() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->concurrency=8;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;auto naming=doc["naming"].toObject();naming["mode"]="generated";doc["naming"]=naming;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString older,newer;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);older=r["operationId"].toString();});ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);newer=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),2);HistoryStore journal(ops.databasePath());const auto marker=journal.operation(newer).items[0].publicationId;int newerIndex=provider->requests[0].publicationId==marker?0:1;auto complete=provider->completions.takeAt(newerIndex);provider->requests.removeAt(newerIndex);complete();PUTGD_TRY_COMPARE(source.copies,1);complete=provider->completions.takeFirst();complete();PUTGD_TRY_VERIFY_WITH_TIMEOUT(([&]{HistoryStore store(ops.databasePath());return store.operation(older).copyStatus=="copied";})(),5000);QCOMPARE(source.copies,2);HistoryStore store(ops.databasePath());QCOMPARE(store.operation(older).items[0].uploadStatus,QString("uploaded"));QCOMPARE(store.operation(newer).items[0].uploadStatus,QString("uploaded"));
  QString third;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);third=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);++source.revision;provider->completions.takeFirst()();PUTGD_TRY_VERIFY(([&]{HistoryStore s(ops.databasePath());return s.operation(third).copyStatus=="suppressed";})());QCOMPARE(source.copies,2);
 }
 void unknownRestartReconcilesWithoutNewUploadAndSnapshotsSettings() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->loseReply=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();QString id,candidate;
  {OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());PUTGD_TRY_VERIFY(([&]{HistoryStore s(ops.databasePath());auto o=s.operation(id);return !o.items.isEmpty()&&o.items[0].uploadStatus=="unknown"&&o.copyStatus=="no_result";})());HistoryStore s(ops.databasePath());candidate=s.operation(id).items[0].candidate;}
  auto changed=configuration(d.path(),true);QVERIFY(settings.save(changed,settings.revision()));OperationService restarted(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(restarted.ready());QCOMPARE(provider->uploads.load(),1);bool accepted=false;restarted.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);accepted=true;});PUTGD_TRY_VERIFY(accepted);PUTGD_TRY_VERIFY(([&]{HistoryStore s(restarted.databasePath());auto o=s.operation(id);return o.items[0].uploadStatus=="uploaded";})());QCOMPARE(provider->uploads.load(),1);HistoryStore s(restarted.databasePath());auto op=s.operation(id);QCOMPARE(op.items[0].candidate,candidate);QCOMPARE(op.destination.profileId,QString("test"));QCOMPARE(op.items[0].receipt.destination.settings["workspace"].toString(),QString("fixture-workspace"));
 }
 void archiveFailureDoesNotStopUploadAndRetryUsesSpool() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=true;const auto blocker=fixture(d.path(),"blocked","synthetic");archive["root"]=blocker;doc["archive"]=archive;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto op=store.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QCOMPARE(op.items[0].local.status,QString("failed"));QVERIFY(QFile::remove(blocker));QVERIFY(QDir().mkpath(blocker));PUTGD_TRY_COMPARE(store.operation(id).copyStatus,QString("copied"));PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);bool done=false;ops.retryLocalSave(id,[&](Error e,QJsonObject){QVERIFY(!e);done=true;});PUTGD_TRY_VERIFY(done);op=store.operation(id);QCOMPARE(op.items[0].local.status,QString("saved"));QCOMPARE(provider->uploads.load(),1);QFile archived(op.items[0].local.path);QVERIFY(archived.open(QIODevice::ReadOnly));QCOMPARE(archived.readAll(),QByteArray("synthetic source content"));
 }
 void overwriteBatchMarksBothAndSkipsNeverCopy() {
  QTemporaryDir d;QDir().mkpath(d.filePath("a"));QDir().mkpath(d.filePath("b"));auto first=fixture(d.filePath("a"),"report.pdf","%PDF-1.4\nfixture-a"),second=fixture(d.filePath("b"),"report.pdf","%PDF-1.4\nfixture-b"),third=fixture(d.path(),"notes.txt","fixture-notes");ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());doc["naming"]=QJsonObject{{"mode","original"},{"collisionPolicy","overwrite"}};QVERIFY(settings.save(doc,{}));Source source;source.payload.kind="files";source.payload.files={first,second,third};OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.copies,1);HistoryStore store(ops.databasePath());auto op=store.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("conflict"));QCOMPARE(op.items[1].uploadStatus,QString("conflict"));QCOMPARE(op.items[2].uploadStatus,QString("uploaded"));QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.copied.text,op.items[2].receipt.directUrl);
  doc["naming"]=QJsonObject{{"mode","original"},{"collisionPolicy","skip"}};QVERIFY(settings.save(doc,settings.revision()));source.payload.files={third};QString skipped;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);skipped=r["operationId"].toString();});PUTGD_TRY_VERIFY(([&]{auto o=store.operation(skipped);return o.copyStatus=="no_result";})());QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.copies,1);QVERIFY(store.operation(skipped).items[0].receipt.publicationId.isEmpty());
 }
 void onlyTheResidentOwnerRecoversAbandonedStaging() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;const auto root=d.filePath("data"),staging=root+"/artifacts/.817effa3-cd94-4a25-b066-d85f7fbbdb9a-4c0b1d4a-13a3-46ba-9316-90859b0cfe4e";
  {OperationService owner(root,settings,registry,source);PUTGD_TRY_VERIFY(owner.ready());QVERIFY(QDir().mkpath(staging));const auto artifact=fixture(staging,"cad9f544-c88c-4d8e-875b-576db75867dd","unfinished synthetic bytes"),local=fixture(staging,"user-owned-copy.txt","user archive");OperationService second(root,settings,registry,source);QSignalSpy initialized(&second,&OperationService::initialized);PUTGD_TRY_COMPARE(initialized.count(),1);QVERIFY(!second.ready());QString code;second.invoke("clipboard",[&](Error e,QJsonObject){code=e.code;});QCOMPARE(code,QString("journal_busy"));QVERIFY(QFile::exists(artifact));QVERIFY(QFile::exists(local));}
  OperationService restarted(root,settings,registry,source);PUTGD_TRY_VERIFY(restarted.ready());QVERIFY(!QFile::exists(staging+"/cad9f544-c88c-4d8e-875b-576db75867dd"));QVERIFY(QFile::exists(staging+"/user-owned-copy.txt"));QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
 void committedAcquisitionRecoversAfterMissingItemCheckpoint() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));const auto root=d.filePath("data"),id=QString("2e0c5cf4-ce1a-4c36-979d-e56c7d28c9e5");Operation accepted;accepted.id=id;accepted.action="clipboard";accepted.state="acquiring";accepted.created="2026-10-07T00:00:00.000Z";accepted.settings=settings.snapshot();accepted.settingsRevision=settings.revision();accepted.destination={"opaque","test",settings.revision(),{{"workspace","fixture-workspace"}},{{"mode","session"},{"reference",QJsonValue::Null}}};HistoryStore store(root+"/history.sqlite");QVERIFY(!store.saveOperation(accepted));ArtifactStore artifacts(root+"/artifacts");auto acquired=artifacts.acquire(id,textPayload(),ResourceLimits{});QVERIFY(!acquired.error);Source source;source.payload=textPayload();OperationService ops(root,settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());auto recovered=store.operation(id);QCOMPARE(recovered.state,QString("interrupted"));QCOMPARE(recovered.items.size(),1);QCOMPARE(recovered.items[0].artifact.sha256,acquired.artifacts[0].sha256);QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.reads,0);bool retried=false;ops.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});PUTGD_TRY_VERIFY(retried);PUTGD_TRY_COMPARE(store.operation(id).items[0].uploadStatus,QString("uploaded"));QVERIFY(!store.operation(id).items[0].candidate.isEmpty());QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.reads,0);
 }
 void resumableUploadJournalSurvivesForRetry() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->journalThenUnknown=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  QJsonObject op;PUTGD_TRY_VERIFY([&]{auto done=std::make_shared<bool>(false);ops.inspect(id,[&op,done](Error,QJsonObject r){op=r;*done=true;});if(!putgd::test::waitFor([&]{return *done;}))return false;return op["state"]=="partial";}());
  QCOMPARE(op["items"].toArray()[0].toObject()["uploadStatus"].toString(),QString("unknown"));
  // The journal is durable on the item but never part of the public operation.
  QVERIFY(!op["items"].toArray()[0].toObject().contains("uploadJournal"));
  HistoryStore store(ops.databasePath());Error error;const auto saved=store.operation(id,&error);QVERIFY(!error);
  QCOMPARE(saved.items[0].uploadJournal,QJsonObject({{"schemaVersion",1},{"upload","opaque-resumable"}}));
  provider->journalThenUnknown=false;bool retried=false;ops.retry(id,[&](Error e,QJsonObject){QVERIFY(!e);retried=true;});PUTGD_TRY_VERIFY(retried);
  PUTGD_TRY_COMPARE(provider->uploads.load(),2);PUTGD_TRY_VERIFY(!ops.storageActive()&&ops.activeObservations().isEmpty());
  QMutexLocker lock(&provider->mutex);QVERIFY(provider->resumes[0].isEmpty());QCOMPARE(provider->resumes[1],saved.items[0].uploadJournal);
 }
 void destinationMaximumRejectsImportBeforeCopying() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->maximumObjectBytes=8;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  QJsonObject op;PUTGD_TRY_VERIFY([&]{auto done=std::make_shared<bool>(false);ops.inspect(id,[&op,done](Error,QJsonObject r){op=r;*done=true;});if(!putgd::test::waitFor([&]{return *done;}))return false;return op["state"]=="failed";}());
  QCOMPARE(op["error"].toObject()["code"].toString(),QString("destination_object_too_large"));QCOMPARE(provider->uploads.load(),0);
  QVERIFY(QDir(d.filePath("data/artifacts")).entryList(QDir::Dirs|QDir::NoDotAndDotDot|QDir::Hidden).isEmpty());
 }
 void uploadSlotsAreBoundedAndFair() {
  UploadSlots pool;pool.setCapacity(2);const auto never=[]{return false;};
  QVERIFY(pool.acquire("batch",never));QVERIFY(pool.acquire("batch",never));
  QCOMPARE(pool.diagnostics()["active"].toInt(),2);
  // A full pool: the batch asks first, a new action second; the new action wins the next slot.
  std::atomic_int order{0};int batch=0,action=0;
  std::thread third([&]{QVERIFY(pool.acquire("batch",never));batch=++order;});
  PUTGD_TRY_COMPARE(pool.diagnostics()["waiting"].toInt(),1);
  std::thread first([&]{QVERIFY(pool.acquire("action",never));action=++order;});
  PUTGD_TRY_COMPARE(pool.diagnostics()["waiting"].toInt(),2);
  pool.release("batch");PUTGD_TRY_COMPARE(order.load(),1);QCOMPARE(action,1);QCOMPARE(batch,0);
  pool.release("action");first.join();third.join();QCOMPARE(batch,2);
  // A waiter stops on its own cancellation, and raising the capacity admits more.
  std::atomic_bool stop{false};bool admitted=true;
  std::thread cancelled([&]{admitted=pool.acquire("other",[&]{return stop.load();});});
  PUTGD_TRY_COMPARE(pool.diagnostics()["waiting"].toInt(),1);stop=true;pool.wake();cancelled.join();QVERIFY(!admitted);
  std::thread raised([&]{admitted=pool.acquire("other",never);});
  PUTGD_TRY_COMPARE(pool.diagnostics()["waiting"].toInt(),1);pool.setCapacity(3);raised.join();QVERIFY(admitted);
  pool.close();QVERIFY(!pool.acquire("late",never));
 }
 void idleSlotsAreLentAndTakenBack() {
  UploadSlots pool;pool.setCapacity(4);const auto never=[]{return false;};
  QVERIFY(pool.acquire("large",never));
  // Nobody waits: the large upload borrows up to what it asks for.
  quint64 loan=0;QCOMPARE(pool.lend(loan,2),2);QVERIFY(loan!=0);QCOMPARE(pool.lend(loan,3),3);QCOMPARE(pool.lend(loan,5),3);
  QCOMPARE(pool.diagnostics()["active"].toInt(),4);QCOMPARE(pool.diagnostics()["lent"].toInt(),3);
  // A new action never waits behind lent slots: it takes one back at once.
  QElapsedTimer clock;clock.start();QVERIFY(pool.acquire("small",never));QVERIFY(clock.elapsed()<100);
  QCOMPARE(pool.diagnostics()["active"].toInt(),4);QCOMPARE(pool.lend(loan,3),2);
  // While anyone waits, a loan never grows.
  QVERIFY(pool.acquire("small",never));QVERIFY(pool.acquire("small",never));QCOMPARE(pool.lend(loan,3),0);
  std::atomic_bool admitted=false;std::thread waiter([&]{admitted=pool.acquire("waiting",never);});
  PUTGD_TRY_COMPARE(pool.diagnostics()["waiting"].toInt(),1);pool.release("small");PUTGD_TRY_VERIFY(admitted.load());waiter.join();QCOMPARE(pool.lend(loan,3),0);
  // Freed slots are lent again; a lower capacity takes them back; closing returns the rest.
  pool.release("small");pool.release("small");pool.release("waiting");QCOMPARE(pool.lend(loan,3),3);
  pool.setCapacity(2);QCOMPARE(pool.lend(loan,3),1);QCOMPARE(pool.diagnostics()["active"].toInt(),2);
  pool.closeLoan(loan);QCOMPARE(pool.diagnostics()["active"].toInt(),1);QCOMPARE(pool.diagnostics()["lent"].toInt(),0);
  pool.release("large");QCOMPARE(pool.diagnostics()["active"].toInt(),0);
 }
 void largeUploadBorrowsIdleSlotsUntilAnotherActionWaits() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->concurrency=8;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));
  // Distinct names: two uploads to one name would wait on its object lock, not on a slot.
  Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"large.bin","synthetic large file")};
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;QElapsedTimer clock;clock.start();
   (void)putgd::test::waitFor([&]{while(!provider->completions.isEmpty())provider->completions.takeFirst()();return operations.diagnostics()["activeOperations"].toInt()==0;},10000);});
  operations.invoke("clipboard",[&](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->completions.size(),1);
  // Three uploads at once by default: the only transfer may borrow the two idle slots.
  QVERIFY(provider->requests[0].extraLanes);QCOMPARE(provider->requests[0].extraLanes(3),2);
  // A second action starts at once, taking a lent slot back.
  source.payload.files={fixture(d.path(),"small.txt","synthetic small file")};
  QElapsedTimer clock;clock.start();operations.invoke("clipboard",[&](Error e,QJsonObject){QVERIFY(!e);});
  PUTGD_TRY_COMPARE(provider->completions.size(),2);QVERIFY2(clock.elapsed()<2000,qPrintable(QString::number(clock.elapsed())));
  QCOMPARE(provider->requests[0].extraLanes(3),1);
  provider->completions.takeAt(1)();PUTGD_TRY_COMPARE(provider->requests[0].extraLanes(3),2);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(operations.diagnostics()["uploadSlots"].toObject()["active"].toInt(),0);
  QCOMPARE(operations.diagnostics()["uploadSlots"].toObject()["lent"].toInt(),0);
 }
 void retiredUploadJournalsAreAbortedBestEffort() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->journalThenFail=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());
  QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  HistoryStore store(ops.databasePath());PUTGD_TRY_VERIFY(QStringList({"partial","failed"}).contains(store.operation(id).state));QCOMPARE(store.operation(id).items[0].uploadStatus,QString("failed"));
  // The journal says partial before the service releases the operation; cleanup skips active ones.
  PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);
  auto op=store.operation(id);QCOMPARE(op.items[0].uploadJournal["uploadId"].toString(),QString("opaque-upload"));
  // A failed item keeps its artifact for Retry until failedArtifactDays pass.
  QJsonObject result;bool cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);result=r;cleaned=true;});PUTGD_TRY_VERIFY(cleaned);
  QCOMPARE(result["removedOperations"].toInt(),0);QVERIFY(provider->abandoned.isEmpty());
  op.created="2000-01-01T00:00:00.000Z";QVERIFY(!store.saveOperation(op));const auto spool=op.items[0].artifact.path;
  // A refused abort is reported and recorded, and the artifact still goes.
  provider->abandonError={"fixture_denied","Synthetic abort denial",{{"requestStage","upload_abort"}}};
  cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);result=r;cleaned=true;});PUTGD_TRY_VERIFY(cleaned);
  QCOMPARE(result["removedOperations"].toInt(),1);QCOMPARE(result["abortedUploads"].toInt(),0);QVERIFY(!QFile::exists(spool));
  QCOMPARE(provider->abandoned.size(),1);QCOMPARE(provider->abandoned[0]["uploadId"].toString(),QString("opaque-upload"));
  const auto kept=result["incompleteUploads"].toArray();QCOMPARE(kept.size(),1);
  QCOMPARE(kept[0].toObject()["operationId"].toString(),id);QCOMPARE(kept[0].toObject()["error"].toObject()["code"].toString(),QString("fixture_denied"));
  op=store.operation(id);QVERIFY(op.items[0].error.details["incompleteUpload"].toBool());QCOMPARE(op.items[0].uploadJournal["uploadId"].toString(),QString("opaque-upload"));
  // #226: the pass words what it left for the user once; the item remembers it was told.
  QSignalSpy kept226(&ops,&OperationService::incompleteUploadsKept);
  QVERIFY(!kept[0].toObject()["reported"].toBool());QVERIFY(op.items[0].error.details["incompleteUploadReported"].toBool());
  auto notice=result["incompleteUploadNotice"].toObject();QCOMPARE(notice["count"].toInt(),1);QCOMPARE(notice["bytes"].toInteger(),op.items[0].artifact.bytes);QVERIFY(!notice["notified"].toBool());
  QCOMPARE(notice["message"].toString(),QString("Cleanup could not cancel an incomplete upload at Synthetic opaque provider (up to %1 of stored parts). Synthetic abort denial. Its parts keep using storage. Check that put.gd's account may cancel uploads at the destination. put.gd tries again at each cleanup.").arg(ArtifactStore::sizeText(op.items[0].artifact.bytes)));
  QCOMPARE(ops.diagnostics()["artifactCleanup"].toObject()["incompleteUploadNotice"].toObject(),notice);
  // Refused again on the next passes, manual or automatic, it is listed but not announced again.
  cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);result=r;cleaned=true;});PUTGD_TRY_VERIFY(cleaned);
  QCOMPARE(result["incompleteUploads"].toArray().size(),1);QVERIFY(result["incompleteUploads"].toArray()[0].toObject()["reported"].toBool());QVERIFY(!result.contains("incompleteUploadNotice"));
  auto now=QDateTime::currentDateTimeUtc();ops.setClockForTesting([&]{return now;});
  const auto last=[&]{return ops.diagnostics()["artifactCleanup"].toObject()["last"].toObject();};
  const auto automaticPass=[&]{now=now.addSecs(1);ops.cleanupExpired();};
  const auto passed=[&]{return last()["finished"].toString()==now.toString(Qt::ISODateWithMs)&&!ops.diagnostics()["artifactCleanup"].toObject()["running"].toBool();};
  automaticPass();PUTGD_TRY_VERIFY(passed());QVERIFY(!last().contains("incompleteUploadNotice"));QCOMPARE(kept226.size(),0);
  // A newly expired upload refused on an automatic pass notifies once; the hourly passes after it don't.
  QString second;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});
  PUTGD_TRY_VERIFY(!second.isEmpty()&&QStringList({"partial","failed"}).contains(store.operation(second).state));PUTGD_TRY_COMPARE(ops.diagnostics()["activeOperations"].toInt(),0);
  now=now.addDays(8);automaticPass();PUTGD_TRY_VERIFY(passed());QCOMPARE(last()["removedOperations"].toInt(),1);
  QCOMPARE(kept226.size(),1);notice=kept226[0][0].toJsonObject();QCOMPARE(notice["count"].toInt(),1);QVERIFY(notice["notified"].toBool());
  QVERIFY2(notice["message"].toString().startsWith("Automatic cleanup could not cancel an incomplete upload at Synthetic opaque provider"),qPrintable(notice["message"].toString()));
  QVERIFY(store.operation(second).items[0].error.details["incompleteUploadReported"].toBool());
  automaticPass();PUTGD_TRY_VERIFY(passed());QVERIFY(!last().contains("incompleteUploadNotice"));QCOMPARE(kept226.size(),1);
  QCOMPARE(ops.diagnostics()["artifactCleanup"].toObject()["incompleteUploadNotice"].toObject(),notice);
  // The next cleanup tries again, and a success clears the journal for good.
  provider->abandonError={};cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);result=r;cleaned=true;});PUTGD_TRY_VERIFY(cleaned);
  QCOMPARE(result["removedOperations"].toInt(),0);QCOMPARE(result["abortedUploads"].toInt(),2);QVERIFY(result["incompleteUploads"].toArray().isEmpty());
  op=store.operation(id);QVERIFY(op.items[0].uploadJournal.isEmpty());QVERIFY(!op.items[0].error.details.contains("incompleteUpload"));QVERIFY(!op.items[0].error.details.contains("incompleteUploadReported"));
  const auto attempts=provider->abandoned.size();
  cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);result=r;cleaned=true;});PUTGD_TRY_VERIFY(cleaned);QCOMPARE(provider->abandoned.size(),attempts);
 }
 // #202: retention applies without the user, on the resident's own clock.
 void automaticCleanupAppliesRetentionWithoutTheUser() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->journalThenFail=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));
  QCOMPARE(settings.snapshot().failedArtifactDays,7);
  Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);
  auto now=QDateTime::currentDateTimeUtc();ops.setClockForTesting([&]{return now;});ops.setCleanupScheduleForTesting({0,60*60*1000,60*1000,50});
  const auto last=[&]{return ops.diagnostics()["artifactCleanup"].toObject()["last"].toObject();};
  const auto passAt=[&](const QDateTime& at){return last()["finished"].toString()==at.toString(Qt::ISODateWithMs)&&!ops.diagnostics()["artifactCleanup"].toObject()["running"].toBool();};
  // A pass runs after journal recovery (a minute later by default), with nobody asking.
  PUTGD_TRY_VERIFY(ops.ready());PUTGD_TRY_VERIFY(passAt(now));QCOMPARE(last()["trigger"].toString(),QString("automatic"));QCOMPARE(last()["removedOperations"].toInt(),0);
  QCOMPARE(ops.diagnostics()["artifactCleanup"].toObject()["intervalMs"].toInt(),60*60*1000);
  QStringList ids;
  for(int i=0;i<4;++i){QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());
   PUTGD_TRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return QStringList({"partial","failed"}).contains(store.operation(id).state);})());PUTGD_TRY_VERIFY(ops.activeObservations().isEmpty());ids<<id;}
  HistoryStore store(ops.databasePath());QStringList spools;for(const auto& id:ids)spools<<store.operation(id).items[0].artifact.path;
  // A preview the user's cache holds is left to Clean artifacts.
  const auto preview=d.filePath("data/previews/"+QUuid::createUuid().toString(QUuid::WithoutBraces)+".png");QVERIFY(QDir().mkpath(QFileInfo(preview).path()));
  {QFile file(preview);QVERIFY(file.open(QIODevice::WriteOnly));file.write("png");}
  // Six days on, every failed artifact stays for Retry.
  now=now.addDays(6);ops.cleanupExpired();PUTGD_TRY_VERIFY(passAt(now));
  QCOMPARE(last()["removedOperations"].toInt(),0);QVERIFY(provider->abandoned.isEmpty());for(const auto& spool:spools)QVERIFY(QFile::exists(spool));
  // Past retention a pass takes at most its batch, aborting each retained upload (#146).
  ops.setCleanupScheduleForTesting({0,60*60*1000,60*60*1000,1});now=now.addDays(2);ops.cleanupExpired();PUTGD_TRY_VERIFY(passAt(now));
  QCOMPARE(last()["removedOperations"].toInt(),1);QCOMPARE(last()["abortedUploads"].toInt(),1);QVERIFY(last()["more"].toBool());QCOMPARE(provider->abandoned.size(),1);
  QCOMPARE(int(std::count_if(spools.begin(),spools.end(),[](const QString& path){return !QFile::exists(path);})),1);QVERIFY(QFile::exists(preview));
  // A Retry during a pass stops it at the next operation and runs once it ends,
  // and a manual cleanup waits for it too instead of failing.
  ops.setCleanupScheduleForTesting({0,60*60*1000,60*60*1000,50});
  QSemaphore entered,release;std::atomic_bool block{true};provider->beforeAbandon=[&]{if(block.exchange(false)){entered.release();release.acquire();}};
  auto unblock=qScopeGuard([&]{release.release();});
  now=now.addSecs(60);ops.cleanupExpired();QVERIFY(entered.tryAcquire(1,10000));
  QStringList remaining;for(int i=0;i<ids.size();++i)if(QFile::exists(spools[i]))remaining<<ids[i];QCOMPARE(remaining.size(),3);std::sort(remaining.begin(),remaining.end());
  const auto retried=remaining.last();Error retryError;bool retryAnswered=false,manualAnswered=false;QJsonObject manual;
  ops.retry(retried,[&](Error e,QJsonObject){retryError=e;retryAnswered=true;});
  ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);manual=r;manualAnswered=true;});
  putgd::test::settle(100);PUTGD_TRY_VERIFY(!retryAnswered);PUTGD_TRY_VERIFY(!manualAnswered);
  release.release();unblock.dismiss();
  PUTGD_TRY_VERIFY(retryAnswered);QVERIFY2(!retryError,qPrintable(retryError.message));PUTGD_TRY_VERIFY(manualAnswered);
  // The pass ended after the operation it was on; the Retry kept its own artifact.
  QCOMPARE(QFile::exists(spools[ids.indexOf(remaining.first())]),false);QVERIFY(QFile::exists(spools[ids.indexOf(retried)]));
  QCOMPARE(manual["trigger"].toString(),QString("manual"));QVERIFY(!QFile::exists(preview));
  PUTGD_TRY_VERIFY(ops.activeObservations().isEmpty());ops.shutdown();provider->beforeAbandon={};
 }
 void concurrentBatchIsBoundedAndCopiesInSourceOrder() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->concurrency=8;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";for(int i=0;i<3;++i)source.payload.files.append(fixture(d.path(),QString("batch-%1.txt").arg(i),QByteArray("synthetic batch ")+QByteArray::number(i)));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  // A failed check must not leave a later upload waiting forever: complete what arrives until the operation ends.
  auto drain=qScopeGuard([&]{provider->deferUploads=false;QElapsedTimer clock;clock.start();
   (void)putgd::test::waitFor([&]{while(!provider->completions.isEmpty())provider->completions.takeFirst()();return operations.diagnostics()["activeOperations"].toInt()==0;},10000);});
  // The overlapping files reach the provider in either order: complete each by name.
  auto pending=[&]{QStringList names;for(const auto& request:provider->requests)names.append(request.candidate);return names;};
  auto finish=[&](const QString& name){const auto index=pending().indexOf(name);QVERIFY2(index>=0,qPrintable(name));provider->requests.removeAt(index);provider->completions.takeAt(index)();};
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  // Two files transfer at once; the third waits for a slot.
  PUTGD_TRY_COMPARE(provider->completions.size(),2);putgd::test::settle(150);PUTGD_TRY_COMPARE(provider->completions.size(),2);
  {auto names=pending();names.sort();QCOMPARE(names,(QStringList{"batch-0.txt","batch-1.txt"}));}
  PUTGD_TRY_VERIFY(operations.activeObservations().size()==1);
  {const auto items=operations.activeObservations()[0].toObject()["items"].toArray();QCOMPARE(items[2].toObject()["phase"].toString(),QString("queued"));}
  // The second file finishes first: its link waits for the first file.
  finish("batch-1.txt");PUTGD_TRY_COMPARE(provider->completions.size(),2);QVERIFY(pending().contains("batch-2.txt"));
  putgd::test::settle(100);PUTGD_TRY_COMPARE(source.copies,0);
  finish("batch-0.txt");PUTGD_TRY_COMPARE(source.copiedTexts.size(),2);
  finish("batch-2.txt");PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("copied"));
  const auto op=journal.operation(id);QCOMPARE(op.state,QString("completed"));QStringList expected;for(const auto& item:op.items)expected.append(item.receipt.directUrl);
  QCOMPARE(source.copiedTexts,expected);
 }
 // #478: Proton Drive's link arrives from the sharing step, after the file's
 // transfer has settled. each_link must still copy it.
 void linkThatFollowsTheTransferIsCopied() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<LateLinkProvider>();provider->dispatcher=this;provider->shareAll=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{(void)putgd::test::waitFor([&]{while(!provider->shares.isEmpty())provider->shares.takeFirst()();return operations.diagnostics()["activeOperations"].toInt()==0;},10000);});
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  // The transfer has ended without a link; the sharing step is still running.
  PUTGD_TRY_COMPARE(provider->shares.size(),1);
  {const auto op=journal.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QVERIFY(op.items[0].receipt.directUrl.isEmpty());QCOMPARE(op.copyStatus,QString("pending"));}
  PUTGD_COMPARE_STEADY(source.copies,0,100);
  provider->shares.takeFirst()();
  PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("copied"));
  const auto op=journal.operation(id);QCOMPARE(op.state,QString("completed"));QCOMPARE(op.items[0].shareStatus,QString("shared"));QVERIFY(!op.error);
  QCOMPARE(source.copies,1);QVERIFY(op.items[0].receipt.directUrl.startsWith("https://example.invalid/shared#"));QCOMPARE(source.copied.text,op.items[0].receipt.directUrl);
 }
 // #478: file 1's link comes with its upload, file 0's only from sharing. The
 // batch still copies in source order, so the clipboard ends on the last file.
 void earlierFileLinkFromSharingStillCopiesFirst() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<LateLinkProvider>();provider->dispatcher=this;provider->storage.dispatcher=this;provider->storage.deferUploads=true;provider->storage.concurrency=8;provider->shareLater={"late-0.txt"};registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";for(int i=0;i<2;++i)source.payload.files.append(fixture(d.path(),QString("late-%1.txt").arg(i),QByteArray("synthetic late link ")+QByteArray::number(i)));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->storage.deferUploads=false;(void)putgd::test::waitFor([&]{while(!provider->storage.completions.isEmpty())provider->storage.completions.takeFirst()();while(!provider->shares.isEmpty())provider->shares.takeFirst()();return operations.diagnostics()["activeOperations"].toInt()==0;},10000);});
  auto pending=[&]{QStringList names;for(const auto& request:provider->storage.requests)names.append(request.candidate);return names;};
  auto finish=[&](const QString& name){const auto index=pending().indexOf(name);QVERIFY2(index>=0,qPrintable(name));provider->storage.requests.removeAt(index);provider->storage.completions.takeAt(index)();};
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  PUTGD_TRY_COMPARE(provider->storage.completions.size(),2);
  // The second file finishes first, link included: it waits for the first.
  finish("late-1.txt");PUTGD_COMPARE_STEADY(source.copies,0,100);
  // The first file's transfer ends without a link; its sharing step is pending.
  finish("late-0.txt");PUTGD_TRY_COMPARE(provider->shares.size(),1);
  PUTGD_TRY_COMPARE(journal.operation(id).items[0].uploadStatus,QString("uploaded"));PUTGD_COMPARE_STEADY(source.copies,0,100);
  provider->shares.takeFirst()();
  PUTGD_TRY_COMPARE(journal.operation(id).copyStatus,QString("copied"));
  const auto op=journal.operation(id);QCOMPARE(op.state,QString("completed"));QStringList expected;for(const auto& item:op.items)expected.append(item.receipt.directUrl);
  QVERIFY(expected[0].startsWith("https://example.invalid/shared#"));QVERIFY(expected[1].startsWith("https://example.invalid/direct?"));
  QCOMPARE(source.copiedTexts,expected);QCOMPARE(source.copied.text,expected[1]);
 }
 void oneFailureLeavesTheRestOfTheBatch() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->concurrency=8;provider->failCandidates={"mixed-1.txt"};registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload.kind="files";for(int i=0;i<3;++i)source.payload.files.append(fixture(d.path(),QString("mixed-%1.txt").arg(i),QByteArray("synthetic mixed ")+QByteArray::number(i)));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  PUTGD_TRY_COMPARE(journal.operation(id).state,QString("partial"));const auto op=journal.operation(id);
  QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QCOMPARE(op.items[1].uploadStatus,QString("failed"));QCOMPARE(op.items[2].uploadStatus,QString("uploaded"));
 }
 void cancellingOneFileKeepsTheOthers() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->concurrency=8;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=1;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";for(int i=0;i<3;++i)source.payload.files.append(fixture(d.path(),QString("cancel-%1.txt").arg(i),QByteArray("synthetic cancel ")+QByteArray::number(i)));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());HistoryStore journal(operations.databasePath());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});
  QString id;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});
  PUTGD_TRY_COMPARE(provider->completions.size(),1);const auto waiting=journal.operation(id).items[1].artifact.id;
  bool replied=false;operations.cancel(id,[&](Error e,QJsonObject r){QVERIFY(!e);QCOMPARE(r["itemId"].toString(),waiting);replied=true;},waiting);PUTGD_TRY_VERIFY(replied);
  operations.cancel("not-active",[&](Error e,QJsonObject){QCOMPARE(e.code,QString("operation_inactive"));},"item");
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(provider->completions.size(),1);QCOMPARE(provider->requests.last().candidate,QString("cancel-2.txt"));
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(journal.operation(id).state,QString("partial"));
  auto op=journal.operation(id);QCOMPARE(op.items[0].uploadStatus,QString("uploaded"));QCOMPARE(op.items[1].uploadStatus,QString("cancelled"));QCOMPARE(op.items[2].uploadStatus,QString("uploaded"));
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
  // Retry uploads only the cancelled file.
  provider->deferUploads=false;operations.retry(id,[](Error e,QJsonObject){QVERIFY(!e);});
  PUTGD_TRY_COMPARE(journal.operation(id).state,QString("completed"));QCOMPARE(journal.operation(id).items[1].uploadStatus,QString("uploaded"));QCOMPARE(provider->uploads.load(),3);
 }
 void newActionTakesTheNextSlotBeforeTheRestOfABatch() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;provider->concurrency=8;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));
  Source source;source.payload.kind="files";for(int i=0;i<4;++i)source.payload.files.append(fixture(d.path(),QString("long-%1.txt").arg(i),QByteArray("synthetic long ")+QByteArray::number(i)));
  OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});
  QString batch,action;operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);batch=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),2);
  source.payload=textPayload();operations.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);action=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.reads,2);
  PUTGD_TRY_COMPARE(operations.activeObservations().size(),2);putgd::test::settle(150);PUTGD_TRY_COMPARE(provider->completions.size(),2);
  provider->completions.takeFirst()();PUTGD_TRY_COMPARE(provider->requests.size(),3);QVERIFY2(provider->requests[2].artifact.path.contains(action),qPrintable(provider->requests[2].artifact.path));
  provider->deferUploads=false;while(!provider->completions.isEmpty())provider->completions.takeFirst()();
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);QCOMPARE(provider->uploads.load(),5);
 }
 void oneRequestProvidersLeaseTheirDestination() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);
  SettingsService settings(d.filePath("config.json"),[&](auto id,auto doc){return registry.validate(id,doc);});QVERIFY(settings.save(configuration(d.path()),{}));
  Source source;source.payload=textPayload();OperationService operations(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(operations.ready());
  auto drain=qScopeGuard([&]{provider->deferUploads=false;for(auto complete:provider->completions)complete();provider->completions.clear();});
  operations.invoke("clipboard",[&](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(provider->completions.size(),1);QCOMPARE(provider->checks.load(),1);
  operations.invoke("clipboard",[&](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.reads,2);
  // The second operation makes no provider request until the first releases the destination.
  putgd::test::settle(250);PUTGD_TRY_COMPARE(provider->checks.load(),1);
  provider->completions.takeFirst()();PUTGD_TRY_VERIFY(provider->checks.load()>=2);PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();
  PUTGD_TRY_COMPARE(operations.diagnostics()["activeOperations"].toInt(),0);
 }
 void retriesRespectPendingOperationBound() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->deferUploads=true;provider->dispatcher=this;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["pendingOperations"]=1;limits["concurrentUploads"]=1;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString first,second;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);PUTGD_TRY_VERIFY(!first.isEmpty()&&!second.isEmpty());HistoryStore store(ops.databasePath());const auto running=store.operation(first).items.isEmpty()?second:first;Operation prior=store.operation(running);QCOMPARE(prior.items.size(),1);prior.id="c6b8d7c1-eaf8-4f74-a341-e7d3af064e18";prior.state="partial";prior.items[0].uploadStatus="failed";prior.items[0].artifact.id="historical-item";QVERIFY(!store.saveOperation(prior));QString copyCode;ops.copyResult(running,[&](Error e,QJsonObject){copyCode=e.code;});QCOMPARE(copyCode,QString("operation_busy"));QString retryCode,localCode;bool retryResponded=false,localResponded=false;ops.retry(prior.id,[&](Error e,QJsonObject){retryCode=e.code;retryResponded=true;});ops.retryLocalSave(prior.id,[&](Error e,QJsonObject){localCode=e.code;localResponded=true;});PUTGD_TRY_VERIFY(retryResponded&&localResponded);provider->deferUploads=false;provider->completions.takeFirst()();PUTGD_TRY_VERIFY(([&]{return store.operation(second).copyStatus=="copied"||store.operation(second).copyStatus=="suppressed";})());QCOMPARE(retryCode,QString("queue_full"));QCOMPARE(localCode,QString("queue_full"));
 }
 void concurrentAcquisitionRespectsAggregateBudget() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto limits=doc["limits"].toObject();limits["concurrentUploads"]=2;limits["pendingArtifactBytes"]=10;doc["limits"]=limits;QVERIFY(settings.save(doc,{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"six.txt","123456")};OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString first,second;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});PUTGD_TRY_VERIFY(!first.isEmpty()&&!second.isEmpty());HistoryStore store(ops.databasePath());PUTGD_TRY_VERIFY(([&]{auto a=store.operation(first),b=store.operation(second);return (a.error.code=="input_excessive"||b.error.code=="input_excessive")&&(a.state=="partial"||b.state=="partial");})());ArtifactStore artifacts(d.filePath("data/artifacts"));QCOMPARE(artifacts.bytes(),6);QCOMPARE(provider->uploads.load(),1);QCOMPARE(source.copies,0);
 }
 void emptyRegularFilesRejectTheEntireBatch() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto archive=doc["archive"].toObject();archive["keepUploads"]=true;doc["archive"]=archive;QVERIFY(settings.save(doc,{}));Source source;source.payload.kind="files";source.payload.files={fixture(d.path(),"valid.txt","synthetic bytes"),fixture(d.path(),"empty.txt",{})};OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(!id.isEmpty());HistoryStore store(ops.databasePath());PUTGD_TRY_COMPARE(store.operation(id).state,QString("failed"));const auto op=store.operation(id);QCOMPARE(op.error.code,QString("source_empty"));QCOMPARE(op.error.details["itemIndex"].toInt(),1);QVERIFY(op.items.isEmpty());QCOMPARE(provider->uploads.load(),0);QCOMPARE(source.copies,0);QVERIFY(!QDir(d.filePath("archive")).exists());ArtifactStore artifacts(d.filePath("data/artifacts"));QCOMPARE(artifacts.bytes(),0);
 }
 void lateAcquisitionAfterDestructionIsIgnored() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.defer=true;std::function<void(PayloadResult)> late;
  {OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.acquisitions.size(),1);late=source.acquisitions.takeFirst();}
  late({textPayload(),{}});putgd::test::settle(20);PUTGD_TRY_COMPARE(source.copies,0);PUTGD_TRY_COMPARE(QDir(d.filePath("data/artifacts")).entryList(QDir::Dirs|QDir::NoDotAndDotDot).size(),0);
 }
 void selectorBelongsToItsOwnOperation() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->dispatcher=this;provider->deferUploads=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});QVERIFY(settings.save(configuration(d.path()),{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString first,second;ops.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);first=r["operationId"].toString();});PUTGD_TRY_COMPARE(provider->completions.size(),1);source.defer=true;ops.invoke("window",[&](Error e,QJsonObject r){QVERIFY(!e);second=r["operationId"].toString();});PUTGD_TRY_COMPARE(source.acquisitions.size(),1);ops.cancel(first,[](Error e,QJsonObject){QVERIFY(!e);});QCOMPARE(source.acquisitions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return store.operation(first).state=="cancelled";})());QString code;ops.invoke("desktop",[&](Error e,QJsonObject){code=e.code;});QCOMPARE(code,QString("busy_selecting"));ops.cancel(second,[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_COMPARE(source.acquisitions.size(),0);
 }
 void zeroRetentionAndActiveRetryPreserveArtifacts() {
  QTemporaryDir d;ProviderRegistry registry;auto provider=std::make_shared<OpaqueProvider>();provider->failUpload=true;registry.add("opaque",provider);SettingsService settings(d.filePath("config.json"),[&](auto id,auto value){return registry.validate(id,value);});auto doc=configuration(d.path());auto retention=doc["retention"].toObject();retention["failedArtifactDays"]=0;doc["retention"]=retention;QVERIFY(settings.save(doc,{}));Source source;source.payload=textPayload();OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;ops.invoke("clipboard",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();});PUTGD_TRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return store.operation(id).copyStatus=="no_result";})());HistoryStore store(ops.databasePath());auto op=store.operation(id);op.created="2000-01-01T00:00:00.000Z";QVERIFY(!store.saveOperation(op));const auto spool=op.items[0].artifact.path;bool cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);QCOMPARE(r["removedOperations"].toInt(),0);cleaned=true;});PUTGD_TRY_VERIFY(cleaned);QVERIFY(QFile::exists(spool));
  provider->failUpload=false;provider->deferUploads=true;provider->dispatcher=this;ops.retry(id,[](Error e,QJsonObject){QVERIFY(!e);});cleaned=false;ops.cleanup([&](Error e,QJsonObject r){QVERIFY(!e);QCOMPARE(r["removedOperations"].toInt(),0);cleaned=true;});PUTGD_TRY_VERIFY(cleaned);QVERIFY(QFile::exists(spool));PUTGD_TRY_COMPARE(provider->completions.size(),1);provider->completions.takeFirst()();PUTGD_TRY_VERIFY(([&]{return store.operation(id).items[0].uploadStatus=="uploaded";})());
 }
 void interactiveAcceptanceAndCancellation() {
  QTemporaryDir d;ProviderRegistry registry;SettingsService settings(d.filePath("config.json"));QVERIFY(settings.save(configuration(d.path(),true),{}));Source source;source.defer=true;OperationService ops(d.filePath("data"),settings,registry,source);PUTGD_TRY_VERIFY(ops.ready());QString id;bool startedAtAcceptance=true;ops.invoke("region",[&](Error e,QJsonObject r){QVERIFY(!e);id=r["operationId"].toString();startedAtAcceptance=source.reads!=0;});PUTGD_TRY_VERIFY(!id.isEmpty());QVERIFY(!startedAtAcceptance);PUTGD_TRY_COMPARE(source.acquisitions.size(),1);QString code;ops.invoke("window",[&](Error e,QJsonObject){code=e.code;});QCOMPARE(code,QString("busy_selecting"));ops.cancel(id,[](Error e,QJsonObject){QVERIFY(!e);});PUTGD_TRY_VERIFY(([&]{HistoryStore store(ops.databasePath());return store.operation(id).state=="cancelled";})());QCOMPARE(source.copies,0);
 }
};
QTEST_GUILESS_MAIN(OperationsTest)
#include "test_operations.moc"
