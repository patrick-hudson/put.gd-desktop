#pragma once
#include "Layout.h"
#include "OcrImage.h"
#include "TextEngine.h"
#include "Words.h"
#include <QByteArray>
#include <QStringList>

// One recognition job, from the grey input to the result JSON (#1428 decisions 3 and 4).
namespace putgd::ocr {

struct JobOptions {
 QStringList languages{QStringLiteral("eng")};
 bool strictLanguages=true;
 QStringList directories;   // language data, in resolution order
 bool words=false;          // the filtered words of the copied text, with boxes
 bool candidates=false;     // every word, unfiltered (the editor's Quick redact)
 EngineOptions engine;
 PlanLimits limits;
 CopyFilter filter;
 bool invertDark=true;      // put.gd's own polarity: a dark page is inverted
};
struct JobTimings { qint64 codesMs=0,engineStartMs=0,recognitionMs=0,totalMs=0; };
struct JobResult {
 QString error;             // input_excessive, language_unavailable, engine_unavailable
 QString failedLanguage;
 Recognition recognition;
 QString text;
 QList<int> lineTops;       // one per line of text, source pixels
 bool truncated=false;
 QStringList used,skipped,dataFiles;
 JobTimings timings;
 Profile profile;
};
JobResult recognizeImage(const GreyView& image,const JobOptions& options);
// The contract's result: text with its line tops, the filtered words when
// asked, every word as candidates when asked, codes and timings. The
// contract's encoder applies the bounds.
TextResult toTextResult(const JobResult& result,const JobOptions& options);
}
