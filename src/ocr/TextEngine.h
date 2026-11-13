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

// Tesseract 5 through its C++ API, LSTM engine only, one instance for a job.
// Language data is read through a FileReader that resolves each identifier on
// its own (the first directory holding it, then the next one when that file
// is not valid data) and counts every file Tesseract asks for, including those
// a data file pulls in by itself, against maximumLanguages and
// maximumLanguageBytes.
class TextEngine final {
public:
 TextEngine();
 ~TextEngine();
 TextEngine(const TextEngine&)=delete;
 TextEngine& operator=(const TextEngine&)=delete;
 // strict: every language must load, or language_unavailable names the first
 // that did not. Otherwise a language that cannot load is skipped while one
 // other loads. Returns the error code, empty on success.
 QString start(const QStringList& languages,bool strict,const QStringList& directories,const EngineOptions& options,QString& failedLanguage);
 QStringList used()const{return used_;}
 QStringList skipped()const{return skipped_;}
 // Data files read, as name, size, SHA-256, for quality receipts.
 QStringList dataFiles()const;
 BandText recognize(const GreyImage& band,double scale);
 static QString version();
private:
 std::unique_ptr<tesseract::TessBaseAPI> api_;
 EngineOptions options_;
 QStringList used_,skipped_;
};
}
