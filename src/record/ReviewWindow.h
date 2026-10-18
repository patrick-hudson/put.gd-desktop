#pragma once
#include "core/RecordingModel.h"
#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QRect>
#include <QTimer>
#include <QWidget>
#include <atomic>
#include <functional>
#include <memory>

class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QThread;

namespace putgd::record {
// Paints the preview frame scaled to fit and lets the user create, select,
// move and resize mask rectangles in canvas pixels.
class FrameView final : public QWidget {
    Q_OBJECT
public:
    explicit FrameView(QWidget* parent = nullptr);
    void setFrame(const QImage&, qint64 timeUs);
    void setCanvas(const QSize& canvas) { canvas_ = canvas; update(); }
    void setMasks(const QList<RecordingMask>& masks, const QString& selected);
    void armCreate(const QString& kind) { armed_ = kind; setCursor(Qt::CrossCursor); }
    QSize sizeHint() const override { return {640, 360}; }
signals:
    void maskCreated(const QString& kind, const QRect& rect);
    void maskMoved(const QString& id, const QRect& rect);
    void maskSelected(const QString& id);
protected:
    void paintEvent(QPaintEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override;
private:
    QRect imageRect() const;
    QPointF toCanvas(const QPointF&) const;
    QRectF toView(const QRect&) const;
    QImage frame_;
    qint64 timeUs_ = 0;
    QSize canvas_;
    QList<RecordingMask> masks_;
    QString selected_, armed_;
    enum class Drag { None, Create, Move, Resize } drag_ = Drag::None;
    QPointF anchor_, current_;
    QRect original_;
};

// The private review: scrub, trim, timed masks, then Upload or Save copy.
// Edits are proposed to the resident as events and the resident's answer
// (setEdit) is what this window displays; nothing uploads from here. Upload
// closes the window and the work continues in the background; Save copy
// keeps it open so the user can still upload afterwards.
class ReviewWindow final : public QWidget {
    Q_OBJECT
public:
    struct Options { QString workingClip; qint64 durationUs = 0; QSize canvas; QStringList formats; QJsonObject codecs; bool audioRecorded = false; };
    ReviewWindow(const Options&, const RecordingConfig&, QWidget* parent = nullptr);
    void setEdit(const RecordingEdit&);
    void setConfig(const RecordingConfig&);
    void setExporting(bool exporting, const QString& label);
    void showError(const QString& message);
signals:
    void editProposed(quint64 revision, const QJsonArray& commands);
    void configChanged(const RecordingConfig&);
    void uploadRequested(quint64 revision);
    void saveCopyRequested(const QString& path, quint64 revision);
    void cancelRequested();
protected:
    void closeEvent(QCloseEvent*) override;
private:
    void refreshFrame();
    // Silent playback of the retained interval from the scrubber: frames are
    // decoded in order on a worker and shown at their clip time; any scrub or
    // edit control pauses it. The export preview has no audio path.
    void startPlayback();
    void stopPlayback();
    void refreshMaskList();
    void refreshLabels();
    void propose(const QJsonArray& commands);
    RecordingMask* selectedMask();
    QString timeText(qint64 us) const;
    Options options_;
    RecordingConfig config_;
    RecordingEdit edit_;
    FrameView* view_ = nullptr;
    QSlider* scrubber_ = nullptr;
    QLabel *time_ = nullptr, *retained_ = nullptr, *status_ = nullptr, *maskTimes_ = nullptr;
    QPushButton* play_ = nullptr;
    QComboBox* speed_ = nullptr;
    QThread* playThread_ = nullptr;
    std::shared_ptr<std::atomic_bool> playStop_;
    bool playing_ = false;
    QPushButton *trimStart_ = nullptr, *trimEnd_ = nullptr, *resetTrim_ = nullptr, *addBlur_ = nullptr, *addSolid_ = nullptr, *removeMask_ = nullptr, *maskStart_ = nullptr, *maskEnd_ = nullptr, *upload_ = nullptr, *save_ = nullptr, *cancel_ = nullptr;
    QSpinBox* strength_ = nullptr;
    QComboBox* format_ = nullptr;
    QListWidget* masks_ = nullptr;
    QString selectedMask_;
    QTimer frameTimer_;
    bool exporting_ = false, closing_ = false;
public:
    ~ReviewWindow() override;
};
}
