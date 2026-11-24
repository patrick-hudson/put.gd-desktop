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
 // tesseract::PageSegMode. 6, one uniform block of text: on the real capture
 // sets, 3 (automatic layout) read aligned terminal and page text as columns
 // and lost 30 to 56% of it; 6 lost nothing there and tied 3 elsewhere.
 int pageSegmentation=6;
 int thresholding=0;          // thresholding_method: 0 Otsu, 1 Leptonica adaptive Otsu, 2 Sauvola
 bool dictionary=true;        // the word lists (system and frequent words)
 bool invertLines=true;       // Tesseract's own retry of a line as inverted
 int resolution=0;            // source resolution in DPI given to Tesseract; 0 picks from the scale
};
// One band's words and lines, in Tesseract's reading order, in the band's pixels.
struct BandText {
 // symbols are the word's characters as Tesseract gives them, each with its
 // own text in `characters`; mergeBand keeps them only when they spell the
 // word once both are cleaned.
 struct Item { QString text;QRect rect;float confidence=0;int line=0;QList<Symbol> symbols={};QStringList characters={}; };
 struct LineBox { QRect rect;int block=0;bool rightToLeft=false; };
 QList<LineBox> lines;
 QList<Item> words;
};

// The language data of one job. Tesseract's FileReader is a plain function,
// so the data lives in one object for the job's lifetime, shared by every
// engine instance and safe across threads. Each identifier resolves on its own
// (the first directory holding it, then the next one when that file is not
// valid data). Every file Tesseract asks for, including those a data file pulls
// in by itself, counts against maximumLanguages and maximumLanguageBytes. A file
// is read from disk once and kept in memory, so a second engine starts without
// touching the disk. Only the entries the LSTM engine reads are kept and handed
// to Tesseract, so a model that also carries the legacy engine costs no more.
class LanguageData final {
public:
 explicit LanguageData(const QStringList& directories);
 ~LanguageData();
 LanguageData(const LanguageData&)=delete;
 LanguageData& operator=(const LanguageData&)=delete;
 // Data files read, as name, size, SHA-256, for quality receipts.
 QStringList files()const;
 // The bytes handed to Tesseract for every data file other than English's
 // (its LSTM entries): what the job's memory budget grows by.
 qint64 bytesBeyondEnglish()const;
 // The bytes handed to Tesseract for every data file: what a second engine
 // shares with the first rather than reading again.
 qint64 bytesHanded()const;
 // Frees the kept file contents once no engine will start any more.
 void release();
 // #1981: whether a directory holds a regular, non-empty data file for this
 // identifier (or osd). It is validated only when an engine loads it.
 bool available(const QString& identifier)const;
 // #1981: the script check's osd and language files count apart from the
 // job's own: up to two more files, within the same byte limit.
 void allowDetection();
};

// #1981: Tesseract's orientation and script detection, the legacy engine on
// osd.traineddata. Only Tesseract builds with the legacy engine have it; on
// others start fails and no script is ever detected.
class ScriptDetector final {
public:
 ScriptDetector();
 ~ScriptDetector();
 ScriptDetector(const ScriptDetector&)=delete;
 ScriptDetector& operator=(const ScriptDetector&)=delete;
 bool start();
 // The script of the text in the image (OSD's name, such as Han or Cyrillic)
 // and its confidence; empty when it found none.
 QString detect(const GreyImage& image,float* confidence=nullptr);
 // Frees the engine.
 void stop();
private:
 std::unique_ptr<tesseract::TessBaseAPI> api_;
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
 // #1981: an engine of other languages for the same job, after the first
 // pass: the data already read stays (start() would forget it). strict.
 QString startAlso(const QStringList& languages,const EngineOptions& options);
 QStringList used()const{return used_;}
 bool ready()const{return api_!=nullptr;}  // started, and not failed to
 QStringList skipped()const{return skipped_;}
 // Stops at the deadline (milliseconds from now; 0 means none) and returns the
 // words read until then, in reading order, with cutShort set. failed is set
 // when Tesseract failed, which is not the same as a band with no text.
 BandText recognize(const GreyImage& band,double scale,int deadlineMs=0,bool* cutShort=nullptr,bool* failed=nullptr);
 static QString version();
private:
 void prepare(const GreyImage& band,double scale);
 std::unique_ptr<tesseract::TessBaseAPI> api_;
 EngineOptions options_;
 QStringList used_,skipped_;
};
}
