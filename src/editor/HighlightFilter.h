#pragma once
#include <QColor>
#include <QImage>
#include <QList>
#include <QPoint>
#include <QRect>
#include <vector>

// The Highlight mark as a pixel filter. Inside the box, every pixel is read as
// a mix of a background colour and ink; only the background part moves toward
// the marker, so glyph pixels stay byte-identical. Where the background goes
// depends on its luminance alone: light backgrounds take the marker multiplied
// in (the classic look), dark backgrounds take the marker's hue at the highest
// luminance that keeps white text readable at 4.5:1. Boxes with no dominant
// background (photos, gradients) fall back to multiply or a dim screen.
// Everything is integer sRGB arithmetic on source pixels, so the export and
// the canvas preview agree, and the same ink mask drives text snapping.
namespace putgd::editor::highlight {
constexpr int blockSize=16;          // background estimate grid, source pixels
constexpr int inkDistance=70;        // sRGB distance at which a pixel is all ink
constexpr double whiteFloor=0.183;   // highest luminance keeping white at 4.5:1
constexpr double visibleLift=0.06;   // a dark background rises at least this much
constexpr qint64 maximumSnapPixels=2*1024*1024;

struct Plan {
 enum class Mode {Unmix,Multiply,Screen};
 Mode mode=Mode::Multiply;
 QRect area;                           // source-pixel box the plan describes
 int columns=0,rows=0;                 // block grid over `area`
 std::vector<int> blockBackground;     // per block: index into backgrounds
 std::vector<QRgb> backgrounds,tinted; // distinct backgrounds and their marker colours
 QRgb fallback=0;                      // multiply or screen colour for the fallbacks
 bool isNull() const {return area.isEmpty();}
};

// WCAG relative luminance of an sRGB pixel.
double luminance(QRgb);
// `color` with its hue and chroma kept and its luminance set in linear light.
QRgb atLuminance(const QColor& color,double target);

// Reads `source` inside `area` (pixels inside `redactions` count as black) and
// decides the marker for each background. Large areas are sampled on a grid.
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
