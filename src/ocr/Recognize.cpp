#include "Recognize.h"
#include "CodeReader.h"
#include "Sanitize.h"
#include <QElapsedTimer>

namespace putgd::ocr {
JobResult recognizeImage(const GreyView& image,const JobOptions& options){
 JobResult result;QElapsedTimer total;total.start();
 if(!image.valid()||qint64(image.width)*image.height>maximumInputPixels){result.error=QStringLiteral("input_excessive");return result;}
 // Codes first, from the full image; their areas are filled in every band copy.
 QElapsedTimer stage;stage.start();
 bool truncated=false;
 result.recognition.codes=readCodes(image,truncated);
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
 bool cut=false;result.text=clampUtf8(copiedText(result.recognition),maximumTextBytes,cut);
 result.truncated=truncated||cut;
 result.timings.totalMs=total.elapsed();
 return result;
}
}
