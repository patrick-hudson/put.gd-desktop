#pragma once
#include "EditDocument.h"
#include <QHash>
#include <QPointer>
#include <QWidget>
#include <QLineEdit>
#include <optional>

namespace putgd::editor {
enum class Tool {Select,Redact,Blur,Crop,Arrow,Rectangle,Highlight,Text};
struct ToolStyle {QColor color=QColor(220,38,38);int width=4;int strength=12;int fontPixels=28;QString fontFamily;};

// Interactive view of one EditDocument. The base raster is painted directly
// from the document; only bounded display-resolution blur tiles are cached.
class EditorCanvas final:public QWidget {
 Q_OBJECT
public:
 static constexpr qint64 blurCacheBytes=64*1024*1024;
 explicit EditorCanvas(EditDocument&,QWidget* parent=nullptr);
 void setTool(Tool);
 Tool tool() const {return tool_;}
 // Sets the style of new shapes only; switching tools never edits a shape.
 void setStyle(const ToolStyle&);
 // Applies the current style to the selected shape as one undoable step.
 void applyStyleToSelection();
 const ToolStyle& style() const {return style_;}
 double zoom() const {return scale_;}
 void fit();
 void actualSize();
 void zoomBy(double factor,QPointF anchor={-1,-1});
 bool applyCrop();
 void cancelCrop();
 bool cropPending() const {return tool_==Tool::Crop;}
 bool editingText() const {return textEditor_!=nullptr;}
 void finishText(bool commit);
 // Documents can change outside the canvas (undo/redo); drop stale UI state.
 void documentChanged();
 quint64 selected() const {return selected_;}
 // Keyboard and agent selection: one shape (0 clears), or the next/previous
 // shape in paint order. Without wrap, stepping past either end returns false.
 void select(quint64 id);
 // Delete, Remove mark and editor.view share this undoable operation.
 bool removeSelected();
 bool selectAdjacent(int step,bool wrap);
 // The current drawing tool's mark, centred in the visible image at a quarter
 // of its area (Insert); Text opens a label there. Returns the new shape's id,
 // or 0 when the tool places nothing (Select, Crop) or Text opened an editor.
 quint64 placeMark();
 // The box the geometry fields edit: the pending crop, or the selected mark's
 // rect (an arrow's tail and head as [from, to - from]). Empty when neither.
 std::optional<QRect> selectionGeometry() const;
 // One undoable document step (the crop stays a draft until applied). Text
 // keeps its own size; an arrow takes its tail at x/y and head at x+w/y+h.
 bool setSelectionGeometry(const QRect&);
 QRect cropDraft() const {return cropDraft_;}
 bool fitted() const {return fitted_;}
 // The image area currently shown, in source pixels.
 QRect visibleImageRect() const;
 void setZoom(double scale);
 void panBy(const QPointF& canvasPixels);
 // What a screen reader hears about the canvas: the selection or crop and
 // the keys that act on it.
 QString selectionSummary() const;
 qint64 cachedBlurBytes() const {return cacheBytes_;}
signals:
 void changed();
 void viewChanged();
 void selectionChanged();
 void toolRequested(putgd::editor::Tool);
protected:
 void paintEvent(QPaintEvent*) override;
 void resizeEvent(QResizeEvent*) override;
 void mousePressEvent(QMouseEvent*) override;
 void mouseMoveEvent(QMouseEvent*) override;
 void mouseReleaseEvent(QMouseEvent*) override;
 void mouseDoubleClickEvent(QMouseEvent*) override;
 void wheelEvent(QWheelEvent*) override;
 void keyPressEvent(QKeyEvent*) override;
 void keyReleaseEvent(QKeyEvent*) override;
 bool eventFilter(QObject*,QEvent*) override;
 bool event(QEvent*) override;
private:
 void announce();
 bool nudge(const QPoint& delta,bool resize);
 enum class Drag {None,Create,Move,Resize,Pan,CropNew,CropMove,CropResize};
 QRect visibleBounds() const;
 QPointF toImage(const QPointF&) const;
 QPointF toWidget(const QPointF&) const;
 QRectF toWidget(const QRect&) const;
 QRect boxFrom(const QPointF&,const QPointF&) const;
 int handleAt(const QRect&,const QPointF& widgetPoint) const;
 int arrowHandleAt(const Shape&,const QPointF& widgetPoint) const;
 // The selected shape's handle under the pointer: arrow ends, a box's eight
 // handles, or a text label's four corners (text keeps its proportions).
 int selectionHandleAt(const Shape&,const QPointF& widgetPoint) const;
 Shape scaledText(const Shape& original,int handle,const QPointF& imagePoint) const;
 quint64 shapeAt(const QPointF& imagePoint) const;
 quint64 grabAt(const QPointF& imagePoint) const;
 void updateHoverCursor(const QPointF& widgetPoint,Qt::KeyboardModifiers);
 QRect resized(QRect,int handle,const QPoint& delta) const;
 void clampOffset();
 void beginText(const QPointF& imagePoint,const Shape* existing);
 const QImage& blurTile(const Shape&);
 Shape::Kind kindFor(Tool) const;
 bool drawingTool() const;
 EditDocument& doc_;
 Tool tool_=Tool::Redact;
 ToolStyle style_;
 double scale_=1;QPointF offset_;
 bool fitted_=true,space_=false;
 Drag drag_=Drag::None;int handle_=-1;bool moved_=false;quint64 editOnClick_=0;
 QPointF press_,last_;Shape original_;
 QRect cropDraft_,cropOriginal_;
 quint64 selected_=0;
 struct Tile {QString key;QImage image;quint64 used=0;};
 QHash<quint64,Tile> tiles_;qint64 cacheBytes_=0;quint64 tick_=0;
 QPointer<QLineEdit> textEditor_;QPointF textAnchor_;quint64 textShape_=0;
};
}
