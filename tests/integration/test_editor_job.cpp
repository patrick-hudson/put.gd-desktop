#include "core/EditorJob.h"
#include "core/SanitizedPng.h"
#include <QDir>
#include <QDirIterator>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QTest>
#include <thread>
using namespace putgd;

namespace {
QImage noise(QSize size,quint32 seed){
 QImage image(size,QImage::Format_RGB32);QRandomGenerator random(seed);
 for(int y=0;y<size.height();++y){auto* line=reinterpret_cast<QRgb*>(image.scanLine(y));for(int x=0;x<size.width();++x)line[x]=random.generate()|0xff000000U;}
 return image;
}
struct Answer {Error error;QJsonObject result;};
// Commands are queued from this thread while the job worker supervises the helper.
Answer ask(EditorSession& session,const QString& command,const QJsonObject& params={}){
 QSemaphore done;Answer answer;
 session.request(command,params,[&](Error e,QJsonObject r){answer={e,r};done.release();});
 if(!done.tryAcquire(1,15000))answer.error={"test_timeout","No editor answer",{}};
 return answer;
}
bool waitPhase(EditorSession& session,const QString& phase,int ms=20000){
 for(int waited=0;waited<ms;waited+=20){if(session.phase()==phase)return true;QTest::qSleep(20);}
 return false;
}
int filesUnder(const QString& root){int count=0;QDirIterator it(root,QDir::Files|QDir::Hidden,QDirIterator::Subdirectories);while(it.hasNext()){it.next();++count;}return count;}
}

