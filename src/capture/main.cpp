#include "platform/x11/X11SourceAdapter.h"
#include "platform/x11/X11NativeSnapshot.h"
#include "platform/x11/CaptureControl.h"
#include "platform/x11/CaptureCursorPolicy.h"
#include <QLibrary>
#include "platform/x11/CaptureArtifactLease.h"
#include "ui/StdioDevice.h"
#include "core/BuildInfo.h"
#include <QApplication>
#include <QWindow>
#include <QScreen>
#include <QSurfaceFormat>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QTimer>
#include <QThread>
#include <QFile>
#include <QCryptographicHash>
#include <QtEndian>
#include <QtGui/qguiapplication_platform.h>
#include "platform/x11/NativeRegionOverlay.h"
#include <QJsonArray>
#include <QUuid>
#include <memory>
#include <optional>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <X11/Xcursor/Xcursor.h>
#undef None
#undef CursorShape
#undef Bool
#undef Status
namespace {
// PNG writer may fail before its output exceeds the accepted artifact budget.
class BoundedFile final:public QFile {
public:
 explicit BoundedFile(qint64 bound):bound_(bound){}
 bool exceeded()const{return exceeded_;}
protected:
 qint64 writeData(const char* bytes,qint64 count)override{
  if(count<0||pos()>bound_||count>bound_-pos()){exceeded_=true;return -1;}
  return QFile::writeData(bytes,count);
 }
private:qint64 bound_;bool exceeded_=false;
};
bool identity(const struct stat& s,const QString& device,const QString& inode){return S_ISREG(s.st_mode)&&s.st_uid==getuid()&&(s.st_mode&0777)==0600&&s.st_nlink==1&&QString::number(qulonglong(s.st_dev))==device&&QString::number(qulonglong(s.st_ino))==inode;}
bool positive(const QJsonValue& v,qint64 maximum){return v.isDouble()&&v.toInteger()>0&&v.toInteger()<=maximum&&v.toDouble()==v.toInteger();}
QJsonObject nativeCursorSeed(){
#if QT_CONFIG(xcb)
 if(!putgd::captureCursorCacheCompatible())return {};
 auto* native=qGuiApp->nativeInterface<QNativeInterface::QX11Application>();
 auto* display=native?native->display():nullptr;
 if(!display||!XcursorSupportsARGB(display))return {};
 // The display-based Qt loader may resize images, unlike LibraryLoadImages.
 // The shared policy already declines explicit environment resize requests.
 // Resolve this public 1.2.3 API at runtime so old distro headers still build
 // and an ABI-compatible library upgrade cannot bypass the resize check.
 static QLibrary cursorLibrary(QStringLiteral("Xcursor"),1);
 if(!cursorLibrary.isLoaded()&&!cursorLibrary.load())return {};
 using GetResizable=XcursorBool(*)(Display*);
 const auto getResizable=reinterpret_cast<GetResizable>(cursorLibrary.resolve("XcursorGetResizable"));
 // Official 1.2.1/1.2.2 lack both resizing and this API. A loaded SONAME1 with
 // no symbol retains that pre-resizing ABI; load failure never admits a seed.
 if(getResizable&&getResizable(display))return {};
 // This is the same first "cross" lookup Qt uses. If it cannot produce one
 // small static ARGB image, preserve Qt's full theme/core/animation fallback.
 std::unique_ptr<XcursorImages,decltype(&XcursorImagesDestroy)> images(XcursorLibraryLoadImages("cross",XcursorGetTheme(display),XcursorGetDefaultSize(display)),&XcursorImagesDestroy);
 if(!images||images->nimage!=1||!images->images||!images->images[0])return {};
 const auto* image=images->images[0];
 if(!image->pixels||image->width<1||image->height<1||image->width>64||image->height>64||image->xhot>=image->width||image->yhot>=image->height)return {};
 QByteArray pixels(qsizetype(image->width)*image->height*4,Qt::Uninitialized);
 for(quint32 i=0;i<image->width*image->height;++i)qToBigEndian<quint32>(image->pixels[i],pixels.data()+i*4);
 return {{"width",int(image->width)},{"height",int(image->height)},{"xhot",int(image->xhot)},{"yhot",int(image->yhot)},{"pixels",QString::fromLatin1(pixels.toBase64())}};
#else
 return {};
#endif
}
}
int main(int argc,char** argv){
 if(argc==2&&(std::strcmp(argv[1],"--version")==0||std::strcmp(argv[1],"-v")==0)){std::printf("putgd-capture %s\n",putgd::buildVersion());return 0;}
 putgd::UiControlPipes handles;if(!handles.valid())return 2;
 // This text-free X11 helper paints raster overlays only. Avoid initializing
 // GTK/style plugins on every screenshot; the separate
 // settings/history process keeps its native fonts and dialogs. Leave cursor,
 // display identity and all DPI/scaling settings unchanged.
 QGuiApplication::setDesktopSettingsAware(false);
 qunsetenv("QT_QPA_PLATFORMTHEME");
 qunsetenv("QT_STYLE_OVERRIDE");
 QApplication app(argc,argv);app.setQuitOnLastWindowClosed(false);QCoreApplication::setOrganizationName("putgd");QCoreApplication::setApplicationName("putgd-capture");QCoreApplication::setApplicationVersion(putgd::buildVersion());
 putgd::StdioDevice pipes(handles.input(),handles.output());if(!pipes.isOpen())return 2;putgd::CaptureControl control(pipes);
 putgd::X11NativeSnapshot native;putgd::X11Capture resolver;putgd::X11SourceAdapter source(nullptr,true);
 const auto build=QJsonDocument::fromJson(putgd::buildManifestJson()).object();QString nonce,generation,path,device,inode;QJsonObject frozen;putgd::ResourceLimits limits;
 putgd::CaptureArtifactLease artifact;putgd::X11Target target;
 quint32 preparedCursor=0;bool cacheCursor=false;
 bool bootstrapped=false,ready=false,begun=false,settled=false,resultSent=false,cancelled=false;
 bool previewed=false,selectionStarted=false,acquisitionQueued=false,acquisitionStarted=false;quint32 previewCursor=0;
 std::optional<putgd::RegionSelectionResult> selection;QThread* encoder=nullptr;
 QTimer deadline;deadline.setSingleShot(true);deadline.start(10000);
 const auto envelope=[&]{return QJsonObject{{"nonce",nonce},{"generation",generation}};};
 const auto finish=[&](const QString& code){if(settled)return;settled=true;deadline.stop();auto message=envelope();message["code"]=code;control.send(code=="cancelled"?"cancelled":"error",message);QTimer::singleShot(0,&app,&QCoreApplication::quit);};
 const auto rejectProtocol=[&]{
  // A fatal frame must stop the decoder immediately: another frame may already
  // be buffered in the same read, before the event loop observes app.exit().
  cancelled=true;settled=true;selection.reset();deadline.stop();control.stop();source.cancelAcquisition();app.exit(4);
 };
 QObject::connect(&deadline,&QTimer::timeout,&app,[&]{if(ready){selection.reset();finish("capture_timeout");source.cancelAcquisition();}else app.exit(3);});
 QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,[&]{cancelled=true;settled=true;selection.reset();deadline.stop();control.stop();source.cancelAcquisition();app.exit(3);});
 QObject::connect(&control,&putgd::CaptureControl::failed,&app,[&]{cancelled=true;settled=true;selection.reset();deadline.stop();control.stop();source.cancelAcquisition();app.exit(3);});
 // Selection may finish before durable BEGIN. Retain geometry only until both
 // gates are satisfied; this is the sole route to pixels and the PNG encoder.
 const auto acquireWhenAuthorized=[&]{
  if(!begun||settled||cancelled||acquisitionStarted||acquisitionQueued||(target.action=="region"&&!selection))return;
  acquisitionQueued=true;
  // Drain the complete buffered command batch before reading pixels. A BEGIN
  // followed by CANCEL/EOF/protocol failure must invalidate this queued join.
  QTimer::singleShot(0,&app,[&]{
  acquisitionQueued=false;
  if(!begun||settled||cancelled||acquisitionStarted||(target.action=="region"&&!selection))return;
  putgd::Error changed;if(!native.unchanged(frozen,changed)){finish(changed.code);return;}
  acquisitionStarted=true;
  auto acquired=[&](putgd::PayloadResult result){
    if(settled||cancelled)return;
    if(result.error){finish(result.error.code);return;}
    putgd::Error changed;if(!native.unchanged(frozen,changed)){finish(changed.code);return;}
    struct Encoded {QString hash; qint64 bytes=0; int width=0,height=0;putgd::Error error;};auto encoded=std::make_shared<Encoded>();
    const auto output=path,dev=device,ino=inode;const auto acceptedLimits=limits;
    encoder=QThread::create([encoded,image=std::move(result.payload.image),output,dev,ino,acceptedLimits]()mutable{
     auto fail=[&](const char* code){encoded->error={QString::fromLatin1(code),"Capture encoding failed",{}};};
     const qint64 pixels=qint64(image.width())*image.height();if(image.isNull()||pixels>acceptedLimits.imagePixels||image.sizeInBytes()>acceptedLimits.imageBytes){fail("input_excessive");return;}encoded->width=image.width();encoded->height=image.height();
     const int fd=::open(QFile::encodeName(output).constData(),O_RDWR|O_CLOEXEC|O_NOFOLLOW);struct stat s{};if(fd<0||::fstat(fd,&s)||!identity(s,dev,ino)||s.st_size!=0){if(fd>=0)::close(fd);fail("capture_io");return;}
     BoundedFile file(std::min(acceptedLimits.maxFileBytes,pixels*4+pixels*4/1000+encoded->height+65536));if(!file.open(fd,QIODevice::ReadWrite,QFileDevice::AutoCloseHandle)){::close(fd);fail("capture_io");return;}
     if(!image.save(&file,"PNG")||!file.flush()){fail(file.exceeded()?"input_excessive":"encoding_failed");return;}image={};encoded->bytes=file.size();if(encoded->bytes<45||encoded->bytes>acceptedLimits.maxFileBytes||encoded->bytes>pixels*4+pixels*4/1000+encoded->height+65536){fail("input_excessive");return;}file.seek(0);QCryptographicHash hash(QCryptographicHash::Sha256);if(!hash.addData(&file)){fail("capture_io");return;}encoded->hash=QString::fromLatin1(hash.result().toHex());
    });
    auto* worker=encoder;QObject::connect(worker,&QThread::finished,&app,[&,worker,encoded]{encoder=nullptr;worker->deleteLater();if(settled||cancelled)return;if(encoded->error){finish(encoded->error.code);return;}putgd::Error changed;if(!native.unchanged(frozen,changed)){finish(changed.code);return;}resultSent=true;auto reply=envelope();reply["bytes"]=QString::number(encoded->bytes);reply["sha256"]=encoded->hash;reply["width"]=encoded->width;reply["height"]=encoded->height;control.send("result",reply);deadline.start(10000);});worker->start();
   };
  if(target.action=="region"){
   const auto selected=*selection;selection.reset();
   source.acquireSelectedRegionFrozen(target,selected,limits,std::move(acquired));
  }else source.acquireFrozen(target,limits,std::move(acquired));
  });
 };
 const auto startSelection=[&]{
  if(selectionStarted||settled||cancelled)return;
  selectionStarted=true;
  source.selectRegionFrozen(target,limits,[&](putgd::RegionSelectionResult selected){
   if(settled||cancelled)return;
   if(selected.error){finish(selected.error.code);return;}
   selection=std::move(selected);
   // A completed early selection owns no input or surfaces. Bound how long its
   // geometry may await durable authorization; no capture is implicit at expiry.
   if(!begun)deadline.start(10000);
   acquireWhenAuthorized();
  });
 };
 QObject::connect(&control,&putgd::CaptureControl::frame,&app,[&](const QString& type,const QJsonObject& payload){
  if(type=="bootstrap"){
   if(bootstrapped||payload.size()!=9||payload["build"].toObject()!=build||!payload["nonce"].isString()||QUuid(payload["nonce"].toString()).isNull()||!payload["generation"].isString()||!payload["path"].isString()||!payload["device"].isString()||!payload["inode"].isString()||!payload["limits"].isObject()){rejectProtocol();return;}
   nonce=payload["nonce"].toString();generation=payload["generation"].toString();bool ok=false;const auto g=generation.toULongLong(&ok);if(!ok||!g||QString::number(g)!=generation){rejectProtocol();return;}
   path=payload["path"].toString();device=payload["device"].toString();inode=payload["inode"].toString();frozen=payload["target"].toObject();const auto l=payload["limits"].toObject();
   if(l.size()!=3||!positive(l["imagePixels"],1000000000LL)||!positive(l["imageBytes"],1099511627776LL)||!positive(l["maxFileBytes"],1099511627776LL)||!path.startsWith("/tmp/putgd-capture-")||!path.endsWith("/result.png")||path.contains("/../")||path.count('/')!=3){rejectProtocol();return;}
   limits.imagePixels=l["imagePixels"].toInteger();limits.imageBytes=l["imageBytes"].toInteger();limits.maxFileBytes=l["maxFileBytes"].toInteger();
   const auto parent=QFile::encodeName(path.left(path.lastIndexOf('/')));struct stat directory{};if(::lstat(parent.constData(),&directory)||!S_ISDIR(directory.st_mode)||directory.st_uid!=getuid()||(directory.st_mode&0777)!=0700){rejectProtocol();return;}
   const int fd=::open(QFile::encodeName(path).constData(),O_WRONLY|O_CLOEXEC|O_NOFOLLOW);struct stat s{};const bool valid=fd>=0&&!::fstat(fd,&s)&&identity(s,device,inode)&&s.st_size==0;if(fd>=0)::close(fd);if(!valid||!artifact.acquire(path,device,inode)){rejectProtocol();return;}
   const auto cursor=payload["cursor"].toObject();bool cursorOk=false;const auto cursorId=cursor["id"].toString().toULongLong(&cursorOk);
   if(cursor.size()!=2||!cursor["id"].isString()||!cursor["cache"].isBool()||!cursorOk||cursorId>UINT32_MAX||QString::number(cursorId)!=cursor["id"].toString()||(cursorId&&(!cursor["cache"].toBool()||frozen["action"]!="region"))){rejectProtocol();return;}
   preparedCursor=quint32(cursorId);cacheCursor=cursor["cache"].toBool();
   bootstrapped=true;putgd::Error error;target=native.resolve(frozen,resolver,error);
   QJsonObject seed;
   if(!error&&target.action=="region"&&!preparedCursor&&cacheCursor&&putgd::NativeRegionOverlay::supportsLayout(target.layout))seed=nativeCursorSeed();
   if(!error&&target.action=="region"&&!preparedCursor&&seed.isEmpty()){
    // Populate Qt's native themed-cursor cache without mapping a window,
    // acquiring input or reading desktop pixels. This can overlap journal IO.
    QWindow cursor;cursor.setSurfaceType(QSurface::RasterSurface);
    QSurfaceFormat format;format.setAlphaBufferSize(8);cursor.setFormat(format);
    cursor.setFlags(Qt::FramelessWindowHint|Qt::WindowDoesNotAcceptFocus|Qt::WindowTransparentForInput);
    // NativeRegionOverlay embeds its cursor under a fromWinId() wrapper.
    // Qt selects that wrapper's screen from a default QWindow geometry, and
    // cursor caches are per screen. Use the same default screen selection here;
    // warming the first RandR output can warm an unrelated cache instead.
    cursor.create();cursor.setCursor(Qt::CrossCursor);
   }
   ready=true;auto reply=envelope();reply["build"]=build;reply["cursor"]=seed;control.send("ready",reply);
   if(error){finish(error.code);return;}
   // Bootstrap prepares only. Durable acceptance must explicitly authorize begin.
   deadline.start(10000);return;

  }
  const bool cursorCommand=type=="begin"||type=="preview";
  if(!ready||settled||payload.size()!=(cursorCommand?3:2)||payload["nonce"]!=nonce||payload["generation"]!=generation){rejectProtocol();return;}
  if(type=="cancel"){cancelled=true;selection.reset();source.cancelAcquisition();finish("cancelled");return;}
  if(cursorCommand){
   bool cursorOk=false;const auto cursor=payload["cursor"].toString().toULongLong(&cursorOk);
   if(!payload["cursor"].isString()||!cursorOk||cursor>UINT32_MAX||QString::number(cursor)!=payload["cursor"].toString()||(cursor&&target.action!="region")||(preparedCursor&&cursor!=preparedCursor)){rejectProtocol();return;}
   if(type=="preview"){
    if(target.action!="region"||previewed||begun||selectionStarted){rejectProtocol();return;}
    putgd::Error changed;if(!native.unchanged(frozen,changed)){finish(changed.code);return;}
    previewed=true;previewCursor=quint32(cursor);
    source.setResidentRegionCursor(previewCursor);
    // Authenticated preview authorizes geometry/input only. It never reads a
    // screen or starts encoding, even if the user completes selection here.
    deadline.start(120000);startSelection();return;
   }
   if(begun||(previewed&&cursor!=previewCursor)){rejectProtocol();return;}
   source.setResidentRegionCursor(quint32(cursor));begun=true;
   putgd::Error changed;if(!native.unchanged(frozen,changed)){finish(changed.code);return;}
   if(!previewed||selection)deadline.start(120000);
   // Keep the invocation target. BEGIN either starts legacy selection or
   // releases previously selected geometry to the authorized pixel path.
   if(target.action=="region")startSelection();
   acquireWhenAuthorized();return;
  }
  if(type=="accept"&&resultSent&&!settled){settled=true;deadline.stop();app.quit();return;}
  rejectProtocol();
 });
 const int status=app.exec();if(encoder){encoder->disconnect(&app);encoder->wait();delete encoder;}artifact.cleanup();return status;
}
