#include "core/EditorJob.h"
#include "core/SanitizedPng.h"
#include <QDir>
#include <QDirIterator>
#include <QFileInfo>
#include <QImage>
#include <QImageWriter>
#include <QJsonArray>
#include <QRandomGenerator>
#include <QSemaphore>
#include <QTemporaryDir>
#include <QScopeGuard>
#include <QTest>
#include <thread>
#include <csignal>
#include <sys/resource.h>
#include <unistd.h>
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
// Direct children of this test process with the given executable name.
QList<int> childProcesses(const QByteArray& name){
 QList<int> pids;
 for(const auto& entry:QDir("/proc").entryList(QDir::Dirs|QDir::NoDotAndDotDot)){
  bool numeric=false;const int pid=entry.toInt(&numeric);if(!numeric)continue;
  QFile stat("/proc/"+entry+"/stat");if(!stat.open(QIODevice::ReadOnly))continue;
  const auto line=stat.readAll();const auto open=line.indexOf('('),close=line.lastIndexOf(')');if(open<0||close<0)continue;
  const auto fields=line.mid(close+2).split(' ');
  if(line.mid(open+1,close-open-1).startsWith(name.left(15))&&fields.size()>1&&fields[1].toInt()==int(::getpid()))pids.append(pid);
 }
 return pids;
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
  std::thread worker([&]{auto image=original;result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).withAppearance("light").run(image,"Upload to synthetic destination",64*1024*1024,session);});
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
  // editor.view drives the window's own tool, zoom and crop controls; inspect reports them.
  auto view=ask(*session,"view",{{"action","tool"},{"tool","crop"}});QVERIFY2(!view.error,qPrintable(view.error.message));
  QCOMPARE(view.result["tool"].toString(),QString("crop"));QVERIFY(view.result["cropPending"].toBool());QCOMPARE(view.result["geometry"].toArray(),QJsonArray({10,10,300,180}));
  QVERIFY(!ask(*session,"view",{{"action","cancelCrop"}}).error);
  auto zoomed=ask(*session,"view",{{"action","zoom"},{"percent",200}});QVERIFY(!zoomed.error);QCOMPARE(zoomed.result["zoomPercent"].toDouble(),200.0);
  QCOMPARE(ask(*session,"inspect").result["view"].toObject()["zoomPercent"].toDouble(),200.0);
  QCOMPARE(ask(*session,"view",{{"action","explode"}}).error.code,QString("invalid_request"));
  QCOMPARE(ask(*session,"view",{{"action","select"},{"shapeId",999}}).error.code,QString("edit_shape_missing"));
  // A failed copy names its reason and the window offers another location; the draft stays.
  auto refused=ask(*session,"saveCopy",{{"path",root.filePath("missing-folder/copy.png")}});QCOMPARE(refused.error.code,QString("save_failed"));
  QCOMPARE(ask(*session,"view").result["lastSave"].toObject()["status"].toString(),QString("failed"));
  auto preview=ask(*session,"preview",{{"maxEdge",150}});QVERIFY2(!preview.error,qPrintable(preview.error.code));
  const auto previewImage=QImage::fromData(QByteArray::fromBase64(preview.result["png"].toString().toLatin1()),"PNG");
  QCOMPARE(previewImage.width(),150);QCOMPARE(preview.result["sourceWidth"].toInt(),300);
  QCOMPARE(ask(*session,"preview",{{"maxEdge",10}}).error.code,QString("invalid_request"));
  const auto copy=root.filePath("copy.png");
  auto saved=ask(*session,"saveCopy",{{"path",copy}});QVERIFY2(!saved.error,qPrintable(saved.error.message));
  QCOMPARE(ask(*session,"view").result["lastSave"].toObject()["path"].toString(),QFileInfo(copy).absoluteFilePath());
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
 // An oversized result is recoverable: nothing is handed off, editing and
 // the resident's phase resume, inspect says why, and a smaller result uploads.
 void oversizedResultReturnsToEditing(){
  QTemporaryDir root;const auto staging=root.filePath("editing");auto session=std::make_shared<EditorSession>();EditorResult result;
  std::thread worker([&]{auto image=noise({320,200},5);result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).run(image,"Upload to synthetic destination",4096,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));
  QVERIFY(!ask(*session,"submit").error);
  Answer state;
  for(int waited=0;waited<20000;waited+=50){state=ask(*session,"inspect");if(!state.error&&state.result["preparation"].isObject())break;QTest::qSleep(50);}
  QVERIFY2(!state.error,qPrintable(state.error.code));
  const auto preparation=state.result["preparation"].toObject();
  QCOMPARE(preparation["status"].toString(),QString("failed"));QCOMPARE(preparation["code"].toString(),QString("input_excessive"));
  QVERIFY(preparation["reason"].toString().contains("limit"));
  QCOMPARE(state.result["phase"].toString(),QString("editing"));QCOMPARE(session->phase(),QString("editing"));
  const auto view=state.result["view"].toObject();QVERIFY(!view["busy"].toBool());QCOMPARE(view["preparationFailure"].toObject()["text"].toString(),preparation["reason"].toString());
  // Editing goes on: crop to something small enough, then upload it.
  QVERIFY(!ask(*session,"apply",{{"commands",QJsonArray{QJsonObject{{"op","crop"},{"rect",QJsonArray{0,0,8,8}}}}}}).error);
  QVERIFY(!ask(*session,"submit").error);
  if(worker.joinable())worker.join();
  QVERIFY2(!result.error,qPrintable(result.error.code+": "+result.error.message));
  QCOMPARE(QSize(result.width,result.height),QSize(8,8));QVERIFY(!inspectSanitizedPng(result.png));QCOMPARE(filesUnder(staging),0);
 }
 void agentCancelDiscardsWithoutOutput(){
  QTemporaryDir root;const auto staging=root.filePath("editing");auto session=std::make_shared<EditorSession>();EditorResult result;
  // Recent custom colors travel to the helper and back even when nothing is uploaded.
  std::thread worker([&]{auto image=noise({64,64},2);result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).withRecentColors({"#123456","#abcdef"}).run(image,"Save to the local archive",1024*1024,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));
  QVERIFY(!ask(*session,"apply",{{"commands",QJsonArray{QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{1,1,5,5}}}}}}}}).error);
  QVERIFY(!ask(*session,"cancel").error);if(worker.joinable())worker.join();
  QCOMPARE(result.error.code,QString("cancelled"));QVERIFY(result.png.isEmpty());QCOMPARE(filesUnder(staging),0);
  QVERIFY(result.recentColors);QCOMPARE(*result.recentColors,QStringList({"#123456","#abcdef"}));
 }
 void helperCrashWhileEditingFailsClosed(){
  QTemporaryDir root;const auto staging=root.filePath("editing");auto session=std::make_shared<EditorSession>();EditorResult result;
  std::thread worker([&]{auto image=noise({64,64},4);result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).run(image,"Upload",1024*1024,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));
  const auto helpers=childProcesses("putgd-ui");QCOMPARE(helpers.size(),1);QCOMPARE(::kill(helpers.first(),SIGKILL),0);
  if(worker.joinable())worker.join();
  QCOMPARE(result.error.code,QString("editor_exit"));QVERIFY(result.png.isEmpty());QCOMPARE(filesUnder(staging),0);QCOMPARE(session->phase(),QString("closed"));
 }
 void misbehavingHelpersNeverHandOffAnImage_data(){
  QTest::addColumn<QString>("mode");QTest::addColumn<QString>("code");
  QTest::newRow("never ready (startup timeout)")<<"silent"<<"editor_timeout";
  QTest::newRow("crash while editing")<<"crash"<<"editor_exit";
  QTest::newRow("hang while finalizing (finalize timeout)")<<"hang-finalizing"<<"editor_timeout";
  QTest::newRow("output is not a PNG")<<"bad-png"<<"edit_output_invalid";
  QTest::newRow("output digest mismatch")<<"wrong-digest"<<"editor_io";
 }
 void misbehavingHelpersNeverHandOffAnImage(){
  QFETCH(QString,mode);QFETCH(QString,code);
  qputenv("PUTGD_TEST_EDITOR_MODE",mode.toUtf8());auto restore=qScopeGuard([]{qunsetenv("PUTGD_TEST_EDITOR_MODE");});
  QTemporaryDir root;const auto staging=root.filePath("editing");auto session=std::make_shared<EditorSession>();
  auto image=noise({32,32},6);
  const auto result=EditorJob(staging,PUTGD_EDITOR_FIXTURE,800,800,800).run(image,"Upload",1024*1024,session);
  QCOMPARE(result.error.code,code);QVERIFY(result.png.isEmpty());QCOMPARE(filesUnder(staging),0);QCOMPARE(session->phase(),QString("closed"));
  QVERIFY(childProcesses("test_editor_hel").isEmpty());
 }
 void fullDiskWhileStagingIsReportedAsStorage(){
  // A file-size limit stands in for a full disk: staging writes fail with EFBIG.
  QTemporaryDir root;const auto staging=root.filePath("editing");auto session=std::make_shared<EditorSession>();
  auto image=noise({256,256},7);
  rlimit previous{};QCOMPARE(::getrlimit(RLIMIT_FSIZE,&previous),0);const auto handler=std::signal(SIGXFSZ,SIG_IGN);
  rlimit small=previous;small.rlim_cur=16*1024;QCOMPARE(::setrlimit(RLIMIT_FSIZE,&small),0);
  const auto result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).run(image,"Upload",1024*1024,session);
  QCOMPARE(::setrlimit(RLIMIT_FSIZE,&previous),0);std::signal(SIGXFSZ,handler);
  QCOMPARE(result.error.code,QString("edit_staging_failed"));QVERIFY(result.png.isEmpty());QCOMPARE(filesUnder(staging),0);
  QVERIFY(childProcesses("putgd-ui").isEmpty());
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
 // Over the budget the edit stays open (oversizedResultReturnsToEditing);
 // discarding it then still hands nothing off.
 void exportLargerThanBudgetKeepsEditingUntilDiscarded(){
  QTemporaryDir root;auto session=std::make_shared<EditorSession>();EditorResult result;
  std::thread worker([&]{auto image=noise({200,200},4);result=EditorJob(root.filePath("editing"),PUTGD_PRODUCT_UI_PATH).run(image,"Upload",1000,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));QVERIFY(!ask(*session,"submit").error);
  bool recovered=false;for(int waited=0;waited<20000&&!recovered;waited+=50){const auto state=ask(*session,"inspect");recovered=!state.error&&state.result["preparation"].isObject();if(!recovered)QTest::qSleep(50);}
  QVERIFY(recovered);QVERIFY(!ask(*session,"cancel").error);if(worker.joinable())worker.join();
  QCOMPARE(result.error.code,QString("cancelled"));QVERIFY(result.png.isEmpty());QCOMPARE(filesUnder(root.filePath("editing")),0);
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
