#include "platform/x11/CaptureProcessController.h"
#include <QTest>
#include <QJsonArray>
#include <QColor>
#include <memory>
#include <QImageReader>
#include "platform/x11/CaptureArtifactLease.h"
#include "core/BuildInfo.h"
#include <QJsonDocument>
#include <QDir>
#include <QUuid>
#include <sys/stat.h>
QJsonObject snapshot(){return {{"schemaVersion",1},{"action","desktop"},{"window","0"},{"windowRect",QJsonArray{}},{"layout",QJsonObject{{"outputs",QJsonArray{QJsonObject{{"identity","monitor"},{"native",QJsonArray{0,0,640,480}},{"rotation",1}}}},{"configuration","1"},{"root",QJsonArray{0,0,640,480}},{"scale",QString(64,'a')}}}};}
class CaptureProcessTests final:public QObject {
 Q_OBJECT
private slots:
 void cleanup(){qunsetenv("PUTGD_TEST_CAPTURE_MODE");}
 void genuinePngValidatedOnlyAfterNormalChildExit(){putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int completed=0;putgd::PayloadResult result;controller.start(snapshot(),{},[&](putgd::PayloadResult r){result=std::move(r);++completed;});QTRY_COMPARE(completed,1);QVERIFY2(!result.error,qPrintable(result.error.code));QCOMPARE(result.payload.image.size(),QSize(3,2));QCOMPARE(result.payload.image.pixelColor(1,1),QColor(Qt::blue));QVERIFY(!controller.busy());QCOMPARE(controller.processId(),qint64(0));const auto exit=controller.diagnostics()["lastExit"].toObject();QCOMPARE(exit["status"].toString(),QString("normal"));QCOMPARE(exit["code"].toInt(),0);QVERIFY(exit["processId"].toInteger()>0);QVERIFY(exit["startTicks"].toString().toULongLong()>0);}
 void trustedImageRespectsItsAcceptedBudgetWithoutChangingReaderGuard(){
  // CTest sets QT_IMAGEIO_MAXALLOC=1 before the first image reader is created.
  QCOMPARE(QImageReader::allocationLimit(),1);
  qputenv("PUTGD_TEST_CAPTURE_MODE","large");
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::PayloadResult result;
  controller.start(snapshot(),{},[&](putgd::PayloadResult value){result=std::move(value);++count;});
  QTRY_COMPARE(count,1);
  QVERIFY2(!result.error,qPrintable(result.error.code));
  QCOMPARE(result.payload.image.size(),QSize(700,700));
  QCOMPARE(result.payload.image.pixelColor(699,699),QColor(Qt::blue));
  QCOMPARE(QImageReader::allocationLimit(),1);
 }
 void immediateCancelWaitsForReadyAndExitsNormally(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::Error error;
  controller.start(snapshot(),{},[&](putgd::PayloadResult value){error=value.error;++count;});
  controller.cancel();
  QTRY_COMPARE(count,1);
  QCOMPARE(error.code,QString("cancelled"));
  const auto exit=controller.diagnostics()["lastExit"].toObject();
  QCOMPARE(exit["status"].toString(),QString("normal"));
  QCOMPARE(exit["code"].toInt(),0);
 }
 void malformedResultsSettleOnce_data(){QTest::addColumn<QByteArray>("mode");for(const char* mode:{"wrong-build","wrong-dimension","wrong-hash","duplicate","early-exit"})QTest::newRow(mode)<<QByteArray(mode);}
 void malformedResultsSettleOnce(){QFETCH(QByteArray,mode);qputenv("PUTGD_TEST_CAPTURE_MODE",mode);putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int count=0;putgd::Error error;controller.start(snapshot(),{},[&](putgd::PayloadResult r){error=r.error;++count;});QTRY_COMPARE(count,1);QVERIFY(error);QVERIFY(!controller.busy());QTest::qWait(20);QCOMPARE(count,1);}
 void cancellationOwnsOnlyOneChildAndCompletesOnce(){qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);int count=0;putgd::Error error;controller.start(snapshot(),{},[&](putgd::PayloadResult r){error=r.error;++count;});QTRY_VERIFY(controller.processId()>0);QTest::qWait(30);controller.cancel();controller.cancel();QTRY_COMPARE(count,1);QCOMPARE(error.code,QString("cancelled"));QVERIFY(!controller.busy());}
 void missingHelperCallbackCanDestroyController(){
  auto controller=std::make_unique<putgd::CaptureProcessController>("/does-not-exist-putgd-capture");
  int count=0;
  controller->start(snapshot(),{},[&](putgd::PayloadResult result){
   QVERIFY(result.error);
   ++count;
   controller.reset();
  });
  QTRY_COMPARE(count,1);
  QVERIFY(!controller);
  QTest::qWait(30);
  QCOMPARE(count,1);
 }
 void successfulCallbackCanDestroyController(){
  auto controller=std::make_unique<putgd::CaptureProcessController>(PUTGD_CAPTURE_FIXTURE);
  int count=0;
  controller->start(snapshot(),{},[&](putgd::PayloadResult result){
   QVERIFY(!result.error);
   QCOMPARE(result.payload.image.size(),QSize(3,2));
   ++count;
   controller.reset();
  });
  QTRY_COMPARE(count,1);
  QVERIFY(!controller);
  QTest::qWait(30);
  QCOMPARE(count,1);
 }
 void successfulCallbackCanStartNewGeneration(){
  putgd::CaptureProcessController controller(PUTGD_CAPTURE_FIXTURE);
  int count=0;
  controller.start(snapshot(),{},[&](putgd::PayloadResult first){
   QVERIFY(!first.error);
   ++count;
   controller.start(snapshot(),{},[&](putgd::PayloadResult second){
    QVERIFY(!second.error);
    ++count;
   });
  });
  QTRY_COMPARE(count,2);
  QVERIFY(!controller.busy());
  QCOMPARE(controller.diagnostics()["lastExit"].toObject()["generation"].toString(),QString("2"));
 }
 void destructionCancelsOutstandingCallbackExactlyOnce(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");
  auto controller=std::make_unique<putgd::CaptureProcessController>(PUTGD_CAPTURE_FIXTURE);
  int count=0;putgd::Error error;
  controller->start(snapshot(),{},[&](putgd::PayloadResult result){error=result.error;++count;});
  QTRY_VERIFY(controller->processId()>0);
  controller.reset();
  QCOMPARE(count,1);
  QCOMPARE(error.code,QString("cancelled"));
  QTest::qWait(30);
  QCOMPARE(count,1);
 }
 void helperEofCleansVerifiedArtifact(){
  qputenv("PUTGD_TEST_CAPTURE_MODE","waiting");
  QTemporaryDir directory("/tmp/putgd-capture-XXXXXX");QVERIFY(directory.isValid());
  const auto path=directory.filePath("result.png");QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));file.close();
  struct stat st{};QVERIFY(!::lstat(QFile::encodeName(path).constData(),&st));
  QProcess process;process.setProcessChannelMode(QProcess::SeparateChannels);process.setReadChannel(QProcess::StandardOutput);
  putgd::CaptureControl control(process);bool ready=false;connect(&control,&putgd::CaptureControl::frame,this,[&](const QString& type,const QJsonObject&){if(type=="ready")ready=true;});
  process.start(PUTGD_CAPTURE_FIXTURE,{});QVERIFY(process.waitForStarted());
  QVERIFY(control.send("bootstrap",{{"build",QJsonDocument::fromJson(putgd::buildManifestJson()).object()},{"nonce",QUuid::createUuid().toString(QUuid::WithoutBraces)},{"generation","1"},{"target",snapshot()},{"path",path},{"device",QString::number(qulonglong(st.st_dev))},{"inode",QString::number(qulonglong(st.st_ino))},{"limits",QJsonObject{{"imagePixels",1000},{"imageBytes",4000},{"maxFileBytes",4000}}}}));
  QTRY_VERIFY(ready);process.closeWriteChannel();QVERIFY(process.waitForFinished());QCOMPARE(process.exitStatus(),QProcess::NormalExit);QCOMPARE(process.exitCode(),3);QVERIFY(!QFile::exists(path));QVERIFY(!QDir(directory.path()).exists());
 }
 void artifactCleanupPreservesReplacementFile(){
  QTemporaryDir directory("/tmp/putgd-capture-XXXXXX");QVERIFY(directory.isValid());const auto path=directory.filePath("result.png");
  QFile original(path);QVERIFY(original.open(QIODevice::ReadWrite));QVERIFY(original.setPermissions(QFile::ReadOwner|QFile::WriteOwner));struct stat st{};QVERIFY(!::lstat(QFile::encodeName(path).constData(),&st));
  putgd::CaptureArtifactLease artifact;QVERIFY(artifact.acquire(path,QString::number(qulonglong(st.st_dev)),QString::number(qulonglong(st.st_ino))));
  QVERIFY(QFile::remove(path));QFile replacement(path);QVERIFY(replacement.open(QIODevice::WriteOnly));QVERIFY(replacement.setPermissions(QFile::ReadOwner|QFile::WriteOwner));QCOMPARE(replacement.write("replacement"),qint64(11));replacement.close();
  artifact.cleanup();QVERIFY(QFile::exists(path));QVERIFY(replacement.open(QIODevice::ReadOnly));QCOMPARE(replacement.readAll(),QByteArray("replacement"));
 }
 void artifactCleanupPreservesReplacementDirectory(){
  QTemporaryDir directory("/tmp/putgd-capture-XXXXXX");QVERIFY(directory.isValid());const auto oldPath=directory.path();const auto moved=oldPath+"-moved";const auto path=directory.filePath("result.png");
  QFile file(path);QVERIFY(file.open(QIODevice::WriteOnly));QVERIFY(file.setPermissions(QFile::ReadOwner|QFile::WriteOwner));file.close();struct stat st{};QVERIFY(!::lstat(QFile::encodeName(path).constData(),&st));
  putgd::CaptureArtifactLease artifact;QVERIFY(artifact.acquire(path,QString::number(qulonglong(st.st_dev)),QString::number(qulonglong(st.st_ino))));
  QVERIFY(QDir().rename(oldPath,moved));QVERIFY(QDir().mkdir(oldPath));QVERIFY(QFile::setPermissions(oldPath,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner));
  artifact.cleanup();QVERIFY(QDir(oldPath).exists());QVERIFY(QFile::exists(moved+"/result.png"));QVERIFY(QDir(moved).removeRecursively());
 }
 void missingHelperDoesNotRestart(){putgd::CaptureProcessController controller("/does-not-exist-putgd-capture");int count=0;controller.start(snapshot(),{},[&](putgd::PayloadResult r){QVERIFY(r.error);++count;});QTRY_COMPARE(count,1);QVERIFY(!controller.busy());QCOMPARE(controller.diagnostics()["lastExit"].toObject()["processId"].toInteger(),qint64(0));QVERIFY(controller.diagnostics()["lastExit"].toObject()["startTicks"].toString().isEmpty());QTest::qWait(30);QCOMPARE(count,1);}
};
QTEST_GUILESS_MAIN(CaptureProcessTests)
#include "test_capture_process.moc"
