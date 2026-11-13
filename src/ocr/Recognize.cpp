#include "Recognize.h"
#include "CodeReader.h"
#include <QElapsedTimer>
#include <cmath>

namespace putgd::ocr {
JobResult recognizeImage(const GreyView& image,const JobOptions& options){
 JobResult result;QElapsedTimer total;total.start();
 if(!image.valid()||qint64(image.width)*image.height>maximumInputPixels){result.error=QStringLiteral("input_excessive");return result;}
 // Codes first, from the full image; their areas are filled in every band copy.
 QElapsedTimer stage;stage.start();
 result.recognition.codes=readCodes(image);
 QList<Quad> areas;for(const auto& code:result.recognition.codes)areas.append(code.quad);
 result.timings.codesMs=stage.elapsed();
 result.profile=measure(image,areas);
 const auto plan=putgd::ocr::plan(image.width,image.height,result.profile,options.limits);
 if(!plan.error.isEmpty()){result.error=plan.error;return result;}
 stage.restart();
 TextEngine engine;
 result.error=engine.start(options.languages,options.strictLanguages,options.directories,options.engine,result.failedLanguage);
 result.timings.engineStartMs=stage.elapsed();
 if(!result.error.isEmpty())return result;
 result.used=engine.used();result.skipped=engine.skipped();result.dataFiles=engine.dataFiles();
 stage.restart();
 const bool invert=options.invertDark&&result.profile.dark;
 int previousFirst=0;
 for(int i=0;i<plan.bands.size();++i){
  const auto& band=plan.bands[i];
  const auto copy=bandImage(image,band,plan.scale,options.limits.border,areas,result.profile.background,invert);
  const auto text=engine.recognize(copy,plan.scale);
  previousFirst=mergeBand(result.recognition,text,band,i>0?&plan.bands[i-1]:nullptr,previousFirst,plan.scale,options.limits.border,i,image.width,image.height);
 }
 result.timings.recognitionMs=stage.elapsed();
 result.recognition.banded=plan.banded();result.recognition.scale=plan.scale;result.recognition.bands=int(plan.bands.size());
 joinUrls(result.recognition);
 applyCopyFilter(result.recognition,image,options.filter);
 result.text=copiedText(result.recognition,&result.lineTops);
 result.timings.totalMs=total.elapsed();
 return result;
}
TextResult toTextResult(const JobResult& job,const JobOptions& options){
 TextResult out;out.text=job.text;out.lineTops=job.lineTops;out.truncated=job.truncated;out.banded=job.recognition.banded;
 out.languagesUsed=job.used;out.languagesSkipped=job.skipped;
 out.engineStartMs=job.timings.engineStartMs;out.recognizeMs=job.timings.codesMs+job.timings.recognitionMs;
 const auto word=[](const Word& w){return TextWord{w.text,w.rect,int(std::lround(w.confidence))};};
 for(const auto& w:job.recognition.words){
  if(options.words&&w.kept)out.words.append(word(w));
  if(options.candidates)out.candidates.append(word(w));
 }
 for(const auto& c:job.recognition.codes){
  TextCode code;code.format=c.format;code.binary=!c.text;code.text=c.text?c.content:QString();code.length=c.length;
  code.quad={c.quad[0],c.quad[1],c.quad[2],c.quad[3]};out.codes.append(code);
 }
 return out;
}
}
