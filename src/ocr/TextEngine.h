#pragma once
#include "OcrImage.h"
#include "core/TextJobProtocol.h"
#include <QList>
#include <QString>
#include <QStringList>
#include <memory>

namespace tesseract { class TessBaseAPI; }
namespace putgd::ocr {
using textjob::maximumLanguages;
inline constexpr qint64 maximumLanguageBytes=textjob::maximumLanguageDataBytes;
using textjob::validLanguage;

struct EngineOptions {
 int pageSegmentation=3;      // tesseract::PageSegMode; 3 is automatic layout
 int thresholding=0;          // thresholding_method: 0 Otsu, 1 Leptonica adaptive Otsu, 2 Sauvola
 bool dictionary=true;        // the word lists (system and frequent words)
 bool invertLines=true;       // Tesseract's own retry of a line as inverted
 int resolution=0;            // source resolution in DPI given to Tesseract; 0 picks from the scale
};
// One band's words and lines, in Tesseract's reading order, in the band's pixels.
struct BandText {
 struct Item { QString text;QRect rect;float confidence=0;int line=0; };
 struct LineBox { QRect rect;int block=0; };
 QList<LineBox> lines;
 QList<Item> words;
};
// The parts of one band recognized separately (its layout blocks), joined in
// the order given: every line once, block numbers and line indexes renumbered.
BandText joinParts(const QList<BandText>& parts);

// The language data of one job. Tesseract's FileReader is a plain function,
// so the data lives in one object for the job's lifetime, shared by every
// engine instance and safe across threads. Each identifier resolves on its own
// (the first directory holding it, then the next one when that file is not
// valid data). Every file Tesseract asks for, including those a data file pulls
// in by itself, counts against maximumLanguages and maximumLanguageBytes. A file
// is read from disk once and kept in memory, so a second engine starts without
// touching the disk.
class LanguageData final {
public:
 explicit LanguageData(const QStringList& directories);
 ~LanguageData();
 LanguageData(const LanguageData&)=delete;
 LanguageData& operator=(const LanguageData&)=delete;
 // Data files read, as name, size, SHA-256, for quality receipts.
 QStringList files()const;
 // Frees the kept file contents once no engine will start any more.
 void release();
};

// Tesseract 5 through its C++ API, LSTM engine only.
class TextEngine final {
public:
 TextEngine();
 ~TextEngine();
 TextEngine(const TextEngine&)=delete;
 TextEngine& operator=(const TextEngine&)=delete;
 // strict: every language must load, or language_unavailable names the first
 // that did not. Otherwise a language that cannot load is skipped while one
 // other loads. Returns the error code, empty on success. A LanguageData must
 // exist for the whole job.
 QString start(const QStringList& languages,bool strict,const EngineOptions& options,QString& failedLanguage);
 // Another instance for the same job: exactly the languages the first loaded.
 QString startLike(const TextEngine& first);
 QStringList used()const{return used_;}
 QStringList skipped()const{return skipped_;}
 // Tesseract's layout of the band: its text blocks in reading order, in the
 // band's pixels. Nothing is recognized.
 QList<QRect> layoutBlocks(const GreyImage& band,double scale);
 // Stops at the deadline (milliseconds from now; 0 means none) and returns the
 // words read until then, in reading order, with cutShort set. With a
 // rectangle, only that part of the band is laid out and recognized; boxes stay
 // in the band's pixels.
 BandText recognize(const GreyImage& band,double scale,int deadlineMs=0,bool* cutShort=nullptr,const QRect& rectangle={});
 static QString version();
private:
 void prepare(const GreyImage& band,double scale);
 std::unique_ptr<tesseract::TessBaseAPI> api_;
 EngineOptions options_;
 QStringList used_,skipped_;
};
}
