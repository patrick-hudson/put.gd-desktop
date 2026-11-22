#pragma once
#include "core/CaptureStyle.h"
#include <QColor>
#include <algorithm>
#include <QFont>
#include <QImage>
#include <QHash>
#include <QPainterPath>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>
#include <functional>

namespace putgd::editor {
// One editable mark. All geometry is in source-image pixels, independent of
// zoom, so redaction maps exactly through crop and display scaling.
struct Shape {
 enum class Kind {Redact,Blur,Arrow,Rectangle,Text,Highlight,Step,Pen,Ellipse,Line};
 Kind kind=Kind::Rectangle;
 quint64 id=0;
 // Quick redact (#1618): the category that suggested this mark (name, list,
 // email, ...), empty for a mark drawn by hand, and the recognized words' own
 // box it was suggested for (unpadded, one line), which moving or resizing
 // leaves alone. Never the recognized text.
 QString suggested;
 QRect suggestedAt;
 QRect rect;          // normalized box: Redact, Blur, Rectangle, Highlight, Text, Ellipse; a Step's square
 QPoint from,to;      // Arrow and Line ends (an arrow's head is at `to`)
 QColor color=QColor(220,38,38);
 int width=4;         // outline/line/pen width in image pixels
 int strength=12;     // Blur radius, or a pixelation's block size, in image pixels
 bool pixelate=false; // Blur drawn as square blocks of `strength` pixels
 int fontPixels=28;   // Text height in image pixels
 int number=1;        // Step label
 QString fontFamily;  // Empty means the default sans-serif family
 QString text;
 QList<QPoint> points; // Pen stroke, in drawing order
 QRect bounds() const;
 // Line-like marks: grabbed by their stroke, edited by their two ends.
 bool twoPoint() const {return kind==Kind::Arrow||kind==Kind::Line;}
 // The box that geometry fields and handles edit (a pen stroke's extent).
 QRect box() const;
 bool operator==(const Shape&) const=default;
};

// Bold text font shared by the canvas, export and agent commands.
QFont textFont(const QString& family,int pixels);
// Exact glyph outline plus the contrast stroke used by export, in image pixels.
QRectF textOutputBounds(const Shape&);
constexpr int minimumFontPixels=8,maximumFontPixels=200,maximumFontFamilyCharacters=100;
// Step circles: diameter in image pixels and the label range.
constexpr int minimumStepPixels=16,defaultStepPixels=40,maximumStepPixels=256,maximumStepNumber=999;
// A step's centre in image pixels; a step at `at` with diameter d has its
// square at at - d/2, so the centre round-trips for every size.
inline QPoint stepCentre(const QRect& square){return square.topLeft()+QPoint(square.width()/2,square.height()/2);}
// A pen stroke keeps at most this many points; drawing smooths and thins them.
constexpr int maximumPenPoints=2000;
// The step label's outline, centred on the circle, in image pixels.
QPainterPath stepLabelPath(const Shape&);
// Light smoothing for a hand-drawn stroke: a moving average, then points
// closer than half a pixel to the simplified line are dropped.
QList<QPoint> smoothStroke(const QList<QPoint>&);
// At most maximumPenPoints, thinning the interior and keeping both ends.
QList<QPoint> limitStroke(QList<QPoint>);
// Scales a pen stroke from its current extent onto `box`.
QList<QPoint> fitStroke(const QList<QPoint>&,const QRect& from,const QRect& box);
// The pen path through a stroke's points (quadratic segments through midpoints).
QPainterPath strokePath(const QList<QPoint>&);

// Undo history stores geometry snapshots, never pixel copies. The bounded base
// image is immutable for the session; export renders it once at finalization.
class EditDocument {
public:
 static constexpr int maximumHistory=200;
 static constexpr qint64 maximumHistoryBytes=4*1024*1024;
 static constexpr int maximumTextCharacters=500;
 static constexpr int minimumBlur=2,maximumBlur=64;
 // Normalizes to a fresh 32-bit raster with no text, color space or other
 // reader metadata. Callers must already have admitted the pixel count.
 explicit EditDocument(const QImage& base);
 const QImage& base() const {return base_;}
 // Axis-aligned display sampling, with sanitized edge patches and no retained source copy.
 void paintPreviewSource(QPainter&,const QRectF& target,const QRectF& source,
                         const std::function<void(QPainter&,const QRectF&)>& backdrop={},const QRect& samplingBounds={}) const;
 QImage previewSource(const QRect& area,const QSize& displaySize) const;
 QSize size() const {return base_.size();}
 QRect imageRect() const {return base_.rect();}
 QRect crop() const {return state_.crop;}
 const QList<Shape>& shapes() const {return state_.shapes;}
 const Shape* find(quint64 id) const;
 bool modified() const {return state_.shapes.size()>0||state_.crop!=base_.rect()||state_.style!=initialStyle_;}
 // The style the editor opened with (a new capture with Capture style on), which is not an edit.
 // Redact and upload sets it after Quick redact's steps: every history state gets it, so undo keeps it.
 void setInitialStyle(const CaptureStyle& style){state_.style=style;initialStyle_=style;for(auto& state:undo_)state.style=style;for(auto& state:redo_)state.style=style;++generation_;}
 // Bumped by every change to what export renders (edits, live drags, undo,
 // redo), so a saved copy can tell whether it still matches the draft.
 quint64 generation() const {return generation_;}