class EditorJobTest:public QObject {
 Q_OBJECT
private slots:
 void agentEditsPreviewSaveAndSubmitSanitizedResult(){
  QTemporaryDir root;const auto staging=root.filePath("editing");
  const auto original=noise({320,200},21);const QRect secret(40,30,80,40);
  auto session=std::make_shared<EditorSession>();EditorResult result;
  std::thread worker([&]{auto image=original;result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).run(image,"Upload to synthetic destination",64*1024*1024,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));
  auto state=ask(*session,"inspect");QVERIFY2(!state.error,qPrintable(state.error.code));
  QCOMPARE(state.result["width"].toInt(),320);QCOMPARE(state.result["phase"].toString(),QString("editing"));QCOMPARE(state.result["destination"].toString(),QString("Upload to synthetic destination"));
  const QJsonArray commands{
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{secret.x(),secret.y(),secret.width(),secret.height()}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","blur"},{"rect",QJsonArray{150,30,60,60}},{"strength",24}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","arrow"},{"from",QJsonArray{300,190}},{"to",QJsonArray{220,120}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","text"},{"at",QJsonArray{10,170}},{"text","synthetic"}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","highlight"},{"rect",QJsonArray{230,10,60,20}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","rectangle"},{"rect",QJsonArray{20,20,120,60}}}}},
   QJsonObject{{"op","crop"},{"rect",QJsonArray{10,10,300,180}}}};
  auto applied=ask(*session,"apply",{{"commands",commands}});QVERIFY2(!applied.error,qPrintable(applied.error.message));
  QCOMPARE(applied.result["created"].toArray().size(),6);QCOMPARE(applied.result["state"].toObject()["shapes"].toArray().size(),6);
  auto bad=ask(*session,"apply",{{"commands",QJsonArray{QJsonObject{{"op","remove"},{"shapeId",999}}}}});QCOMPARE(bad.error.code,QString("edit_shape_missing"));
  QCOMPARE(ask(*session,"inspect").result["shapes"].toArray().size(),6);
  auto undo=ask(*session,"apply",{{"commands",QJsonArray{QJsonObject{{"op","undo"}},QJsonObject{{"op","redo"}}}}});QVERIFY(!undo.error);
  auto preview=ask(*session,"preview",{{"maxEdge",150}});QVERIFY2(!preview.error,qPrintable(preview.error.code));
  const auto previewImage=QImage::fromData(QByteArray::fromBase64(preview.result["png"].toString().toLatin1()),"PNG");
  QCOMPARE(previewImage.width(),150);QCOMPARE(preview.result["sourceWidth"].toInt(),300);
  QCOMPARE(ask(*session,"preview",{{"maxEdge",10}}).error.code,QString("invalid_request"));
  const auto copy=root.filePath("copy.png");
  auto saved=ask(*session,"saveCopy",{{"path",copy}});QVERIFY2(!saved.error,qPrintable(saved.error.message));
  QCOMPARE(ask(*session,"saveCopy",{{"path",copy}}).error.code,QString("file_exists"));
  QCOMPARE(ask(*session,"saveCopy",{{"path","relative.png"}}).error.code,QString("invalid_request"));
  QCOMPARE(ask(*session,"shell").error.code,QString("invalid_request"));
  QVERIFY(!ask(*session,"raise").error);
  QVERIFY(!ask(*session,"submit").error);
  if(worker.joinable())worker.join();
  QVERIFY2(!result.error,qPrintable(result.error.code+": "+result.error.message));
  QVERIFY(!inspectSanitizedPng(result.png));QCOMPARE(QSize(result.width,result.height),QSize(300,180));
  const auto uploaded=QImage::fromData(result.png,"PNG").convertToFormat(QImage::Format_RGB32);
  // Crop moved the origin by (10,10); every covered source pixel is black.
  for(int y=secret.top();y<=secret.bottom();++y)for(int x=secret.left();x<=secret.right();++x)QCOMPARE(uploaded.pixel(x-10,y-10),qRgb(0,0,0));
  QCOMPARE(uploaded.pixel(0,0),original.pixel(10,10));
  QFile copyFile(copy);QVERIFY(copyFile.open(QIODevice::ReadOnly));QCOMPARE(copyFile.readAll(),result.png);
  QCOMPARE(filesUnder(staging),0);QCOMPARE(session->phase(),QString("closed"));
  QCOMPARE(ask(*session,"inspect").error.code,QString("editor_finalizing"));
 }
 void agentCancelDiscardsWithoutOutput(){
  QTemporaryDir root;const auto staging=root.filePath("editing");auto session=std::make_shared<EditorSession>();EditorResult result;
  std::thread worker([&]{auto image=noise({64,64},2);result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).run(image,"Save to the local archive",1024*1024,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));
  QVERIFY(!ask(*session,"apply",{{"commands",QJsonArray{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{1,1,5,5}}}}}}}}).error);
  QVERIFY(!ask(*session,"cancel").error);if(worker.joinable())worker.join();
  QCOMPARE(result.error.code,QString("cancelled"));QVERIFY(result.png.isEmpty());QCOMPARE(filesUnder(staging),0);
 }
 void residentCancellationTerminatesTheHelper(){
  QTemporaryDir root;const auto staging=root.filePath("editing");auto session=std::make_shared<EditorSession>();EditorResult result;
  std::thread worker([&]{auto image=noise({64,64},3);result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).run(image,"Upload",1024*1024,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));session->cancel();if(worker.joinable())worker.join();
  QCOMPARE(result.error.code,QString("cancelled"));QCOMPARE(filesUnder(staging),0);
 }
 void oversizedInputIsRejectedBeforeStaging(){
  QTemporaryDir root;auto session=std::make_shared<EditorSession>();QImage image(6000,4001,QImage::Format_RGB32);
  const auto result=EditorJob(root.filePath("editing"),PUTGD_PRODUCT_UI_PATH).run(image,"Upload",1024*1024,session);
  QCOMPARE(result.error.code,QString("edit_input_too_large"));QVERIFY(image.isNull());QVERIFY(!QDir(root.filePath("editing")).exists());
 }
 void exportLargerThanBudgetFails(){
  QTemporaryDir root;auto session=std::make_shared<EditorSession>();EditorResult result;
  std::thread worker([&]{auto image=noise({200,200},4);result=EditorJob(root.filePath("editing"),PUTGD_PRODUCT_UI_PATH).run(image,"Upload",1000,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));QVERIFY(!ask(*session,"submit").error);if(worker.joinable())worker.join();
  QCOMPARE(result.error.code,QString("edit_input_too_large"));QVERIFY(result.png.isEmpty());
 }
 void fileSourceIsDecodedOnlyByTheHelper(){
  // A JPEG carrying an EXIF 90-degree rotation opens upright, decoded in the helper.
  QTemporaryDir root;const auto jpeg=root.filePath("rotated.jpg");
  {QImage image(40,20,QImage::Format_RGB32);image.fill(Qt::red);QImageWriter writer(jpeg,"jpeg");writer.setTransformation(QImageIOHandler::TransformationRotate90);QVERIFY(writer.write(image));}
  auto session=std::make_shared<EditorSession>();EditorResult result;
  std::thread worker([&]{result=EditorJob(root.filePath("editing"),PUTGD_PRODUCT_UI_PATH).runFile(jpeg,1024*1024,"Upload",1024*1024,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));
  const auto state=ask(*session,"inspect");QVERIFY2(!state.error,qPrintable(state.error.code));
  QCOMPARE(QSize(state.result["width"].toInt(),state.result["height"].toInt()),QSize(20,40));
  QVERIFY(!ask(*session,"submit").error);if(worker.joinable())worker.join();
  QVERIFY2(!result.error,qPrintable(result.error.code));QCOMPARE(QSize(result.width,result.height),QSize(20,40));QVERIFY(!inspectSanitizedPng(result.png));
  // Bytes that are not a static PNG/JPEG are refused by the helper; nothing is returned.
  const auto text=root.filePath("note.png");QFile file(text);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic, not an image");file.close();
  auto second=std::make_shared<EditorSession>();
  const auto refused=EditorJob(root.filePath("editing"),PUTGD_PRODUCT_UI_PATH).runFile(text,1024*1024,"Upload",1024*1024,second);
  QCOMPARE(refused.error.code,QString("source_unsupported"));QVERIFY(refused.png.isEmpty());
  auto third=std::make_shared<EditorSession>();
  QCOMPARE(EditorJob(root.filePath("editing"),PUTGD_PRODUCT_UI_PATH).runFile(jpeg,16,"Upload",1024*1024,third).error.code,QString("source_unsupported"));
  QCOMPARE(filesUnder(root.filePath("editing")),0);
 }
 void missingHelperFailsClosed(){
  QTemporaryDir root;auto session=std::make_shared<EditorSession>();auto image=noise({20,20},5);
  const auto result=EditorJob(root.filePath("editing"),root.filePath("absent-helper")).run(image,"Upload",1024*1024,session);
  QCOMPARE(result.error.code,QString("editor_start"));QCOMPARE(filesUnder(root.filePath("editing")),0);
  QCOMPARE(ask(*session,"inspect").error.code,QString("editor_finalizing"));
 }
 void abandonedStagingIsDiscarded(){
  QTemporaryDir root;const auto staging=root.filePath("editing");QVERIFY(QDir().mkpath(staging+"/pge-abcdef"));
  QFile left(staging+"/pge-abcdef/input.png");QVERIFY(left.open(QIODevice::WriteOnly));left.write("synthetic");left.close();
  EditorJob::discardStaging(staging);QVERIFY(!QDir(staging).exists());
 }
};
QTEST_GUILESS_MAIN(EditorJobTest)
#include "test_editor_job.moc"
