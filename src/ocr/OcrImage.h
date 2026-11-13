#pragma once
#include <QByteArray>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
#include <array>
#include <cstdint>

// The helper's own value types (#1428). Only putgd-ocr and its tests use them;
// nothing here links Tesseract, Leptonica or ZXing.
namespace putgd::ocr {
// One 8-bit grey image: a view of pixels owned elsewhere.
struct GreyView {
 const uint8_t* data=nullptr;
 int width=0,height=0,stride=0;
 bool valid()const{return data&&width>0&&height>0&&stride>=width;}
 const uint8_t* row(int y)const{return data+qsizetype(y)*stride;}
};
// An owned grey image, for band copies.
struct GreyImage {
 QByteArray pixels;int width=0,height=0;
 GreyView view()const{return {reinterpret_cast<const uint8_t*>(pixels.constData()),width,height,width};}
 uint8_t* row(int y){return reinterpret_cast<uint8_t*>(pixels.data())+qsizetype(y)*width;}
};
struct Word {
 QString text;
 QRect rect;          // source pixels
 float confidence=0;  // 0..100, Tesseract's
 int line=0;          // index of its line in reading order
 int lineHeight=0;    // its line's box height in source pixels
 bool kept=true;      // passes the copy filters
 double seamOrder=-1; // while merging a seam: the word's place in the engine's logical order
};
struct Line {
 QRect rect;          // source pixels
 int block=0;         // index of its block; a new block starts a paragraph gap
 int band=0;
};
// A code's corners: top-left, top-right, bottom-right, bottom-left. QtCore only, so
// not QPolygon, which is QtGui.
using Quad=std::array<QPoint,4>;
struct Code {
 QString format;      // QRCode, MicroQRCode, DataMatrix, Aztec
 bool text=false;     // false: binary, only its length is reported
 QString content;     // sanitized, text codes only
 qsizetype length=0;  // bytes of the decoded content
 Quad quad;           // source pixels
 QPoint centre()const{return (quad[0]+quad[1]+quad[2]+quad[3])/4;}
};
struct Recognition {
 QList<Line> lines;
 QList<Word> words;   // reading order; kept marks the copied words
 QList<Code> codes;
 bool banded=false;
 double scale=1;
 int bands=0;
 int parts=0;         // recognition tasks: bands, or a single pass's strips
 int panels=1;        // windows or columns read one after another
 int threads=0;       // engine instances that ran
};
}