 quint64 add(Shape);
 bool replace(const Shape&);  // same id; returns false when unknown or unchanged
 // Removing a suggested mark remembers where it was suggested, in this
 // document's history only: undoing the removal forgets it again.
 bool remove(quint64 id);
 // Every suggested mark that is still there, as one undo step, without
 // remembering any as dismissed. Marks drawn by hand stay. Returns how many.
 int removeSuggested();
 // Only those suggested by these categories (#1820: the warn kinds before a covered upload).
 int removeSuggested(const QStringList& categories);
 // True when the user removed a mark of this category suggested for about this
 // words box (Shape::suggestedAt):
 // a later run reads the page with the other marks blacked out, so its boxes move
 // a few pixels and shrink a little. Two boxes are the same suggestion when their
 // intersection is at least half of their union. Quick redact skips such a mark.
 bool dismissed(const QRect& suggestedAt,const QString& category) const;
 bool setCrop(const QRect&);
 // #1893: the capture style export draws last, around the cropped result. Undoable like any
 // edit; enabled false means none. The picture of a picture backdrop is read once per path.
 static constexpr qint64 maximumStyledPixels=24000000; // the editor's output admission
 const CaptureStyle& captureStyle() const {return state_.style;}
 // The configured image limits, when lower than the output admission: padding steps down to fit them.
 void setStyleLimit(qint64 pixels){styleLimit_=std::clamp<qint64>(pixels,1,maximumStyledPixels);++generation_;}
 bool setCaptureStyle(const CaptureStyle&);
 // The style's geometry for the current crop; invalid when the style is off or can't fit.
 CaptureStyle::Geometry styleGeometry() const;
 // The exported image's size: the crop, or the styled canvas around it.
 QSize exportSize() const;
 // The backdrop picture for the current style, loaded on first use; null when it can't be read
 // (export then draws Graphite, as the capture path does). Call on the GUI thread.
 const QImage& stylePicture() const;
 // Live drags update without a history entry; commit once on release.
 void preview(const Shape&);
 void beginGesture();
 // True between beginGesture and commitGesture or cancelGesture: a drag is still in progress.
 bool inGesture() const {return gesture_;}
 void commitGesture();
 void cancelGesture();

 bool canUndo() const {return !undo_.isEmpty();}
 bool canRedo() const {return !redo_.isEmpty();}
 bool undo();
 bool redo();
 bool historyTruncated() const {return truncated_;}

 // Resolve text on the calling (GUI) thread before a worker renders a copy.
 EditDocument snapshotForExport() const;

 // Full-resolution sanitized result: redaction overwrites source pixels first,
 // blur reads only redacted pixels, annotations are drawn, then crop applies.
 // Fully transparent pixels have their hidden color cleared.
 QImage render() const;
 // PNG with no text, ICC or EXIF chunks. Empty on encoder failure.
 QByteArray exportPng() const;
 // Display-resolution blur for one shape, computed from the redacted base.
 QImage blurPreview(const Shape&,double scale) const;
 // Display-resolution highlight for one shape: the redacted, blurred base
 // under the box with the marker filter applied. Its colours are decided on
 // full-resolution pixels, so the preview matches the export at any zoom.
 // The source pixels highlightPreview reads for `shape`: its box and the reach of blurs crossing its edge.
 QRect highlightWindow(const Shape& shape) const;
 QImage highlightPreview(const Shape&,double scale) const;
 QList<QRect> redactionRects() const;
 // One more than the highest step label, or 1 without steps (at most 999).
 int nextStepNumber() const;

 static QRect clampRect(const QRect&,const QRect& bounds);
 static void blurInPlace(QImage&,const QRect&,int radius);
 // Replaces each block of `block` pixels, on a grid anchored at `origin`, with
 // the average of its pixels inside `area`; pixels outside `area` are untouched.
 static void pixelateInPlace(QImage&,const QRect& area,const QPoint& origin,int block);
 static void paintAnnotation(class QPainter&,const Shape&,const QPainterPath* textPath=nullptr);
private:
 struct Dismissed {QRect at;QString category;bool operator==(const Dismissed&) const=default;};
 struct State {QList<Shape> shapes;QRect crop;QList<Dismissed> dismissed;CaptureStyle style;bool operator==(const State&) const=default;};
 static qint64 bytesOf(const State&);
 void push(State before);
 void applyRedactions(QImage&,const QRect& within) const;
 // A pixelation at display scale: block colours from the redacted base, drawn
 // into `target` (mapping `window` onto it) wherever the shape covers it.
 void pixelatePreview(QImage& target,const QRect& window,const Shape&) const;
 QImage base_;
 CaptureStyle initialStyle_;qint64 styleLimit_=maximumStyledPixels;
 State state_,gestureStart_;
 bool gesture_=false,truncated_=false;
 QList<State> undo_,redo_;
 qint64 historyBytes_=0;
 quint64 nextId_=1,generation_=0;
 mutable QString picturePath_;mutable QImage picture_;
 mutable QHash<quint64,QPainterPath> textPaths_;
 mutable quint64 textPathsGeneration_=0;
 mutable QFont textPathsFont_;
 bool textPathsFrozen_=false;
};
}
