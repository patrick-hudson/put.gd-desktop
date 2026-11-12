#include "core/EditorJob.h"
#include "core/SanitizedPng.h"
#include <QDir>
#include <QCryptographicHash>
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
#include <mutex>
#include <atomic>
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
  // View-only work must not change another copy or the bytes handed to Upload.
  QVERIFY(!ask(*session,"view",{{"action","zoom"},{"percent",125}}).error);
  QVERIFY(!ask(*session,"preview",{{"maxEdge",128}}).error);
  const auto repeated=ask(*session,"saveCopy",{{"path",root.filePath("copy-after-preview.png")}});
  QVERIFY2(!repeated.error,qPrintable(repeated.error.message));QCOMPARE(repeated.result["sha256"],saved.result["sha256"]);
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

 // #1362: Copy image hands the resident the same sanitized PNG as Save a copy
 // and Upload; the helper's staged copy never stays behind.
 void copyImageHandsTheSanitizedPngToTheClipboardOnly(){
  QTemporaryDir root;const auto staging=root.filePath("editing");const QRect secret(40,30,80,40);
  auto session=std::make_shared<EditorSession>();EditorResult result;
  std::mutex mutex;QList<QByteArray> clipboard;Error refuse;
  session->setCopyHandler([&](QByteArray png,std::function<void(Error)> done){Error answer;{std::lock_guard lock(mutex);clipboard.append(png);answer=refuse;}done(answer);});
  std::thread worker([&]{auto image=noise({320,200},53);result=EditorJob(staging,PUTGD_PRODUCT_UI_PATH).run(image,"Upload to synthetic destination",64*1024*1024,session);});
  auto joined=qScopeGuard([&]{session->cancel();if(worker.joinable())worker.join();});
  QVERIFY(waitPhase(*session,"editing"));
  const QJsonArray commands{
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","step"},{"at",QJsonArray{200,150}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","pen"},{"points",QJsonArray{QJsonArray{20,150},QJsonArray{80,120},QJsonArray{140,160}}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","ellipse"},{"rect",QJsonArray{160,40,100,60}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","line"},{"from",QJsonArray{10,190}},{"to",QJsonArray{300,190}}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","blur"},{"rect",QJsonArray{180,110,100,40}},{"style","pixelate"},{"strength",10}}}},
   QJsonObject{{"op","add"},{"shape",QJsonObject{{"kind","redact"},{"rect",QJsonArray{secret.x(),secret.y(),secret.width(),secret.height()}}}}}};
  auto applied=ask(*session,"apply",{{"commands",commands}});QVERIFY2(!applied.error,qPrintable(applied.error.message));
  QCOMPARE(applied.result["state"].toObject()["shapes"].toArray()[0].toObject()["number"].toInt(),1);
  QCOMPARE(ask(*session,"copy",{{"format","png"}}).error.code,QString("invalid_request"));
  auto copied=ask(*session,"copy");QVERIFY2(!copied.error,qPrintable(copied.error.code+": "+copied.error.message));
  QVERIFY(copied.result["copied"].toBool());QCOMPARE(copied.result["width"].toInt(),320);
  QByteArray png;{std::lock_guard lock(mutex);QCOMPARE(clipboard.size(),1);png=clipboard.first();}
  QVERIFY(!inspectSanitizedPng(png));QCOMPARE(copied.result["bytes"].toDouble(),double(png.size()));
  QCOMPARE(copied.result["sha256"].toString(),QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex()));
  const auto pasted=QImage::fromData(png,"PNG").convertToFormat(QImage::Format_RGB32);
  for(int y=secret.top();y<=secret.bottom();++y)for(int x=secret.left();x<=secret.right();++x)QCOMPARE(pasted.pixel(x,y),qRgb(0,0,0));
  // Nothing of the copy stays in private staging: only the input and the empty output.
  QCOMPARE(filesUnder(staging),2);
  auto view=ask(*session,"view");QCOMPARE(view.result["lastCopy"].toObject()["status"].toString(),QString("copied"));QVERIFY(!view.result["copying"].toBool());
  // The same bytes as Save a copy.
  auto saved=ask(*session,"saveCopy",{{"path",root.filePath("saved.png")}});QVERIFY2(!saved.error,qPrintable(saved.error.message));QCOMPARE(saved.result["sha256"],copied.result["sha256"]);
  // A clipboard refusal reaches the agent and the window; editing goes on.
  {std::lock_guard lock(mutex);refuse={"clipboard_unavailable","No clipboard owner",{}};}
  auto failed=ask(*session,"copy");QCOMPARE(failed.error.code,QString("clipboard_unavailable"));
  view=ask(*session,"view");QCOMPARE(view.result["lastCopy"].toObject()["status"].toString(),QString("failed"));QCOMPARE(view.result["message"].toObject()["tone"].toString(),QString("error"));
  QCOMPARE(filesUnder(staging),2);QCOMPARE(session->phase(),QString("editing"));
  // Upload hands off exactly the copied PNG.
  QVERIFY(!ask(*session,"submit").error);worker.join();
  QVERIFY2(!result.error,qPrintable(result.error.code));QCOMPARE(result.png,png);
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
 // An untrusted helper's bad output never becomes the image. Only the timeout row has
 // short deadlines; the others get ones a slow machine cannot reach first.
 void misbehavingHelpersNeverHandOffAnImage_data(){
  QTest::addColumn<QString>("mode");QTest::addColumn<QString>("code");QTest::addColumn<int>("deadlineMs");
  QTest::newRow("crash while editing")<<"crash"<<"editor_exit"<<60000;
  QTest::newRow("hang while finalizing (finalize timeout)")<<"hang-finalizing"<<"editor_timeout"<<800;
  QTest::newRow("output is not a PNG")<<"bad-png"<<"edit_output_invalid"<<60000;
  QTest::newRow("output digest mismatch")<<"wrong-digest"<<"editor_io"<<60000;
 }
 void misbehavingHelpersNeverHandOffAnImage(){
  QFETCH(QString,mode);QFETCH(QString,code);QFETCH(int,deadlineMs);
  qputenv("PUTGD_TEST_EDITOR_MODE",mode.toUtf8());auto restore=qScopeGuard([]{qunsetenv("PUTGD_TEST_EDITOR_MODE");});
  QTemporaryDir root;const auto staging=root.filePath("editing");auto session=std::make_shared<EditorSession>();
  auto image=noise({32,32},6);
  const auto result=EditorJob(staging,PUTGD_EDITOR_FIXTURE,deadlineMs,deadlineMs,deadlineMs).run(image,"Upload",1024*1024,session);
  QCOMPARE(result.error.code,code);QVERIFY(result.png.isEmpty());QCOMPARE(filesUnder(staging),0);QCOMPARE(session->phase(),QString("closed"));
  QVERIFY(childProcesses("test_editor_hel").isEmpty());
 }
};
QTEST_GUILESS_MAIN(EditorJobTest)
#include "test_editor_job.moc"
