#pragma once
#include "core/Error.h"
#include "core/RecordingModel.h"
#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QStringList>
#include <QRect>
#include <QWidget>
#include <functional>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QTimer;

namespace putgd::record {
// How the helper's own surfaces reach the screen. X11 windows bypass the window
// manager and use root pixels; KDE Wayland places layer-shell surfaces at
// logical positions; elsewhere on Wayland the compositor (or the desktop shell,
// through the resident) decides, and the helper shows only the bar.
enum class SurfaceMode { X11, LayerShell, Compositor };
SurfaceMode surfaceMode();
// The window title the GNOME Shell extension uses to find and place the bar.
inline constexpr const char* controlBarTitle = "put.gd recording controls";

// Drag-to-select over every screen. Reports the selection in X11 root pixels;
// Escape or a right click cancels. Surfaces are released before reporting.
class SelectionOverlay final : public QObject {
    Q_OBJECT
public:
    explicit SelectionOverlay(QObject* parent = nullptr);
    ~SelectionOverlay() override;
    void start();
    void cancel(Error error = {"cancelled", "Region selection cancelled", {}});
    // Completes the selection with root pixels, as a drag release would.
    void complete(const QRect& rootRect);
signals:
    void selected(const QRect& rootRect);
    void cancelled(const putgd::Error&);
private:
    class Surface;
    friend class Surface;
    void press(QPointF);
    void move(QPointF);
    void release(QPointF);
    void finish();
    QList<Surface*> surfaces_;
    QRectF selection_;
    QPointF anchor_;
    bool dragging_ = false, finished_ = false;
};

// The ready bounds (a ring just outside the recorded rectangle) and, while
// recording, four dim strips covering everything outside it. Every window is
// transparent for input, bypasses the window manager (X11) or is an overlay
// layer surface (KDE Wayland) and stays on top; none of them overlaps the
// recorded pixels, so the capture never sees them. Rectangles are X11 root
// pixels or, on Wayland, logical desktop pixels.
class BoundsOverlay final : public QObject {
    Q_OBJECT
public:
    explicit BoundsOverlay(QObject* parent = nullptr);
    ~BoundsOverlay() override;
    void showBounds(const QRect& rootRect);
    void setDimmed(bool dimmed);
    void hideAll();
    QRect rect() const { return rect_; }
private:
    void add(QList<QWidget*>& into, const QRect& geometry, const QColor& color);
    QRect rect_;
    QList<QWidget*> ring_, dim_;
    bool dimmed_ = false;
};

// The floating controls: state dot and retained time, target and output,
// system-audio and format choices before Record, and Record / Pause / Resume /
// Stop / Cancel. It is placed outside the recorded rectangle when it fits
// (placeBar) and placed again whenever its size changes, so it never leaves its
// screen. When it has to sit inside, it collapses to a handle (state dot,
// Resume while paused, Stop) from Record on. On X11 it can be dragged; a bar
// outside the rectangle cannot be dragged into it.
class ControlBar final : public QWidget {
    Q_OBJECT
public:
    struct Options { QString targetLabel; QStringList formats; QJsonObject codecs; bool audioSupported = false; };
    explicit ControlBar(const Options&, const RecordingConfig&, QWidget* parent = nullptr);
    // X11: positions the bar around rootRect (root pixels) on any screen;
    // returns false when it had to sit inside it.
    bool placeAround(const QRect& rootRect);
    // KDE Wayland: makes the bar an overlay layer surface below or above the
    // logical rectangle, else on another screen, else inside its corner. Call
    // before the first show; returns whether it stays outside the rectangle.
    bool placeOnLayer(const QRect& logical);
    // Collapse to the handle while recording, paused or stopping.
    void setHandleWhileRecording(bool handle) { handle_ = handle; refresh(); }
    bool handleWhileRecording() const { return handle_; }
    bool outside() const { return outside_; }
    // The recorded rectangle in Qt logical pixels, as placed.
    QRect recordedArea() const { return recorded_; }
    void setState(const QString& state);  // ready | recording | paused | stopping
    void setRetained(qint64 us);
    RecordingConfig config() const { return config_; }
signals:
    void recordRequested();
    void pauseRequested();
    void resumeRequested();
    void stopRequested();
    void cancelRequested();
    void configChanged(const RecordingConfig&);
protected:
    void keyPressEvent(QKeyEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    enum class Placed { None, Root, Layer };
    void refresh();
    // Places the bar again for its current size: by the placement rules, or,
    // after a drag, kept by its right edge on the screen it was dropped on.
    void reposition();
    void apply(const QRect& logical, int screen);
    // The largest size the full bar takes in any state; placement decides with it.
    QSize widestSize();
    RecordingConfig config_;
    Options options_;
    QString state_ = "ready";
    Placed placed_ = Placed::None;
    QRect recorded_, spot_;
    int screen_ = -1, layerScreen_ = -1;
    bool outside_ = false, handle_ = false, dragged_ = false, dragging_ = false;
    QPoint dragOffset_;
    QLabel *dot_ = nullptr, *time_ = nullptr, *target_ = nullptr;
    QCheckBox* audio_ = nullptr;
    QComboBox* format_ = nullptr;
    QPushButton *record_ = nullptr, *pause_ = nullptr, *resume_ = nullptr, *stop_ = nullptr, *cancel_ = nullptr;
};
QString formatLabel(const QString& format, const QString& codec);
// Why to pick this output; shown as the combo item tooltip and in the review status.
QString formatHint(const QString& format, const QString& codec);
}
