// Emoji matching (#1748) on PNG files, for measurements on the builders; never
// part of a package. One JSON line per image: the hits, candidates and time.
//   bench_emoji [--templates FILE] [--distance D] [--shortlist K] [--coarse K] [--stretched D]
//     [--candidates] IMAGE...
#include "ocr/Emoji.h"
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <cstdio>

int main(int argc,char** argv){
 qputenv("QT_QPA_PLATFORM","offscreen");
 QGuiApplication app(argc,argv);
 putgd::ocr::EmojiOptions options;QString path;QStringList images;
 const auto args=app.arguments();
 for(int i=1;i<args.size();++i){
  const auto& a=args[i];const auto next=[&]{return i+1<args.size()?args[++i]:QString();};
  if(a=="--templates")path=next();
  else if(a=="--distance")options.distance=next().toDouble();
  else if(a=="--shortlist")options.shortlist=next().toInt();
  else if(a=="--candidates")options.keepCandidates=true;
  else if(a=="--stretched")options.stretchedDistance=next().toDouble();
  else if(a=="--coarse")options.coarseShortlist=next().toInt();
  else images.append(a);
 }
 QByteArray bytes;putgd::ocr::EmojiTemplates loaded;
 if(!path.isEmpty()){
  QFile file(path);if(!file.open(QIODevice::ReadOnly)){std::fprintf(stderr,"cannot read %s\n",qPrintable(path));return 2;}
  bytes=file.readAll();
  if(!loaded.load(reinterpret_cast<const uint8_t*>(bytes.constData()),bytes.size())){std::fprintf(stderr,"not a template file\n");return 2;}
 }
 const auto& templates=path.isEmpty()?putgd::ocr::EmojiTemplates::builtIn():loaded;
 for(const auto& image:images){
  QImage source(image);if(source.isNull()){std::fprintf(stderr,"cannot read %s\n",qPrintable(image));continue;}
  QImage flat(source.size(),QImage::Format_RGB888);flat.fill(Qt::white);{QPainter p(&flat);p.drawImage(0,0,source);}
  putgd::ocr::ColourView view{flat.constBits(),flat.width(),flat.height(),flat.bytesPerLine(),{}};
  QElapsedTimer timer;timer.start();
  const auto scan=putgd::ocr::findEmoji(view,templates,options);
  QJsonArray hits,examined;
  for(const auto& c:scan.examined)examined.append(QJsonArray{c.rect.x(),c.rect.y(),c.rect.width(),c.rect.height(),c.fill,c.distance,c.stretched});
  for(const auto& hit:scan.hits)hits.append(QJsonObject{{"t",hit.text},{"r",QJsonArray{hit.rect.x(),hit.rect.y(),hit.rect.width(),hit.rect.height()}},{"d",hit.distance},{"s",hit.style}});
  std::printf("%s\n",QJsonDocument(QJsonObject{{"image",image},{"hits",hits},{"candidates",scan.candidates},{"cut",scan.cut},{"examined",examined},{"ms",double(timer.elapsed())}}).toJson(QJsonDocument::Compact).constData());
  std::fflush(stdout);
 }
 return 0;
}
