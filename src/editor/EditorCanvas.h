#pragma once
#include "EditDocument.h"
#include <QHash>
#include <QPointer>
#include <QWidget>
#include <QLineEdit>

namespace putgd::editor {
enum class Tool {Select,Redact,Blur,Crop,Arrow,Rectangle,Highlight,Text};
struct ToolStyle {QColor color=QColor(220,38,38);int width=4;int strength=12;int fontPixels=28;};

// Interactive view of one EditDocument. The base raster is painted directly
// from the document; only bounded display-resolution blur tiles are cached.
class EditorCanvas final:public QWidget {
 Q_OBJECT
public:
 static constexpr qint64 blurCacheBytes=64*1024*1024;
 explicit EditorCanvas(EditDocument&,QWidget* parent=nullptr);
 void setTool(Tool);
 Tool tool() const {return tool_;}
 void setStyle(const ToolStyle&);
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
private:
 enum class Drag {None,Create,Move,Resize,Pan,CropNew,CropMove,CropResize};
 QRect visibleBounds() const;
 QPointF toImage(const QPointF&) const;
 QPointF toWidget(const QPointF&) const;
 QRectF toWidget(const QRect&) const;
 QRect boxFrom(const QPointF&,const QPointF&) const;
 int handleAt(const QRect&,const QPointF& widgetPoint) const;
 int arrowHandleAt(const Shape&,const QPointF& widgetPoint) const;
 quint64 shapeAt(const QPointF& imagePoint) const;
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
 Drag drag_=Drag::None;int handle_=-1;
 QPointF press_,last_;Shape original_;
 QRect cropDraft_,cropOriginal_;
 quint64 selected_=0;
 struct Tile {QString key;QImage image;quint64 used=0;};
 QHash<quint64,Tile> tiles_;qint64 cacheBytes_=0;quint64 tick_=0;
 QPointer<QLineEdit> textEditor_;QPointF textAnchor_;quint64 textShape_=0;
};
}
