#pragma once
#include <QColor>
#include <QFont>
#include <QImage>
#include <QHash>
#include <QPainterPath>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>

namespace putgd::editor {
// One editable mark. All geometry is in source-image pixels, independent of
// zoom, so redaction maps exactly through crop and display scaling.
struct Shape {
 enum class Kind {Redact,Blur,Arrow,Rectangle,Text,Highlight};
 Kind kind=Kind::Rectangle;
 quint64 id=0;
 QRect rect;          // normalized box: Redact, Blur, Rectangle, Highlight, Text
 QPoint from,to;      // Arrow tail and head
 QColor color=QColor(220,38,38);
 int width=4;         // outline/arrow pen width in image pixels
 int strength=12;     // Blur radius in image pixels
 int fontPixels=28;   // Text height in image pixels
 QString fontFamily;  // Empty means the default sans-serif family
 QString text;
 QRect bounds() const;
 bool operator==(const Shape&) const=default;
};

// Bold text font shared by the canvas, export and agent commands.
QFont textFont(const QString& family,int pixels);
constexpr int minimumFontPixels=8,maximumFontPixels=200,maximumFontFamilyCharacters=100;

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
 QSize size() const {return base_.size();}
 QRect imageRect() const {return base_.rect();}
 QRect crop() const {return state_.crop;}
 const QList<Shape>& shapes() const {return state_.shapes;}
 const Shape* find(quint64 id) const;
 bool modified() const {return state_.shapes.size()>0||state_.crop!=base_.rect();}
 // Bumped by every change to what export renders (edits, live drags, undo,
 // redo), so a saved copy can tell whether it still matches the draft.
 quint64 generation() const {return generation_;}

 quint64 add(Shape);
 bool replace(const Shape&);  // same id; returns false when unknown or unchanged
 bool remove(quint64 id);
 bool setCrop(const QRect&);
 // Live drags update without a history entry; commit once on release.
 void preview(const Shape&);
 void beginGesture();
 void commitGesture();
 void cancelGesture();

 bool canUndo() const {return !undo_.isEmpty();}
 bool canRedo() const {return !redo_.isEmpty();}
 bool undo();
 bool redo();
 qint64 historyBytes() const {return historyBytes_;}
 int historyEntries() const {return undo_.size()+redo_.size();}
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

 static QRect clampRect(const QRect&,const QRect& bounds);
 static void blurInPlace(QImage&,const QRect&,int radius);
 static void paintAnnotation(class QPainter&,const Shape&,const QPainterPath* textPath=nullptr);
private:
 struct State {QList<Shape> shapes;QRect crop;bool operator==(const State&) const=default;};
 static qint64 bytesOf(const State&);
 void push(State before);
 void applyRedactions(QImage&,const QRect& within) const;
 QImage base_;
 State state_,gestureStart_;
 bool gesture_=false,truncated_=false;
 QList<State> undo_,redo_;
 qint64 historyBytes_=0;
 quint64 nextId_=1,generation_=0;
 QHash<quint64,QPainterPath> textPaths_;
 quint64 textPathsGeneration_=0;
};
}
