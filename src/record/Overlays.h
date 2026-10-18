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
// Drag-to-select over every screen. Reports the selection in X11 root pixels;
// Escape or a right click cancels. Surfaces are released before reporting.
class SelectionOverlay final : public QObject {
    Q_OBJECT
public:
    explicit SelectionOverlay(QObject* parent = nullptr);
    ~SelectionOverlay() override;
    void start();
    void cancel(Error error = {"cancelled", "Region selection cancelled", {}});
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
// transparent for input, bypasses the window manager and stays on top; none
// of them overlaps the recorded pixels, so the capture never sees them.
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
    QWidget* passive(const QRect& geometry, const QColor& color);
    QRect rect_;
    QList<QWidget*> ring_, dim_;
    bool dimmed_ = false;
};

// The floating controls: state dot and retained time, target and output,
// system-audio and format choices before Record, and Record / Pause / Resume /
// Stop / Cancel. It is placed outside the recorded rectangle when it fits.
class ControlBar final : public QWidget {
    Q_OBJECT
public:
    struct Options { QString targetLabel; QStringList formats; QJsonObject codecs; bool audioSupported = false; };
    explicit ControlBar(const Options&, const RecordingConfig&, QWidget* parent = nullptr);
    // Positions the bar around rootRect; returns false when it had to sit inside it.
    bool placeAround(const QRect& rootRect, const QRect& desktop);
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
private:
    void refresh();
    RecordingConfig config_;
    Options options_;
    QString state_ = "ready";
    QLabel *dot_ = nullptr, *time_ = nullptr, *target_ = nullptr;
    QCheckBox* audio_ = nullptr;
    QComboBox* format_ = nullptr;
    QPushButton *record_ = nullptr, *pause_ = nullptr, *resume_ = nullptr, *stop_ = nullptr, *cancel_ = nullptr;
};
QString formatLabel(const QString& format, const QString& codec);
}
