// The method comparisons of #1428 decision 4, in process: one image, one set
// of options, the copied text and the stage times as one JSON line. The engine
// runs here without the helper's protocol or confinement, so a corpus can be
// scored quickly; the helper itself runs the same recognizeImage.
//   bench_ocr_method [--data DIR] [--lang L] [--psm N] [--threshold N] [--no-dict]
//     [--no-invert-lines] [--no-invert] [--scale S] [--max-scale N] [--border N]
//     [--dpi N] [--cap MP] [--confidence F] [--contrast N] [--words] IMAGE...
#include "ocr/Recognize.h"
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <cstdio>

int main(int argc,char** argv){
 qputenv("QT_QPA_PLATFORM","offscreen");
 QGuiApplication app(argc,argv);
 putgd::ocr::JobOptions options;QStringList images,languages;bool words=false;
 const auto args=app.arguments();
 for(int i=1;i<args.size();++i){
  const auto& a=args[i];const auto next=[&]{return i+1<args.size()?args[++i]:QString();};
  if(a=="--data")options.directories.append(next());
  else if(a=="--lang")languages.append(next());
  else if(a=="--psm")options.engine.pageSegmentation=next().toInt();
  else if(a=="--threshold")options.engine.thresholding=next().toInt();
  else if(a=="--no-dict")options.engine.dictionary=false;
  else if(a=="--no-invert-lines")options.engine.invertLines=false;
  else if(a=="--no-invert")options.invertDark=false;
  else if(a=="--scale")options.limits.forcedScale=next().toDouble();
  else if(a=="--max-scale")options.limits.maximumScale=next().toInt();
  else if(a=="--border")options.limits.border=next().toInt();
  else if(a=="--dpi")options.engine.resolution=next().toInt();
  else if(a=="--cap")options.limits.scaledPixels=qint64(next().toDouble()*1000000);
  else if(a=="--confidence")options.filter.confidence=next().toFloat();
  else if(a=="--contrast")options.filter.contrast=next().toInt();
  else if(a=="--words")words=true;
  else images.append(a);
 }
 if(!languages.isEmpty())options.languages=languages;
 if(options.directories.isEmpty())for(const auto& d:{"/usr/share/tesseract-ocr/5/tessdata","/usr/share/tesseract/tessdata","/usr/share/tessdata"})options.directories.append(QString::fromLatin1(d));
 options.words=words;
 for(const auto& path:images){
  QImageReader reader(path);reader.setAllocationLimit(0);const auto image=reader.read();
  if(image.isNull()){std::fprintf(stderr,"cannot read %s\n",qPrintable(path));continue;}
  QImage flat(image.size(),QImage::Format_RGB32);flat.fill(Qt::white);{QPainter p(&flat);p.drawImage(0,0,image);}
  const auto grey=flat.convertToFormat(QImage::Format_Grayscale8);
  const putgd::ocr::GreyView view{grey.constBits(),grey.width(),grey.height(),int(grey.bytesPerLine())};
  const auto result=putgd::ocr::recognizeImage(view,options);
  QJsonObject line{{"image",path},{"error",result.error},{"text",result.text},{"scale",result.recognition.scale},{"bands",result.recognition.bands},
                   {"lineHeight",result.profile.lineHeight},{"dark",result.profile.dark},{"codesMs",result.timings.codesMs},
                   {"engineStartMs",result.timings.engineStartMs},{"recognitionMs",result.timings.recognitionMs},{"totalMs",result.timings.totalMs},
                   {"codes",result.recognition.codes.size()},{"words",result.recognition.words.size()}};
  if(words){
   QJsonArray list;for(const auto& w:result.recognition.words)list.append(QJsonObject{{"t",w.text},{"c",int(w.confidence)},{"k",w.kept},{"r",QJsonArray{w.rect.x(),w.rect.y(),w.rect.width(),w.rect.height()}}});
   line["wordList"]=list;
  }
  std::printf("%s\n",QJsonDocument(line).toJson(QJsonDocument::Compact).constData());std::fflush(stdout);
 }
 return 0;
}
