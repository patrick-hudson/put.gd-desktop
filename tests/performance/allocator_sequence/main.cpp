#include <QCoreApplication>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMimeDatabase>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QJsonArray>
#include <cstring>
#include <gnu/libc-version.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif
static QJsonObject memory(){
 QFile f("/proc/self/smaps_rollup");QJsonObject r;if(!f.open(QIODevice::ReadOnly))return {};
 const auto lines=f.readAll().split('\n');for(const auto& line:lines){const auto a=line.simplified().split(' ');if(a.size()>=2&&(a[0]=="Rss:"||a[0]=="Pss:"||a[0]=="Anonymous:"||a[0]=="Private_Dirty:"||a[0]=="Private_Clean:"))r[QString::fromLatin1(a[0].chopped(1))]=double(a[1].toLongLong()*1024);}
#ifdef __GLIBC__
 const auto m=mallinfo2();r["allocatorLiveBytes"]=double(m.uordblks);r["allocatorFreeBytes"]=double(m.fordblks);r["allocatorMmapBytes"]=double(m.hblkhd);r["allocatorArenaBytes"]=double(m.arena);
#endif
 return r;
}
int main(int argc,char**argv){
 if(argc!=2||(std::strcmp(argv[1],"dynamic")&&std::strcmp(argv[1],"static128k")))return 2;
 const bool pinned=std::strcmp(argv[1],"static128k")==0;
#ifdef __GLIBC__
 const int policyResult=pinned?mallopt(M_MMAP_THRESHOLD,128*1024):0;
 if(pinned&&policyResult!=1)return 3;
#else
 return 4;
#endif
 QCoreApplication app(argc,argv);const auto before=memory();QJsonArray stages;
 const QList<QSize> sizes{QSize(640,448),QSize(1280,800),QSize(640,448),QSize(1280,800)};
 for(const auto size:sizes){
  const auto stageBefore=memory();const qsizetype total=qsizetype(size.width())*size.height()*4;
  QByteArray raw;const QByteArray chunk(65536,'x');
  while(raw.size()<total)raw.append(chunk.constData(),std::min<qsizetype>(chunk.size(),total-raw.size()));
  const auto capacity=raw.capacity();
  for(int y=0;y<size.height();++y){auto* row=reinterpret_cast<QRgb*>(raw.data()+y*size.width()*4);for(int x=0;x<size.width();++x)row[x]=y<size.height()/2?(x<size.width()/2?qRgb(224,32,32):qRgb(32,192,64)):(x<size.width()/2?qRgb(32,64,224):qRgb(232,216,32));}
  QImage decoded;auto* decode=QThread::create([bytes=std::move(raw),size,&decoded]{const QImage view(reinterpret_cast<const uchar*>(bytes.constData()),size.width(),size.height(),size.width()*4,QImage::Format_ARGB32);decoded=view.copy();});decode->start();decode->wait();delete decode;
  bool ok=false;auto* encode=QThread::create([image=decoded,size,&ok]{QByteArray png;QBuffer output(&png);output.open(QIODevice::WriteOnly);if(!image.save(&output,"PNG"))return;output.close();QBuffer input(&png);input.open(QIODevice::ReadOnly);QImageReader reader(&input,"png");const auto actual=reader.size();reader.setScaledSize(actual.scaled(256,256,Qt::KeepAspectRatio));auto thumbnail=reader.read().scaled(256,256,Qt::KeepAspectRatio,Qt::SmoothTransformation);QByteArray preview;QBuffer previewOutput(&preview);previewOutput.open(QIODevice::WriteOnly);ok=actual==size&&thumbnail.size()==size.scaled(256,256,Qt::KeepAspectRatio)&&thumbnail.save(&previewOutput,"PNG");});encode->start();encode->wait();delete encode;decoded={};if(!ok)return 5;
  const auto after=memory();const int trimmed=malloc_trim(0);const auto postTrim=memory();stages.append(QJsonObject{{"width",size.width()},{"height",size.height()},{"rawBytes",double(total)},{"rawCapacityBytes",double(capacity)},{"pngStagePassed",ok},{"before",stageBefore},{"after",after},{"postTrim",postTrim},{"trimResult",trimmed}});
 }
 const auto final=memory();QJsonObject result{{"schemaVersion",1},{"qtVersion",qVersion()},{"glibcVersion",gnu_get_libc_version()},{"mode",pinned?"static128k":"dynamic"},{"policyResult",policyResult},{"before",before},{"stages",stages},{"final",final},{"syntheticOnly",true},{"payloadExported",false},{"environmentChanged",false},{"appOwnedUnixSockets",QJsonArray{}}};QTextStream(stdout)<<QJsonDocument(result).toJson(QJsonDocument::Compact)<<Qt::endl;return 0;
}
