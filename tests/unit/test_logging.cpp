#include "app/LogService.h"
#include "core/Log.h"
#include "core/RunMarker.h"
#include "storage/s3/SdkRuntime.h"
#include "fixtures/U2Fixtures.h"
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#ifdef Q_OS_UNIX
#include <csignal>
#include <sys/resource.h>
#endif
using namespace putgd;
namespace {
// Each canary is a value that must never reach a log line (#74 §1).
const QStringList canaries{"synthetic-private-secret","synthetic-private-token","synthetic-private-signature","synthetic-private-password",
 "synthetic-private-clipboard","synthetic-private-bytes","AKIASYNTHETIC0000001","wJalrXUtnFEMI/K7MDENG/bPxRfiCYsynthetic1","synthetic-private-share"};
QByteArray readLogs(const QString& directory) {
 QByteArray bytes;for(const auto& name:QDir(directory).entryList({"putgd-*.jsonl"},QDir::Files,QDir::Name)) {QFile file(directory+"/"+name);if(file.open(QIODevice::ReadOnly))bytes+=file.readAll();}
 return bytes;
}
QList<QJsonObject> records(const QByteArray& bytes) {
 QList<QJsonObject> result;
 for(const auto& line:bytes.split('\n')) {if(line.isEmpty())continue;QJsonParseError error;const auto document=QJsonDocument::fromJson(line,&error);if(error.error!=QJsonParseError::NoError||!document.isObject())return {};result.append(document.object());}
 return result;
}
void verifyNoCanary(const QByteArray& bytes) {for(const auto& canary:canaries)QVERIFY2(!bytes.contains(canary.toUtf8()),qPrintable(canary));}
// A provider whose every upload fails the way S3 reports a denied PUT.
class DeniedProvider final:public StorageProvider {
public:
 ProviderCapabilities capabilities(const Destination&) const override {return {true,true,true,true,true,true,true,255,"Remove the recorded object"};}
 QString serializationKey(const Destination&,const QJsonObject& reference,const QString& name) const override {return reference.isEmpty()?name:reference["key"].toString();}
 Error validateSettings(const QJsonObject& settings) const override {return settings["workspace"].isString()?Error{}:Error{"config_invalid","A workspace is required",{}};}
 void checkName(const Destination&,const QString& name,std::function<void(NameCheck)> reply) override {reply({"absent",{{"key",name}},{},{}});}
 void upload(const UploadRequest&,std::function<void(UploadResult)> reply) override {
  reply({"failed",{},{"s3_permission_denied","S3 denied the request (HTTP 403).",{{"httpStatus",403},{"serviceCode","AccessDenied"},{"requestStage","upload_put"},
   {"requestId","SYNTHREQ0123ABCD"},{"extendedRequestId","synthetic+Extended/Id2="}}}});
 }
 void stat(const StorageReceipt&,std::function<void(StatResult)> reply) override {reply({"missing",{},{}});}
 void reconcile(const UploadRequest&,std::function<void(StatResult)> reply) override {reply({"missing",{},{}});}
 void remove(const DeleteRequest&,DurableCheckpoint,std::function<void(DeleteResult)> reply) override {reply({"removed",{},{}});}
};
}
class LoggingContract final:public QObject {
 Q_OBJECT
private slots:
 void cleanup() {logging::close();logging::setDebug(false);}
 void redactionRemovesSecretsButKeepsDiagnosis() {
  const QStringList inputs{
   "Authorization: AWS4-HMAC-SHA256 Credential=AKIASYNTHETIC0000001/20261019/us-east-2/s3/aws4_request, Signature=synthetic-private-signature",
   "X-Amz-Security-Token: synthetic-private-token",
   "GET https://bucket.example.invalid/key.png?X-Amz-Credential=AKIASYNTHETIC0000001&X-Amz-Signature=synthetic-private-signature HTTP/1.1",
   "share https://drive.proton.me/urls/ABCDEF#synthetic-private-share",
   "aws_secret_access_key=synthetic-private-secret",
   "{\"secretKey\":\"synthetic-private-secret\",\"sessionToken\":\"synthetic-private-token\"}",
   "password: synthetic-private-password",
   "Bearer synthetic-private-token",
   "secret wJalrXUtnFEMI/K7MDENG/bPxRfiCYsynthetic1 in prose",
   "-----BEGIN PRIVATE KEY-----\nsynthetic-private-secret\n-----END PRIVATE KEY-----"};
  for(const auto& input:inputs) {
   const auto redacted=logging::redact(input);
   for(const auto& canary:canaries)QVERIFY2(!redacted.contains(canary),qPrintable(redacted));
  }
  // What a maintainer needs stays readable.
  QCOMPARE(logging::redact(QString("HTTP 403 AccessDenied request SYNTHREQ0123ABCD id2 synthetic+Extended/Id2= commit 0123456789abcdef0123456789abcdef01234567")),
   QString("HTTP 403 AccessDenied request SYNTHREQ0123ABCD id2 synthetic+Extended/Id2= commit 0123456789abcdef0123456789abcdef01234567"));
  QCOMPARE(logging::redact(QString("https://put.gd/Screen-Capture.png")),QString("https://put.gd/Screen-Capture.png"));
  const auto fields=logging::redact(QJsonObject{{"password","synthetic-private-password"},{"clipboardText","synthetic-private-clipboard"},
   {"credentials",QJsonObject{{"secretKey","synthetic-private-secret"}}},{"nested",QJsonArray{QJsonObject{{"token","synthetic-private-token"}}}},{"httpStatus",403}}).toObject();
  verifyNoCanary(QJsonDocument(fields).toJson());
  QCOMPARE(fields["httpStatus"].toInt(),403);
 }
 void recordsAreRedactedJsonLinesInAPrivateFolder() {
  QTemporaryDir dir;const auto logs=dir.filePath("state/putgd/logs");
  logging::open(logs,{},"putgd-test");
  logging::warning(logS3(),"Upload failed: Authorization: AWS4-HMAC-SHA256 Credential=AKIASYNTHETIC0000001/x, Signature=synthetic-private-signature",
   {{"operationId","op-1"},{"itemId","item-1"},{"error",QJsonObject{{"code","s3_permission_denied"},{"details",QJsonObject{{"httpStatus",403},{"requestId","SYNTHREQ0123ABCD"}}}}},
    {"secretKey","synthetic-private-secret"},{"url","https://bucket.example.invalid/a.png?X-Amz-Signature=synthetic-private-signature"},{"content","synthetic-private-bytes"}});
  qWarning("Qt warning with password=synthetic-private-password");
  logging::debug(logOperation(),"debug records stay out by default");
  logging::flush();
  const auto bytes=readLogs(logs);verifyNoCanary(bytes);
  const auto lines=records(bytes);QCOMPARE(lines.size(),2);
  const auto first=lines[0];
  QCOMPARE(first["category"].toString(),QString("putgd.s3"));QCOMPARE(first["level"].toString(),QString("warning"));
  QCOMPARE(first["process"].toString(),QString("putgd-test"));QCOMPARE(first["pid"].toInteger(),QCoreApplication::applicationPid());
  QCOMPARE(first["operationId"].toString(),QString("op-1"));QCOMPARE(first["itemId"].toString(),QString("item-1"));
  QCOMPARE(first["fields"].toObject()["error"].toObject()["details"].toObject()["requestId"].toString(),QString("SYNTHREQ0123ABCD"));
  QVERIFY(QDateTime::fromString(first["time"].toString(),Qt::ISODateWithMs).isValid());
  QCOMPARE(lines[1]["category"].toString(),QString("default"));
#ifdef Q_OS_UNIX
  QCOMPARE(QFileInfo(logs).permissions()&(QFile::ReadGroup|QFile::ReadOther|QFile::WriteGroup|QFile::WriteOther|QFile::ExeGroup|QFile::ExeOther),QFile::Permissions{});
  QCOMPARE(QFileInfo(logging::currentFile()).permissions()&(QFile::ReadGroup|QFile::ReadOther|QFile::WriteGroup|QFile::WriteOther),QFile::Permissions{});
#endif
  const auto status=logging::status();
  QVERIFY(status["available"].toBool());QCOMPARE(status["files"].toInt(),1);QVERIFY(status["bytes"].toInteger()>0);QVERIFY(!status["degraded"].toBool());
  QCOMPARE(status["oldestRecord"].toString(),first["time"].toString());
 }
 void debugLoggingIsOptInAndRulesStillApply() {
  QTemporaryDir dir;logging::open(dir.path());
  logging::debug(logOperation(),"hidden");
  logging::setDebug(true,QDateTime::currentDateTimeUtc().addSecs(60));
  logging::debug(logOperation(),"shown");qDebug("qt debug shown");
  QVERIFY(!logging::status()["debugLoggingExpiresAt"].isNull());
  logging::setDebug(false);
  logging::debug(logOperation(),"hidden again");
  logging::flush();
  const auto bytes=readLogs(dir.path());
  QVERIFY(!bytes.contains("hidden"));QVERIFY(bytes.contains("\"shown\""));QVERIFY(bytes.contains("qt debug shown"));
 }
 void rotationAndRetentionBoundTheFolder() {
  QTemporaryDir dir;
  // An old file and an unrelated file: only the first is the log's to delete.
  const auto old=dir.filePath("putgd-20200101T000000000Z-1-1.jsonl");QFile stale(old);QVERIFY(stale.open(QIODevice::WriteOnly));stale.write("{}\n");stale.flush();
  stale.setFileTime(QDateTime::currentDateTimeUtc().addDays(-30),QFileDevice::FileModificationTime);stale.close();
  QFile other(dir.filePath("notes.txt"));QVERIFY(other.open(QIODevice::WriteOnly));other.close();
  logging::open(dir.path(),{14,1024*1024});
  const auto filler=QString("lorem ipsum ").repeated(75);
  for(int index=0;index<2500;++index)logging::info(logApp(),filler,{{"index",index}});
  logging::flush();logging::cleanup();logging::flush();
  QVERIFY(!QFile::exists(old));QVERIFY(QFile::exists(dir.filePath("notes.txt")));
  const auto files=logging::files();QVERIFY2(files.size()>=3,qPrintable(files.join(',')));
  qint64 total=0;for(const auto& name:files){const auto size=QFileInfo(dir.filePath(name)).size();QVERIFY(size<=256*1024);total+=size;}
  QVERIFY2(total<=1024*1024,qPrintable(QString::number(total)));
  QCOMPARE(logging::status()["bytes"].toInteger(),total);
  // The newest records survive; the oldest went first.
  const auto lines=records(readLogs(dir.path()));QVERIFY(!lines.isEmpty());
  QCOMPARE(lines.last()["fields"].toObject()["index"].toInt(),2499);QVERIFY(lines.first()["fields"].toObject()["index"].toInt()>0);
  // Tightening the limit applies at once.
  logging::setRetention({1,1024*1024});logging::flush();
  Error error;const auto cleared=logging::clear(error);QVERIFY(!error);
  QCOMPARE(cleared["removedFiles"].toInt(),int(files.size()));
  const auto after=records(readLogs(dir.path()));QCOMPARE(after.size(),1);QCOMPARE(after[0]["message"].toString(),QString("Logs cleared"));
 }
 void failedWritesDegradeWithoutBlocking() {
  QTemporaryDir dir;QFile blocker(dir.filePath("not-a-folder"));QVERIFY(blocker.open(QIODevice::WriteOnly));blocker.close();
  logging::open(dir.filePath("not-a-folder/logs"));
  QElapsedTimer clock;clock.start();
  for(int index=0;index<10000;++index)logging::info(logApp(),"dropped");
  QVERIFY2(clock.elapsed()<5000,"logging must never hold up its caller");
  logging::flush();
  const auto status=logging::status();
  QVERIFY(status["degraded"].toBool());QVERIFY(status["droppedRecords"].toInteger()>0);
  logging::close();
#ifdef Q_OS_UNIX
  // A full disk: writes beyond the file size limit fail with EFBIG.
  QTemporaryDir full;logging::open(full.path());logging::info(logApp(),"first");logging::flush();
  ::signal(SIGXFSZ,SIG_IGN);rlimit previous{};::getrlimit(RLIMIT_FSIZE,&previous);rlimit limited=previous;limited.rlim_cur=QFileInfo(logging::currentFile()).size()+10;::setrlimit(RLIMIT_FSIZE,&limited);
  logging::info(logApp(),QString(200,'y'));logging::flush();
  ::setrlimit(RLIMIT_FSIZE,&previous);
  QVERIFY(logging::status()["degraded"].toBool());
  logging::info(logApp(),"recovered");logging::flush();
  QVERIFY(!logging::status()["degraded"].toBool());
  // The failed record left no partial line behind.
  QVERIFY(!records(readLogs(full.path())).isEmpty());
#endif
 }
 void helperOutputIsBoundedAndKeepsAwsLevels() {
  QTemporaryDir dir;logging::open(dir.path());
  HelperOutput output("putgd-test-helper",logS3());
  output.started(4242,{{"generation",1}});
  output.append("putgd-aws\tE\tAWSClient\tHTTP response code: 403 | Request ID: SYNTHREQ0123ABCD | Authorization: synthetic-private-token\n");
  output.append("plain warning with aws_secret_access_key=synthetic-private-secret\npartial ");
  output.append("line\n");
  for(int index=0;index<100;++index)output.append(QByteArray(1000,'z')+'\n');
  output.finished(11,QProcess::CrashExit);
  logging::flush();
  const auto bytes=readLogs(dir.path());verifyNoCanary(bytes);
  const auto lines=records(bytes);QVERIFY(lines.size()>=6);
  QCOMPARE(lines[0]["message"].toString(),QString("Helper started"));
  QCOMPARE(lines[1]["level"].toString(),QString("error"));QCOMPARE(lines[1]["process"].toString(),QString("putgd-test-helper"));QCOMPARE(lines[1]["pid"].toInteger(),4242);
  QCOMPARE(lines[1]["fields"].toObject()["awsTag"].toString(),QString("AWSClient"));QVERIFY(lines[1]["message"].toString().contains("SYNTHREQ0123ABCD"));
  QCOMPARE(lines[3]["message"].toString(),QString("partial line"));
  QVERIFY(bytes.size()<HelperOutput::generationBytes*2);
  bool truncated=false,crashed=false;
  for(const auto& line:lines) {truncated=truncated||line["message"]=="Helper output truncated";if(line["message"]=="Helper crashed"){crashed=true;QCOMPARE(line["fields"].toObject()["signal"].toInt(),11);QCOMPARE(line["level"].toString(),QString("error"));}}
  QVERIFY(truncated);QVERIFY(crashed);
 }
 void awsTraceCannotBeSelected() {
  QCOMPARE(SdkRuntime::logLevel(nullptr),SdkRuntime::LogLevel::Warn);
  QCOMPARE(SdkRuntime::logLevel("warn"),SdkRuntime::LogLevel::Warn);
  QCOMPARE(SdkRuntime::logLevel("info"),SdkRuntime::LogLevel::Info);
  QCOMPARE(SdkRuntime::logLevel("error"),SdkRuntime::LogLevel::Error);
  for(const char* value:{"trace","debug","Trace","DEBUG","6","5",""})QCOMPARE(SdkRuntime::logLevel(value),SdkRuntime::LogLevel::Warn);
 }
 void uncleanExitIsDetectedOnTheNextStart() {
  QTemporaryDir dir;logging::open(dir.filePath("logs"));
  RunMarker::begin(dir.path(),"9.9.9");QVERIFY(!RunMarker::report()["detected"].toBool());QVERIFY(QFile::exists(dir.filePath("running.json")));
  RunMarker::end();QVERIFY(!QFile::exists(dir.filePath("running.json")));
  RunMarker::begin(dir.path(),"9.9.9");QVERIFY(!RunMarker::report()["detected"].toBool());
  // A crash or power loss leaves the marker; a logout's SIGTERM leaves a note too.
  auto marker=[&](bool signalled){
   QSaveFile file(dir.filePath("running.json"));QVERIFY(file.open(QIODevice::WriteOnly));
   file.write(QJsonDocument(QJsonObject{{"pid",2147483000},{"started","2026-10-19T00:00:00.000Z"},{"version","9.9.8"},{"bootId","synthetic"}}).toJson());QVERIFY(file.commit());
   QFile note(dir.filePath("running.signal"));QVERIFY(note.open(QIODevice::WriteOnly|QIODevice::Truncate));if(signalled)note.write("15\n");note.close();
  };
  RunMarker::end();marker(false);RunMarker::begin(dir.path(),"9.9.9");
  auto report=RunMarker::report();QVERIFY(report["detected"].toBool());
  QCOMPARE(report["last"].toObject()["cause"].toString(),QString("unknown"));QCOMPARE(report["last"].toObject()["previousVersion"].toString(),QString("9.9.8"));
  RunMarker::end();marker(true);RunMarker::begin(dir.path(),"9.9.9");
  report=RunMarker::report();QCOMPARE(report["last"].toObject()["cause"].toString(),QString("signal"));QCOMPARE(report["last"].toObject()["signal"].toInt(),15);
  QCOMPARE(report["recent"].toArray().size(),2);
  for(int index=0;index<12;++index){RunMarker::end();marker(false);RunMarker::begin(dir.path(),"9.9.9");}
  QCOMPARE(RunMarker::report()["recent"].toArray().size(),RunMarker::retainedRecords);
  RunMarker::end();
  logging::flush();
  const auto lines=records(readLogs(dir.filePath("logs")));
  QVERIFY(std::any_of(lines.begin(),lines.end(),[](const QJsonObject& line){return line["message"]=="The previous resident did not shut down cleanly"&&line["level"]=="warning";}));
  QVERIFY(std::any_of(lines.begin(),lines.end(),[](const QJsonObject& line){return line["message"]=="The previous resident was ended by a signal"&&line["level"]=="info";}));
 }
 void settingsValidateTheNewKeys() {
  auto document=Settings::defaults();QVERIFY(!Settings::validate(document));
  auto retention=document["retention"].toObject();retention["logDays"]=30;retention["logBytes"]=10485760;document["retention"]=retention;
  auto preferences=document["preferences"].toObject();preferences["diagnostics"]=QJsonObject{{"debugLogging",true}};document["preferences"]=preferences;
  QVERIFY(!Settings::validate(document));
  const auto parsed=Settings::fromValidated(document);QCOMPARE(parsed.logDays,30);QCOMPARE(parsed.logBytes,10485760LL);QVERIFY(parsed.debugLogging);
  const auto defaults=Settings::fromValidated(Settings::defaults());QCOMPARE(defaults.logDays,14);QCOMPARE(defaults.logBytes,52428800LL);QVERIFY(!defaults.debugLogging);
  const QList<QPair<QString,QJsonValue>> invalid{{"logDays",0},{"logDays",366},{"logDays",1.5},{"logBytes",1048575},{"logBytes",1073741825},{"logBytes","50"}};
  for(const auto& [key,value]:invalid){auto changed=document;auto r=changed["retention"].toObject();r[key]=value;changed["retention"]=r;const auto error=Settings::validate(changed);QVERIFY(error);QCOMPARE(error.details["path"].toString(),"/retention/"+key);}
  for(const auto& value:QList<QJsonValue>{QJsonObject{},QJsonObject{{"debugLogging","yes"}},QJsonObject{{"debugLogging",true},{"extra",1}},true}){auto changed=document;auto p=changed["preferences"].toObject();p["diagnostics"]=value;changed["preferences"]=p;QCOMPARE(Settings::validate(changed).details["path"].toString(),QString("/preferences/diagnostics"));}
 }
 void debugLoggingTurnsItselfOffAfterADay() {
  QTemporaryDir dir;logging::open(dir.filePath("logs"));
  SettingsService settings(dir.filePath("config.json"));auto document=Settings::defaults();
  auto preferences=document["preferences"].toObject();preferences["diagnostics"]=QJsonObject{{"debugLogging",true}};document["preferences"]=preferences;
  QVERIFY(settings.save(document,{}));
  auto now=QDateTime::currentDateTimeUtc();
  LogService service(settings,dir.path());service.setClock([&]{return now;});service.apply();
  QVERIFY(logging::debug());QVERIFY(QFile::exists(dir.filePath("debug-logging.json")));
  QCOMPARE(service.debugStatus()["expiresAt"].toString(),now.addMSecs(LogService::debugLifetimeMs).toString(Qt::ISODateWithMs));
  // A restart keeps the original start time instead of extending it.
  now=now.addSecs(23*3600);
  LogService restarted(settings,dir.path());restarted.setClock([&]{return now;});restarted.apply();
  QVERIFY(logging::debug());QCOMPARE(restarted.debugStatus()["expiresAt"],service.debugStatus()["expiresAt"]);
  now=now.addSecs(3600+1);restarted.apply();
  QVERIFY(!logging::debug());
  QTRY_VERIFY(!settings.snapshot().debugLogging);
  QTRY_VERIFY(!QFile::exists(dir.filePath("debug-logging.json")));
 }
 void failedUploadIsLoggedWithoutPrivateContent() {
  QTemporaryDir dir;logging::open(dir.filePath("logs"));
  ProviderRegistry registry;registry.add("opaque",std::make_shared<DeniedProvider>());
  SettingsService settings(dir.filePath("config.json"),[&registry](const auto& id,const auto& value){return registry.validate(id,value);});
  QVERIFY(settings.save(test::configuration(dir.path()),{}));
  test::Source source;source.payload.kind="text";source.payload.bytes="synthetic-private-clipboard text that must stay out of every log";
  OperationService operations(dir.filePath("data"),settings,registry,source);QTRY_VERIFY(operations.ready());
  QString id;bool accepted=false;
  operations.invoke("clipboard",[&](Error error,QJsonObject result){QVERIFY(!error);id=result["operationId"].toString();accepted=true;});
  QTRY_VERIFY(accepted);
  QList<QJsonObject> lines;
  auto find=[&](const QString& message){for(const auto& line:lines)if(line["message"]==message&&line["operationId"]==id)return line;return QJsonObject{};};
  QTRY_VERIFY_WITH_TIMEOUT((logging::flush(),lines=records(readLogs(dir.filePath("logs"))),!find("Operation finished").isEmpty()),10000);
  verifyNoCanary(readLogs(dir.filePath("logs")));
  QVERIFY(!find("Operation accepted").isEmpty());QVERIFY(!find("Artifacts acquired").isEmpty());QVERIFY(!find("Upload names chosen").isEmpty());QVERIFY(!find("Upload attempt").isEmpty());
  const auto failure=find("Upload failed");QVERIFY(!failure.isEmpty());QCOMPARE(failure["level"].toString(),QString("warning"));
  const auto details=failure["fields"].toObject()["error"].toObject()["details"].toObject();
  QCOMPARE(failure["fields"].toObject()["error"].toObject()["code"].toString(),QString("s3_permission_denied"));
  QCOMPARE(details["httpStatus"].toInt(),403);QCOMPARE(details["requestId"].toString(),QString("SYNTHREQ0123ABCD"));QCOMPARE(details["extendedRequestId"].toString(),QString("synthetic+Extended/Id2="));
  const auto final=find("Operation finished");QVERIFY(!final.isEmpty());QVERIFY(final["fields"].toObject().contains("durationMs"));
  QVERIFY(final["fields"].toObject()["byteCount"].toInteger()>0);
  // Diagnostics report the log without reading it.
  const auto diagnostics=operations.diagnostics();
  QVERIFY(diagnostics["logging"].toObject()["available"].toBool());QVERIFY(!diagnostics["loggingDegraded"].toBool());QVERIFY(diagnostics.contains("uncleanExit"));
  operations.shutdown();
 }
};
QTEST_GUILESS_MAIN(LoggingContract)
#include "test_logging.moc"
