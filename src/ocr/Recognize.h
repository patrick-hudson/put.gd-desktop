#pragma once
#include "Layout.h"
#include "OcrImage.h"
#include "TextEngine.h"
#include "Words.h"
#include <QByteArray>
#include <QStringList>

// One recognition job, from the grey input to the result JSON (#1428 decisions 3 and 4).
namespace putgd::ocr {
// Recognition runs on at most this many threads, each with its own engine
// (#1812: six engines on six cores read 4 MP screens in 1.09 s against 1.78 s
// on four). The memory budget still decides how many engines start.
inline constexpr int maximumWorkerThreads=8;
// The threads a job uses: requested, or one per CPU the process may use (its
// affinity, lowered to its cgroup's cpu.max quota), at most
// maximumWorkerThreads. Count before confinement: Landlock hides the quota.
int workerThreads(int requested=0);

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
 // Recognition stops when this much time has passed since the job started and
 // returns what it has, truncated; the parent's deadline and RLIMIT_CPU stay
 // behind it. 0: no budget (method comparisons).
 int budgetMs=0;
 QString dumpBand;          // method comparisons only: write the first band here
 // Threads for recognition (0: one per core, at most maximumWorkerThreads).
 // Bands run in parallel; a single pass of at least parallelPixels (scaled) is
 // cut into strips at blank row gaps that run in parallel. Results join in
 // page order, by task index.
 int threads=0;
 // The CPUs the job's threads run on (preferredCpus(), #1929); empty: any.
 // The calling thread's own affinity is restored when the job returns.
 QList<int> cpus;
 qint64 parallelPixels=500000;
 // A single pass is cut into about this many strips a thread. Smaller
 // strips use less memory (a dense 1 MP terminal at 2x: four engines peaked at
 // 133 MiB on 8 strips, 123 on 16), but Tesseract reads a strip of three or
 // four lines less well than a longer one (editor code 1.27% whole, 1.48% in
 // 8 strips, 1.55% in 16; Tesseract 5.3.4 garbled a chat line in 16).
 int stripsPerThread=2;
 // The job's resident memory budget. An extra engine starts, and an extra
 // worker takes a task, only while the resident set plus what one engine or
 // one task has added so far stays under it; the first worker always goes on.
 // 0: no gate (method comparisons). It grows by the language data beyond
 // English, up to memoryCeiling.
 qint64 memoryBudget=0;
 bool panels=true;          // method comparisons only: false reads the image as one panel
 qint64 memoryCeiling=0;
 // Emoji (#1748): the input's colour plane and the templates to name its
 // colour glyphs by. Without either, no emoji are looked for.
 ColourView colour;
 const EmojiTemplates* emojiTemplates=nullptr;
 EmojiOptions emoji;
 // The emoji scan stops this long after the job starts and keeps what it found.
 int emojiBudgetMs=3000;
};
// emojiMs is the whole scan; emojiWaitMs what the job waited for it beyond
// recognition (all of it when it ran inline).
struct JobTimings { qint64 codesMs=0,engineStartMs=0,recognitionMs=0,emojiMs=0,emojiWaitMs=0,totalMs=0; };
struct JobResult {
 QString error;             // input_excessive, language_unavailable, engine_unavailable
 QString failedLanguage;
 Recognition recognition;
 QString text;
 QList<int> lineTops;       // one per line of text, source pixels
 bool truncated=false;
 QStringList used,skipped,dataFiles;
 JobTimings timings;
 int emoji=0,emojiCandidates=0;   // emoji placed, colour glyphs examined
 bool emojiCut=false;
 Profile profile;
};
JobResult recognizeImage(const GreyView& image,const JobOptions& options);
// The contract's result: text with its line tops, the filtered words when
// asked, every word as candidates when asked, codes and timings. The
// contract's encoder applies the bounds.
TextResult toTextResult(const JobResult& result,const JobOptions& options);
}
