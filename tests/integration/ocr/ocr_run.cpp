// Drives the real putgd-ocr through the contract's TextJob and prints one JSON
// line per run: the parent's spawn and total times, the helper's engine start
// and recognition times, and the helper's peak RSS. For measurements on the
// builders; never part of a package. The peak is the largest of every helper
// this process has reaped (RUSAGE_CHILDREN), so give one image per invocation
// for a per-job figure.
//   test_ocr_run --helper PATH [--data DIR]... [--lang L]... [--words] [--candidates] [--text] IMAGE
#include "core/TextJob.h"
#include <QFileInfo>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QElapsedTimer>
#include <cstdio>
#include <sys/resource.h>

int main(int argc,char** argv){
 qputenv("QT_QPA_PLATFORM","offscreen");
 QGuiApplication app(argc,argv);
 QString helper;QStringList data,images;bool printText=false;
 putgd::TextJob::Options options;
 const auto args=app.arguments();
 for(int i=1;i<args.size();++i){
  const auto& a=args[i];
  if(a=="--helper"&&i+1<args.size())helper=args[++i];
  else if(a=="--data"&&i+1<args.size())data.append(args[++i]);
  else if(a=="--lang"&&i+1<args.size())options.languages.append(args[++i]);
  else if(a=="--required")options.languagesRequired=true;
  else if(a=="--words")options.words=true;
  else if(a=="--candidates")options.candidates=true;
  else if(a=="--text")printText=true;
  else images.append(a);
 }
 if(options.languages.isEmpty())options.languages={QStringLiteral("eng")};
 if(helper.isEmpty()||images.isEmpty()){std::fprintf(stderr,"usage: test_ocr_run --helper PATH [--data DIR] [--lang L] IMAGE\n");return 2;}
 putgd::TextJob job(helper);if(!data.isEmpty())job.setDataDirectories(data);
 int failures=0;
 for(const auto& path:images){
  QImageReader reader(path);reader.setAllocationLimit(0);const auto image=reader.read();
  if(image.isNull()){std::fprintf(stderr,"cannot read %s\n",qPrintable(path));++failures;continue;}
  QElapsedTimer clock;clock.start();
  const auto grey=putgd::TextJob::grey(image);const auto greyMs=clock.elapsed();
  putgd::Error error;const auto result=job.recognize(grey,options,error);
  struct rusage usage{};::getrusage(RUSAGE_CHILDREN,&usage);
  QJsonObject line{{"image",path},{"width",image.width()},{"height",image.height()},{"error",error.code},{"greyMs",greyMs},
                   {"spawnMs",result.spawnMs},{"engineStartMs",result.engineStartMs},{"recognizeMs",result.recognizeMs},{"totalMs",result.totalMs},
                   {"peakRssKiB",qint64(usage.ru_maxrss)},{"chars",result.text.size()},{"codes",result.codes.size()},{"banded",result.banded},{"truncated",result.truncated}};
  if(printText)line["text"]=result.text;
  std::printf("%s\n",QJsonDocument(line).toJson(QJsonDocument::Compact).constData());std::fflush(stdout);
  if(error)++failures;
 }
 return failures?1:0;
}
