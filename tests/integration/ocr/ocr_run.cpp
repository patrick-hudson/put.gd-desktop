// Drives the real putgd-ocr once per image and prints one JSON line per run:
// stage timings, the helper's peak RSS and the result. For measurements on the
// builders; never part of a package.
//   test_ocr_run --helper PATH [--data DIR]... [--lang L]... [--words] [--candidates] [--repeat N] [--text] IMAGE...
#include "integration/ocr/OcrParent.h"
#include <QCoreApplication>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

int main(int argc,char** argv){
 qputenv("QT_QPA_PLATFORM","offscreen");
 QGuiApplication app(argc,argv);
 putgd::test::OcrRequest request;QStringList images;int repeat=1;bool printText=false;QStringList languages;
 const auto args=app.arguments();
 for(int i=1;i<args.size();++i){
  const auto& a=args[i];
  if(a=="--helper"&&i+1<args.size())request.helper=args[++i];
  else if(a=="--data"&&i+1<args.size())request.dataDirectories.append(args[++i]);
  else if(a=="--lang"&&i+1<args.size())languages.append(args[++i]);
  else if(a=="--skip-missing")request.strictLanguages=false;
  else if(a=="--words")request.words=true;
  else if(a=="--candidates")request.candidates=true;
  else if(a=="--repeat"&&i+1<args.size())repeat=args[++i].toInt();
  else if(a=="--text")printText=true;
  else if(a=="--timeout"&&i+1<args.size())request.timeoutMs=args[++i].toInt();
  else images.append(a);
 }
 if(!languages.isEmpty())request.languages=languages;
 if(request.dataDirectories.isEmpty())request.dataDirectories.append(putgd::test::defaultDataDirectory());
 if(request.helper.isEmpty()||images.isEmpty()){std::fprintf(stderr,"usage: test_ocr_run --helper PATH [--data DIR] [--lang L] [--words] [--repeat N] IMAGE...\n");return 2;}
 int failures=0;
 for(const auto& path:images){
  QImageReader reader(path);reader.setAllocationLimit(0);const auto image=reader.read();
  if(image.isNull()){std::fprintf(stderr,"cannot read %s\n",qPrintable(path));++failures;continue;}
  for(int r=0;r<repeat;++r){
   const auto run=putgd::test::runOcr(image,request);
   QJsonObject line{{"image",path},{"width",image.width()},{"height",image.height()},{"error",run.error},{"inputMs",run.inputMs},{"spawnMs",run.spawnMs},
                    {"recognitionMs",run.recognitionMs},{"copyMs",run.copyMs},{"totalMs",run.totalMs},{"peakRssKiB",qint64(run.peakRssKiB)},
                    {"exitCode",run.exitCode},{"signal",run.signal},{"childGone",run.childGone},{"engine",run.result["engine"]},
                    {"chars",run.result["text"].toString().size()},{"codes",run.result["codes"].toArray().size()},{"banded",run.result["banded"]}};
   if(printText){line["text"]=run.result["text"];line["codesList"]=run.result["codes"];}
   std::printf("%s\n",QJsonDocument(line).toJson(QJsonDocument::Compact).constData());std::fflush(stdout);
   if(!run.error.isEmpty())++failures;
  }
 }
 return failures?1:0;
}
