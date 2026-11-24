// The sensitive-text fuzz driver (#1931): the real putgd-ocr helper and the real
// quickRedact::find() and markBoxes(), on many images in one process. Built by
// tools/lab/redact_fuzz/build.py from a put.gd source tree at the commit of the
// package whose helper it drives; never part of a package.
//
//   redact_fuzz_driver --helper PATH --data DIR [--data DIR] --build-json FILE
//                      [--lang L]... [--detect] [--text] < JOBS.jsonl > RESULTS.jsonl
//
// --detect: the script check (#1981) as the product runs it when no language
// was chosen; each result then carries "detection". --text: also the copied text.
//
// One JSON object per input line:
//   {"id": "...", "image": "/abs/scene.png",
//    "words": [[text, x, y, w, h], ...],        // optional: ground-truth words (the oracle); a sixth
//                                               // element is the confidence (95 when left out) and a
//                                               // seventh 1 for a second read, as the "words" results give
//    "login": "...", "realName": "...", "terms": ["..."],
//    "profiles": {"all": ["email", ...], "check": ["card", "key", "list"]}}
// and one result line per job: the recognized candidates, and for each profile
// the matches with their words and Redact marks, on the recognized words
// ("ocr") and, when given, on the ground-truth words ("oracle").
//
// The editor's path is copied exactly: the helper runs with words and
// candidates on, the candidates go through TextLayer::exclude, a profile is one
// find() over its categories with the name and list sources (no patterns), and
// each match becomes markBoxes() over the image. --build-json is the package's
// `putgd --version-json`: the helper accepts only a parent of its own build.
#include "core/BuildInfo.h"
#include "core/TextJob.h"
#include "editor/QuickRedact.h"
#include "editor/TextLayer.h"
#include <QElapsedTimer>
#include <QFile>
#include <QGuiApplication>
#include <QImageReader>
#include <QJsonArray>
#include <QJsonDocument>
#include <sys/resource.h>
#include <QJsonObject>
#include <cstdio>
#include <iostream>
#include <string>

namespace {
QByteArray manifest;
}
namespace putgd {
const char* buildManifestJson() noexcept {return manifest.constData();}
const char* buildVersion() noexcept {return "redact-fuzz-driver";}
// TextLayer.cpp's recognition revision needs this; the driver never calls it. The body is EditDocument's.
QRect editor::EditDocument::clampRect(const QRect& rect,const QRect& bounds){return rect.normalized().intersected(bounds);}
}

using namespace putgd;
namespace qr=editor::quickRedact;

namespace {
QJsonArray rect(const QRect& r){return QJsonArray{r.x(),r.y(),r.width(),r.height()};}
QJsonObject profile(const QList<editor::RecognizedWord>& words,const QStringList& categories,const qr::Sources& sources,const QImage& image){
 QElapsedTimer clock;clock.start();
 const auto found=qr::find(words,categories,sources,2000);
 QJsonArray matches;
 for(const auto& match:found.matches){
  QJsonArray indexes;QStringList text;
  for(const int w:match.words){indexes.append(w);text.append(words.at(w).text);}
  QJsonArray marks;
  for(const auto& box:qr::markBoxes(words,match,image.rect(),image))marks.append(QJsonArray{rect(box.mark),rect(box.words)});
  matches.append(QJsonObject{{"category",match.category},{"words",indexes},{"text",text.join(' ')},{"marks",marks}});
 }
 QJsonObject out{{"matches",matches},{"matchMs",double(clock.elapsed())}};
 if(found.unfinished)out["unfinished"]=true;
 if(found.nameUnchecked)out["nameUnchecked"]=true;
 return out;
}
QJsonObject run(const QList<editor::RecognizedWord>& words,const QJsonObject& profiles,const qr::Sources& sources,const QImage& image){
 QJsonObject out;
 for(auto it=profiles.begin();it!=profiles.end();++it){
  QStringList categories;for(const auto& c:it.value().toArray())categories.append(c.toString());
  out[it.key()]=profile(words,categories,sources,image);
 }
 return out;
}
}

