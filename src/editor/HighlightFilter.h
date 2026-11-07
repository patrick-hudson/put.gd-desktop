#pragma once
#include <QColor>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QRect>
#include <cstdint>
#include <vector>

// The Highlight mark as a pixel filter: marker paper. Inside the box the page
// becomes a pale band of the marker colour and every pixel keeps the contrast
// it had with its page, now as dark lettering on that band. The rule depends
// only on the page's luminance, the marker and each pixel's own luminance, so
// anti-aliased edges stay smooth, LCD subpixel fringes collapse to neutral and
// no glyph gets heavier. Boxes with no dominant background (photos, gradients)
// get a light wash of the marker instead. Deterministic arithmetic on source
// pixels, so the export and the canvas preview agree, and the same background
// estimate drives text snapping.
namespace putgd::editor::highlight {
constexpr int blockSize=16;          // background estimate grid, source pixels
constexpr int inkDistance=70;        // sRGB distance at which a pixel is all ink (snapping mask)
constexpr double darkPage=0.3;       // background luminance below which the page is dark
constexpr double paperLuminance=0.55;// dark pages: the band is the marker mixed with white up to this
constexpr int whiteMarker=240;       // markers with every channel at or above this are white
constexpr int blackMarker=32;        // markers with every channel at or below this are black
constexpr int transferSteps=4095;    // pixel luminance resolution of the transfer tables
constexpr qint64 maximumSnapPixels=2*1024*1024;

struct Plan {
 enum class Mode {Paper,Wash};
 Mode mode=Mode::Wash;
 QRect area;                              // source-pixel box the plan describes
 int columns=0,rows=0;                    // block grid over `area`
 std::vector<int> blockBackground;        // per block: index into backgrounds
 std::vector<QRgb> backgrounds,paper;     // distinct backgrounds and the band each becomes
 std::vector<std::vector<QRgb>> transfer; // per background: pixel luminance (0..transferSteps) to colour
 QRgb fallback=0;                         // the marker, for the wash
 bool isNull() const {return area.isEmpty();}
};

// WCAG relative luminance of an sRGB pixel.
double luminance(QRgb);
// `color` with its hue and chroma kept and its luminance set in linear light.
QRgb atLuminance(const QColor& color,double target);
// The band `marker` leaves on a light or a dark page.
QRgb paperFor(const QColor& marker,bool darkPage);

// Reads `source` inside `area` (pixels inside `redactions` count as black) and
// decides the band and transfer for each background. Large areas are sampled on a grid.
Plan plan(const QImage& source,const QRect& area,const QList<QRect>& redactions,const QColor& marker);
// Applies `plan` to `image`, where `target` shows plan.area at any scale.
void apply(QImage& image,const QRect& target,const Plan&);

// Text snapping on the plan's ink mask. The box comes back unchanged when it
// holds no text or no dominant background, or is larger than maximumSnapPixels.
QRect snapToText(const QImage& source,const QRect& box);
// The word or line under `point`; a null rect when there is none.
QRect wordAt(const QImage& source,const QPoint& point);
QRect lineAt(const QImage& source,const QPoint& point);
}
