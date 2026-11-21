// The method comparisons of #1428 decision 4, in process: one image, one set
// of options, the copied text and the stage times as one JSON line. The engine
// runs here without the helper's protocol or confinement, so a corpus can be
// scored quickly; the helper itself runs the same recognizeImage.
//   bench_ocr_method [--data DIR] [--lang L] [--psm N] [--threshold N] [--no-dict]
//     [--no-invert-lines] [--no-invert] [--scale S] [--max-scale N] [--border N]
//     [--dpi N] [--cap MP] [--band MP] [--confidence F] [--contrast N] [--words]
//     [--no-emoji] [--emoji-templates FILE] [--emoji-distance D] [--symbols] IMAGE...
// Emoji (#1748) are looked for in the image's colour, with the built-in
// templates unless --emoji-templates names a file.
#include "ocr/Recognize.h"
#include "ocr/Layout.h"
#include <QGuiApplication>
#include <QFile>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPainter>
#include <cstdio>

int main(int argc,char** argv){
 qputenv("QT_QPA_PLATFORM","offscreen");
 QGuiApplication app(argc,argv);
 putgd::ocr::JobOptions options;QStringList images,languages;bool words=false,emoji=true,symbols=false;QString templatesPath;
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
  else if(a=="--threads")options.threads=next().toInt();
  else if(a=="--parallel-mp")options.parallelPixels=qint64(next().toDouble()*1000000);
  else if(a=="--dump")options.dumpBand=next();
  else if(a=="--budget")options.budgetMs=next().toInt();
  else if(a=="--band")options.limits.bandPixels=qint64(next().toDouble()*1000000);
  else if(a=="--single")options.limits.singlePassPixels=qint64(next().toDouble()*1000000);
  else if(a=="--strips")options.stripsPerThread=next().toInt();
  else if(a=="--memory")options.memoryBudget=qint64(next().toDouble()*1024*1024);
  else if(a=="--no-panels")options.panels=false;
  else if(a=="--large-cap")options.limits.largeScaledPixels=qint64(next().toDouble()*1000000);
  else if(a=="--cap")options.limits.scaledPixels=qint64(next().toDouble()*1000000);
  else if(a=="--confidence")options.filter.confidence=next().toFloat();
  else if(a=="--contrast")options.filter.contrast=next().toInt();
  else if(a=="--words")words=true;
  else if(a=="--no-emoji")emoji=false;
  else if(a=="--emoji-templates")templatesPath=next();
  else if(a=="--emoji-distance")options.emoji.distance=next().toDouble();
  else if(a=="--symbols")symbols=true;
  else if(a=="--var"){const auto kv=next();const int eq=kv.indexOf('=');options.engine.research.emplace_back(kv.left(eq).toStdString(),kv.mid(eq+1).toStdString());}
  else if(a=="--target-rows")putgd::ocr::researchTargetLineRows=next().toInt();
  else if(a=="--line-tasks")putgd::ocr::researchLineTasks=1;
  else if(a=="--min-scale")putgd::ocr::researchMinimumScale=next().toDouble();
  else images.append(a);
 }
 if(!languages.isEmpty())options.languages=languages;
 if(options.directories.isEmpty())for(const auto& d:{"/usr/share/tesseract-ocr/5/tessdata","/usr/share/tesseract/tessdata","/usr/share/tessdata"})options.directories.append(QString::fromLatin1(d));
 options.words=words;
 QByteArray templateBytes;putgd::ocr::EmojiTemplates loaded;
 if(!templatesPath.isEmpty()){
  QFile file(templatesPath);
  if(file.open(QIODevice::ReadOnly))templateBytes=file.readAll();
  if(templateBytes.isEmpty()||!loaded.load(reinterpret_cast<const uint8_t*>(templateBytes.constData()),templateBytes.size())){std::fprintf(stderr,"not a template file: %s\n",qPrintable(templatesPath));return 2;}
 }
 if(emoji)options.emojiTemplates=templatesPath.isEmpty()?&putgd::ocr::EmojiTemplates::builtIn():&loaded;
 for(const auto& path:images){
  // Decoded and converted in a scope of their own, so only the grey copy is
  // alive when the peak is reset below.
  QImage grey,colour;
  {
   QImageReader reader(path);reader.setAllocationLimit(0);const auto image=reader.read();
   if(image.isNull()){std::fprintf(stderr,"cannot read %s\n",qPrintable(path));continue;}
   QImage flat(image.size(),QImage::Format_RGB32);flat.fill(Qt::white);{QPainter p(&flat);p.drawImage(0,0,image);}
   grey=flat.convertToFormat(QImage::Format_Grayscale8);
   if(emoji)colour=flat.convertToFormat(QImage::Format_RGB888);
  }
  options.colour=emoji?putgd::ocr::ColourView{colour.constBits(),colour.width(),colour.height(),colour.bytesPerLine(),{}}:putgd::ocr::ColourView{};
  // Peak RSS from here on only: the decode above is the parent's, not the helper's.
  {QFile clear(QStringLiteral("/proc/self/clear_refs"));if(clear.open(QIODevice::WriteOnly))clear.write("5");}
  const putgd::ocr::GreyView view{grey.constBits(),grey.width(),grey.height(),int(grey.bytesPerLine())};
  const auto result=putgd::ocr::recognizeImage(view,options);
  long peak=0;{QFile status(QStringLiteral("/proc/self/status"));if(status.open(QIODevice::ReadOnly))for(const auto& l:status.readAll().split('\n'))if(l.startsWith("VmHWM:"))peak=l.mid(6).trimmed().split(' ').value(0).toLong();}
  QJsonObject line{{"image",path},{"peakKiB",qint64(peak)},{"error",result.error},{"text",result.text},{"scale",result.recognition.scale},{"bands",result.recognition.bands},
                   {"lineHeight",result.profile.lineHeight},{"dark",result.profile.dark},{"codesMs",result.timings.codesMs},
                   {"engineStartMs",result.timings.engineStartMs},{"recognitionMs",result.timings.recognitionMs},{"totalMs",result.timings.totalMs},
                   {"codes",result.recognition.codes.size()},{"words",result.recognition.words.size()},
                   {"emoji",result.emoji},{"emojiCandidates",result.emojiCandidates},{"emojiCut",result.emojiCut},{"emojiMs",result.timings.emojiMs},{"truncated",result.truncated},{"parts",result.recognition.parts},{"panels",result.recognition.panels},{"threads",result.recognition.threads},{"engineTasks",[&]{QJsonArray a;for(int n:result.recognition.engineTasks)a.append(n);return a;}()}};
  if(words){
   QJsonArray list;
   for(const auto& w:result.recognition.words){
    QJsonObject item{{"t",w.text},{"c",int(w.confidence)},{"k",w.kept},{"l",w.line},{"r",QJsonArray{w.rect.x(),w.rect.y(),w.rect.width(),w.rect.height()}}};
    if(w.emoji)item["e"]=true;
    if(w.attach)item["a"]=true;
    if(symbols){QJsonArray list;for(const auto& c:w.symbols)list.append(QJsonArray{c.rect.x(),c.rect.y(),c.rect.width(),c.rect.height(),int(c.confidence),c.length});item["s"]=list;}
    list.append(item);
   }
   line["wordList"]=list;
  }
  std::printf("%s\n",QJsonDocument(line).toJson(QJsonDocument::Compact).constData());std::fflush(stdout);
 }
 return 0;
}