int main(int argc,char** argv){
 qputenv("QT_QPA_PLATFORM","offscreen");
 QGuiApplication app(argc,argv);
 QString helper,buildJson;QStringList data;textjob::Options options;options.words=true;options.candidates=true;bool withText=false;
 const auto args=app.arguments();
 for(int i=1;i<args.size();++i){
  const auto& a=args[i];
  if(a=="--helper"&&i+1<args.size())helper=args[++i];
  else if(a=="--data"&&i+1<args.size())data.append(args[++i]);
  else if(a=="--build-json"&&i+1<args.size())buildJson=args[++i];
  else if(a=="--lang"&&i+1<args.size())options.languages.append(args[++i]);
  else if(a=="--detect")options.detectScript=true;
  else if(a=="--text")withText=true;
  else{std::fprintf(stderr,"unknown argument %s\n",qPrintable(a));return 2;}
 }
 if(options.languages.isEmpty())options.languages={QStringLiteral("eng")};
 QFile build(buildJson);
 if(helper.isEmpty()||!build.open(QIODevice::ReadOnly)){std::fprintf(stderr,"usage: redact_fuzz_driver --helper PATH --data DIR --build-json FILE < jobs\n");return 2;}
 // The helper compares the parsed object; keep it compact like the compiled one.
 manifest=QJsonDocument::fromJson(build.readAll()).toJson(QJsonDocument::Compact);
 if(manifest.isEmpty()||manifest=="{}"){std::fprintf(stderr,"--build-json holds no build object\n");return 2;}
 TextJob job(helper);if(!data.isEmpty())job.setDataDirectories(data);
 std::string line;int failures=0;
 while(std::getline(std::cin,line)){
  if(line.empty())continue;
  const auto spec=QJsonDocument::fromJson(QByteArray::fromStdString(line)).object();
  const auto path=spec["image"].toString();
  QJsonObject out{{"id",spec["id"]},{"image",path}};
  qr::Sources sources;sources.login=spec["login"].toString();sources.realName=spec["realName"].toString();
  for(const auto& t:spec["terms"].toArray())sources.terms.append(t.toString());
  const auto profiles=spec["profiles"].toObject();
  QImageReader reader(path);reader.setAllocationLimit(0);const auto image=reader.read();
  if(image.isNull()){out["error"]="unreadable_image";++failures;}
  else{
   out["width"]=image.width();out["height"]=image.height();
   if(spec.contains("words")){
    QList<editor::RecognizedWord> truth;
    for(const auto& w:spec["words"].toArray()){const auto a=w.toArray();truth.append({a.at(0).toString(),QRect(a.at(1).toInt(),a.at(2).toInt(),a.at(3).toInt(),a.at(4).toInt()),a.size()>5?a.at(5).toInt():95,0,a.size()>6&&a.at(6).toInt()!=0});}
    QList<editor::RecognizedCode> none;editor::TextLayer::exclude(truth,none,{});
    out["oracle"]=run(truth,profiles,sources,image);
   }
   if(!spec["oracleOnly"].toBool()){
    Error error;const auto result=job.recognize(image,options,error);
    out["recognizeMs"]=double(result.recognizeMs);out["totalMs"]=double(result.totalMs);
    out["truncated"]=result.truncated;out["banded"]=result.banded;
    if(!result.detectionOutcome.isEmpty())out["detection"]=textjob::detectionJson(result);
    if(withText)out["text"]=result.text;
    // The largest helper this driver has reaped so far (RUSAGE_CHILDREN), not this job's:
    // a per-job figure needs one job per driver.
    {struct rusage usage{};::getrusage(RUSAGE_CHILDREN,&usage);out["driverPeakRssKiB"]=qint64(usage.ru_maxrss);}
    if(error){out["error"]=error.code;++failures;}
    else{
     QList<editor::RecognizedWord> words;for(const auto& c:result.candidates)words.append({c.text,c.rect,c.confidence,0,c.second});
     QList<editor::RecognizedCode> none;editor::TextLayer::exclude(words,none,{});
     QJsonArray jw;for(const auto& w:words){QJsonArray e{w.text,w.rect.x(),w.rect.y(),w.rect.width(),w.rect.height(),w.confidence};if(w.second)e.append(1);jw.append(e);}
     out["words"]=jw;
     QJsonArray codes;for(const auto& c:result.codes)codes.append(QJsonObject{{"format",c.format},{"length",double(c.length)}});
     if(!codes.isEmpty())out["codes"]=codes;
     out["ocr"]=run(words,profiles,sources,image);
    }
   }
  }
  std::printf("%s\n",QJsonDocument(out).toJson(QJsonDocument::Compact).constData());std::fflush(stdout);
 }
 return failures?1:0;
}
