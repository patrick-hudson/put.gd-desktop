#pragma once
#include "OcrImage.h"
#include "core/TextJobProtocol.h"
#include <QList>
#include <QString>
#include <algorithm>

// Scale and band planning, and the band copies Tesseract reads (#1428 decision 3).
// Pure image arithmetic: no engine, no allocation beyond one band at a time.
namespace putgd::ocr {
// Admission: the editor's own limit.
using textjob::maximumInputPixels;
// One pass or one band never exceeds this after scaling, overlap included.
inline constexpr qint64 bandPixelLimit=8000000;

struct Profile {
 int background=255;     // estimated background grey
 bool dark=false;        // light text on a dark background
 int lineHeight=0;       // median height of the ink row runs; 0 when none
 QList<int> rowInk;      // ink pixels per row, code areas excluded
 bool blank(int y,int width)const{return rowInk.at(y)<=std::max(2,width/256);}
};
// Background, polarity, ink per row and the text height, from the whole image.
// Pixels inside the codes' bounding boxes do not count.
Profile measure(const GreyView& image,const QList<Quad>& codes);

struct Band {
 int top=0,bottom=0;     // source rows [top, bottom)
 bool seamAbove=false;   // starts inside the band before it (no gap was found)
};
struct Plan {
 double scale=1;
 QList<Band> bands;      // one band means a single pass
 QString error;          // input_excessive
 bool banded()const{return bands.size()>1;}
};
struct PlanLimits {
 // The whole job's scaled pixels. Measured on m74 (i5-6500T, Tesseract 5.3.4):
 // above this a scale over 1x costs more than the deadline and the speed bars
 // allow (a 4K UI page took 20 s at 1.7x and 3 s at 1x).
 qint64 scaledPixels=8000000;
 qint64 singlePassPixels=bandPixelLimit; // up to this the image is recognized whole
 // A banded image is cut into bands of about this size, or eight line heights
 // when that is more, never over bandPixelLimit. Tesseract's layout time grows
 // faster than a band's size: on m74 a 24 MP page took 17.5 s in 8 MP bands
 // and 8.8 s in 2 MP bands, with a lower peak.
 qint64 bandPixels=2000000;
 int border=0;                  // background pixels added on every side of a band, scaled
 int maximumScale=3;
 double forcedScale=0;           // method comparisons only: this scale, before the cap
};
// The scale chosen from the text height, then limited by the job's scaled-pixel
// cap; and the bands: cuts at the last blank row gap in a band's final quarter,
// otherwise at the limit with the next band starting two line heights earlier.
Plan plan(int width,int height,const Profile& profile,const PlanLimits& limits);
// The integer scale the text height asks for, before the cap.
int preferredScale(int lineHeight,int maximumScale);

// A band of the source, scaled, with every code polygon filled with the
// background, inverted when the page is dark, and with the border added.
GreyImage bandImage(const GreyView& source,const Band& band,double scale,int border,
                    const QList<Quad>& codes,int background,bool invert);
}
