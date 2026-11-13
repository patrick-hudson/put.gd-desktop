#pragma once
#include "Layout.h"
#include "OcrImage.h"
#include "TextEngine.h"
#include "Words.h"
#include <QByteArray>
#include <QStringList>

// One recognition job, from the grey input to the result JSON (#1428 decisions 3 and 4).
namespace putgd::ocr {
inline constexpr qint64 maximumOutputBytes=8*1024*1024;
inline constexpr qsizetype maximumTextBytes=1024*1024;
inline constexpr int maximumWords=20000;
inline constexpr qsizetype maximumWordBytes=256;

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
 bool truncated=false;
 QStringList used,skipped,dataFiles;
 JobTimings timings;
 Profile profile;
};
JobResult recognizeImage(const GreyView& image,const JobOptions& options);
// The result as compact JSON, within the bounds: text at most 1 MiB, 20,000
// words of at most 256 bytes, 64 codes of at most 8 KiB, and the whole at
// most outputLimit, cutting candidates first, then words, then codes, then text.
QByteArray encodeResult(const JobResult& result,const JobOptions& options,qint64 outputLimit=maximumOutputBytes);
}
