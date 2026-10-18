#include "ReviewWindow.h"
#include "ClipExporter.h"
#include "DebugLog.h"
#include "Overlays.h"
#include <QCloseEvent>
#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMouseEvent>
#include <QPainter>
#include <QPushButton>
#include <QSlider>
#include <QSpinBox>
#include <QVBoxLayout>
#include <algorithm>

namespace putgd::record {
namespace {
const char* const reviewStyle = R"QSS(
QWidget#putgd-review { background: #15171c; color: #e6e8ee; }
QLabel { color: #e6e8ee; }
QLabel#muted { color: #9aa3b5; }
QLabel#status { color: #f0b429; }
QPushButton { background: #2a2f3a; color: #e6e8ee; border: 1px solid #3a4150; border-radius: 6px; padding: 5px 12px; }
QPushButton:hover { background: #343a47; }
QPushButton:disabled { color: #6f7685; background: #1c1f26; }
QPushButton#upload { background: #2e7d32; border-color: #43a047; color: white; font-weight: 600; }
QSpinBox, QListWidget { background: #22262f; color: #e6e8ee; border: 1px solid #3a4150; border-radius: 6px; padding: 2px 6px; }
QCheckBox { color: #e6e8ee; spacing: 6px; }
QCheckBox:disabled { color: #6f7685; }
QCheckBox::indicator { width: 14px; height: 14px; border: 1px solid #9bbcff; border-radius: 3px; background: #22262f; }
QCheckBox::indicator:checked { background: #9bbcff; }
QCheckBox::indicator:disabled { border-color: #3a4150; background: #1c1f26; }
QComboBox { background: #22262f; color: #e6e8ee; border: 1px solid #3a4150; border-radius: 6px; padding: 2px 8px; }
QComboBox:disabled { color: #6f7685; }
QComboBox::drop-down { border: none; width: 18px; }
QComboBox QAbstractItemView { background: #22262f; color: #e6e8ee; border: 1px solid #3a4150; selection-background-color: #3a4150; selection-color: #ffffff; outline: none; }
QSlider::groove:horizontal { height: 6px; background: #2a2f3a; border-radius: 3px; }
QSlider::handle:horizontal { width: 14px; margin: -5px 0; background: #9bbcff; border-radius: 7px; }
)QSS";
constexpr int handleSize = 8;
}

FrameView::FrameView(QWidget* parent) : QWidget(parent) { setMinimumSize(320, 180); setMouseTracking(true); setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); }
void FrameView::setFrame(const QImage& image, qint64 timeUs) { frame_ = image; timeUs_ = timeUs; update(); }
void FrameView::setMasks(const QList<RecordingMask>& masks, const QString& selected) { masks_ = masks; selected_ = selected; update(); }
QRect FrameView::imageRect() const {
    const QSize source = canvas_.isEmpty() ? frame_.size() : canvas_;
    if (source.isEmpty()) return rect();
    const QSize scaled = source.scaled(size(), Qt::KeepAspectRatio);
    return QRect(QPoint((width() - scaled.width()) / 2, (height() - scaled.height()) / 2), scaled);
}
QPointF FrameView::toCanvas(const QPointF& p) const {
    const auto r = imageRect();
    const QSize source = canvas_.isEmpty() ? frame_.size() : canvas_;
    if (r.width() < 1 || r.height() < 1) return p;
    return QPointF((p.x() - r.x()) * source.width() / r.width(), (p.y() - r.y()) * source.height() / r.height());
}
QRectF FrameView::toView(const QRect& c) const {
    const auto r = imageRect();
    const QSize source = canvas_.isEmpty() ? frame_.size() : canvas_;
    if (source.isEmpty()) return c;
    const qreal sx = qreal(r.width()) / source.width(), sy = qreal(r.height()) / source.height();
    return QRectF(r.x() + c.x() * sx, r.y() + c.y() * sy, c.width() * sx, c.height() * sy);
}
void FrameView::paintEvent(QPaintEvent*) {
    QPainter painter(this);
    painter.fillRect(rect(), QColor("#0d0f13"));
    const auto target = imageRect();
    if (!frame_.isNull()) painter.drawImage(target, frame_);
    for (const auto& mask : masks_) {
        const bool active = mask.start <= timeUs_ && timeUs_ < mask.end;
        const auto r = toView(mask.rect);
        QPen pen(mask.id == selected_ ? QColor("#9bbcff") : active ? QColor(255, 255, 255, 200) : QColor(255, 255, 255, 90), mask.id == selected_ ? 2 : 1, active ? Qt::SolidLine : Qt::DashLine);
        painter.setPen(pen);
        painter.setBrush(mask.kind == "solid" ? QBrush(QColor(0, 0, 0, active ? 0 : 60)) : QBrush(QColor(155, 188, 255, active ? 0 : 30)));
        painter.drawRect(r);
        painter.setPen(Qt::white);
        painter.drawText(r.adjusted(4, 2, -4, -2), Qt::AlignTop | Qt::AlignLeft, mask.kind == "solid" ? QStringLiteral("solid") : QStringLiteral("blur"));
        if (mask.id == selected_) { painter.setBrush(QColor("#9bbcff")); painter.setPen(Qt::NoPen); painter.drawRect(QRectF(r.right() - handleSize, r.bottom() - handleSize, handleSize, handleSize)); }
    }
    if (!armed_.isEmpty()) { painter.setPen(Qt::white); painter.drawText(rect().adjusted(8, 8, -8, -8), Qt::AlignTop | Qt::AlignLeft, QStringLiteral("Drag on the frame to place the %1 area").arg(armed_)); }
}
void FrameView::mousePressEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton) return;
    const auto p = e->position();
    if (!armed_.isEmpty()) { drag_ = Drag::Create; anchor_ = p; return; }
    for (auto it = masks_.rbegin(); it != masks_.rend(); ++it) {
        const auto r = toView(it->rect);
        if (it->id == selected_ && QRectF(r.right() - handleSize - 2, r.bottom() - handleSize - 2, handleSize + 4, handleSize + 4).contains(p)) { drag_ = Drag::Resize; anchor_ = p; original_ = it->rect; return; }
        if (r.contains(p)) { selected_ = it->id; emit maskSelected(it->id); drag_ = Drag::Move; anchor_ = p; original_ = it->rect; update(); return; }
    }
    selected_.clear();
    emit maskSelected({});
    update();
}
void FrameView::mouseMoveEvent(QMouseEvent* e) {
    if (drag_ == Drag::None) return;
    const auto p = e->position();
    if (drag_ == Drag::Create) { update(); return; }
    const QPointF delta = toCanvas(p) - toCanvas(anchor_);
    auto mask = std::find_if(masks_.begin(), masks_.end(), [&](const RecordingMask& m) { return m.id == selected_; });
    if (mask == masks_.end()) return;
    const QRect bounds(QPoint(0, 0), canvas_.isEmpty() ? frame_.size() : canvas_);
    if (drag_ == Drag::Move) { QRect moved = original_.translated(delta.toPoint()); moved.moveLeft(std::clamp(moved.left(), 0, std::max(0, bounds.width() - moved.width()))); moved.moveTop(std::clamp(moved.top(), 0, std::max(0, bounds.height() - moved.height()))); mask->rect = moved; }
    else { QRect resized = original_; resized.setWidth(std::max(2, original_.width() + int(delta.x()))); resized.setHeight(std::max(2, original_.height() + int(delta.y()))); mask->rect = resized.intersected(bounds); }
    update();
}
void FrameView::mouseReleaseEvent(QMouseEvent* e) {
    if (e->button() != Qt::LeftButton || drag_ == Drag::None) return;
    const auto p = e->position();
    const QRect bounds(QPoint(0, 0), canvas_.isEmpty() ? frame_.size() : canvas_);
    if (drag_ == Drag::Create) {
        const QRect r = QRectF(toCanvas(anchor_), toCanvas(p)).normalized().toAlignedRect().intersected(bounds);
        const auto kind = armed_;
        armed_.clear();
        setCursor(Qt::ArrowCursor);
        drag_ = Drag::None;
        if (r.width() >= 2 && r.height() >= 2) emit maskCreated(kind, r);
        update();
        return;
    }
    drag_ = Drag::None;
    for (const auto& mask : masks_) if (mask.id == selected_ && mask.rect != original_) emit maskMoved(mask.id, mask.rect);
}

ReviewWindow::ReviewWindow(const Options& options, const RecordingConfig& config, QWidget* parent) : QWidget(parent, Qt::Window), options_(options), config_(config), frameTimer_(this) {
    setObjectName("putgd-review");
    setWindowTitle("Review recording");
    setStyleSheet(reviewStyle);
    setAttribute(Qt::WA_DeleteOnClose, false);
    auto* root = new QVBoxLayout(this);
    root->setContentsMargins(14, 12, 14, 12);
    root->setSpacing(8);
    view_ = new FrameView(this);
    view_->setCanvas(options.canvas);
    root->addWidget(view_, 1);
    auto* scrub = new QHBoxLayout;
    time_ = new QLabel("0:00.00", this);
    scrubber_ = new QSlider(Qt::Horizontal, this);
    scrubber_->setRange(0, int(std::max<qint64>(1, options.durationUs / 1000) - 1));
    scrub->addWidget(time_);
    scrub->addWidget(scrubber_, 1);
    root->addLayout(scrub);
    auto* trim = new QHBoxLayout;
    trimStart_ = new QPushButton("Trim start here", this);
    trimEnd_ = new QPushButton("Trim end here", this);
    resetTrim_ = new QPushButton("Reset trim", this);
    retained_ = new QLabel(this);
    retained_->setObjectName("muted");
    for (auto* w : std::initializer_list<QWidget*>{trimStart_, trimEnd_, resetTrim_, retained_}) trim->addWidget(w);
    trim->addStretch(1);
    root->addLayout(trim);
    auto* maskRow = new QHBoxLayout;
    masks_ = new QListWidget(this);
    masks_->setMaximumHeight(96);
    auto* maskButtons = new QVBoxLayout;
    addBlur_ = new QPushButton("Add blur", this);
    addSolid_ = new QPushButton("Add solid cover", this);
    removeMask_ = new QPushButton("Remove", this);
    auto* strengthRow = new QHBoxLayout;
    auto* strengthLabel = new QLabel("Blur", this);
    strengthLabel->setObjectName("muted");
    strength_ = new QSpinBox(this);
    strength_->setRange(2, 64);
    strength_->setValue(16);
    strengthRow->addWidget(strengthLabel);
    strengthRow->addWidget(strength_);
    maskStart_ = new QPushButton("Mask starts here", this);
    maskEnd_ = new QPushButton("Mask ends here", this);
    maskTimes_ = new QLabel(this);
    maskTimes_->setObjectName("muted");
    for (auto* w : std::initializer_list<QWidget*>{addBlur_, addSolid_, removeMask_, maskStart_, maskEnd_, maskTimes_}) maskButtons->addWidget(w);
    maskButtons->addLayout(strengthRow);
    maskRow->addWidget(masks_, 1);
    maskRow->addLayout(maskButtons);
    root->addLayout(maskRow);
    auto* actions = new QHBoxLayout;
    format_ = new QComboBox(this);
    for (const auto& format : options.formats) {
        const auto codecs = options.codecs[format].toArray();
        if (codecs.isEmpty()) format_->addItem(formatLabel(format, {}), QStringList{format, QString()});
        for (const auto& codec : codecs) format_->addItem(formatLabel(format, codec.toString()), QStringList{format, codec.toString()});
    }
    status_ = new QLabel(this);
    status_->setObjectName("status");
    upload_ = new QPushButton("Upload", this);
    upload_->setObjectName("upload");
    save_ = new QPushButton("Save copy…", this);
    cancel_ = new QPushButton("Discard", this);
    actions->addWidget(new QLabel("Output", this));
    actions->addWidget(format_);
    actions->addWidget(status_, 1);
    actions->addWidget(save_);
    actions->addWidget(cancel_);
    actions->addWidget(upload_);
    root->addLayout(actions);
    frameTimer_.setSingleShot(true);
    frameTimer_.setInterval(qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS") > 0 ? qEnvironmentVariableIntValue("PUTGD_RECORD_REVIEW_DELAY_MS") : 60);
    connect(&frameTimer_, &QTimer::timeout, this, &ReviewWindow::refreshFrame);
    connect(scrubber_, &QSlider::valueChanged, this, [this] { refreshLabels(); frameTimer_.start(); });
    connect(trimStart_, &QPushButton::clicked, this, [this] { const qint64 at = qint64(scrubber_->value()) * 1000; const auto end = edit_.exportEnd(options_.durationUs); if (at >= end) { showError("Trim start must be before the trim end"); return; } propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(at)}, {"end", edit_.trimEnd ? QJsonValue(double(*edit_.trimEnd)) : QJsonValue(QJsonValue::Null)}}}); });
    connect(trimEnd_, &QPushButton::clicked, this, [this] { const qint64 at = qint64(scrubber_->value()) * 1000; if (at <= edit_.trimStart) { showError("Trim end must be after the trim start"); return; } propose(QJsonArray{QJsonObject{{"op", "setTrim"}, {"start", double(edit_.trimStart)}, {"end", double(at)}}}); });
    connect(resetTrim_, &QPushButton::clicked, this, [this] { propose(QJsonArray{QJsonObject{{"op", "resetTrim"}}}); });
    connect(addBlur_, &QPushButton::clicked, this, [this] { view_->armCreate("blur"); status_->setText("Drag on the frame to place the blur"); });
    connect(addSolid_, &QPushButton::clicked, this, [this] { view_->armCreate("solid"); status_->setText("Drag on the frame to place the cover"); });
    connect(view_, &FrameView::maskCreated, this, [this](const QString& kind, const QRect& r) {
        // A new mask starts at the current frame and runs to the retained end.
        const qint64 at = std::min<qint64>(qint64(scrubber_->value()) * 1000, options_.durationUs - 1);
        const qint64 end = std::max<qint64>(at + 1, edit_.exportEnd(options_.durationUs));
        QJsonObject mask{{"kind", kind}, {"rect", QJsonArray{r.x(), r.y(), r.width(), r.height()}}, {"start", double(at)}, {"end", double(end)}};
        if (kind == "blur") mask["strength"] = strength_->value();
        status_->clear();
        propose(QJsonArray{QJsonObject{{"op", "addMask"}, {"mask", mask}}});
    });
    connect(view_, &FrameView::maskMoved, this, [this](const QString& id, const QRect& r) {
        auto mask = std::find_if(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == id; });
        if (mask == edit_.masks.end()) return;
        auto updated = mask->toJson();
        updated.remove("id");
        updated["rect"] = QJsonArray{r.x(), r.y(), r.width(), r.height()};
        propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", id}, {"mask", updated}}});
    });
    connect(view_, &FrameView::maskSelected, this, [this](const QString& id) { selectedMask_ = id; refreshMaskList(); });
    connect(masks_, &QListWidget::currentRowChanged, this, [this](int row) { if (row >= 0 && row < edit_.masks.size()) { selectedMask_ = edit_.masks[row].id; view_->setMasks(edit_.masks, selectedMask_); refreshLabels(); } });
    connect(removeMask_, &QPushButton::clicked, this, [this] { if (!selectedMask_.isEmpty()) propose(QJsonArray{QJsonObject{{"op", "removeMask"}, {"maskId", selectedMask_}}}); });
    auto retime = [this](bool start) {
        auto* mask = selectedMask();
        if (!mask) return;
        const qint64 at = qint64(scrubber_->value()) * 1000;
        auto updated = mask->toJson();
        updated.remove("id");
        if (start) { if (at >= mask->end) { showError("The mask must start before it ends"); return; } updated["start"] = double(at); }
        else { if (at <= mask->start) { showError("The mask must end after it starts"); return; } updated["end"] = double(at); }
        propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", mask->id}, {"mask", updated}}});
    };
    connect(maskStart_, &QPushButton::clicked, this, [retime] { retime(true); });
    connect(maskEnd_, &QPushButton::clicked, this, [retime] { retime(false); });
    connect(strength_, &QSpinBox::valueChanged, this, [this](int value) { auto* mask = selectedMask(); if (!mask || mask->kind != "blur" || mask->strength == value) return; auto updated = mask->toJson(); updated.remove("id"); updated["strength"] = value; propose(QJsonArray{QJsonObject{{"op", "updateMask"}, {"maskId", mask->id}, {"mask", updated}}}); });
    connect(format_, &QComboBox::currentIndexChanged, this, [this](int index) {
        const auto value = format_->itemData(index).toStringList();
        if (value.size() != 2 || (value[0] == config_.format && value[1] == config_.codec)) return;
        config_.format = value[0];
        config_.codec = value[1];
        if (config_.silent()) config_.audio.system = false;
        emit configChanged(config_);
        refreshLabels();
    });
    // Upload hands the clip to the resident: the review closes at once and the
    // export and upload continue in the background, reported like any upload.
    connect(upload_, &QPushButton::clicked, this, [this] {
        if (exporting_) return;
        closing_ = true;
        emit uploadRequested(edit_.revision);
        close();
    });
    connect(save_, &QPushButton::clicked, this, [this] {
        if (exporting_) return;
        const auto path = QFileDialog::getSaveFileName(this, "Save a copy", QDir::homePath() + "/recording." + config_.extension(), QStringLiteral("%1 (*.%2)").arg(config_.format.toUpper(), config_.extension()));
        if (path.isEmpty()) return;
        emit saveCopyRequested(path.endsWith("." + config_.extension(), Qt::CaseInsensitive) ? path : path + "." + config_.extension(), edit_.revision);
    });
    connect(cancel_, &QPushButton::clicked, this, [this] { closing_ = true; emit cancelRequested(); });
    setConfig(config);
    setEdit({});
    resize(900, 700);
}
RecordingMask* ReviewWindow::selectedMask() {
    auto it = std::find_if(edit_.masks.begin(), edit_.masks.end(), [&](const RecordingMask& m) { return m.id == selectedMask_; });
    return it == edit_.masks.end() ? nullptr : &*it;
}
QString ReviewWindow::timeText(qint64 us) const { const qint64 h = us / 10000; return QStringLiteral("%1:%2.%3").arg(h / 6000).arg((h / 100) % 60, 2, 10, QChar('0')).arg(h % 100, 2, 10, QChar('0')); }
void ReviewWindow::propose(const QJsonArray& commands) { if (!exporting_) emit editProposed(edit_.revision, commands); }
void ReviewWindow::setEdit(const RecordingEdit& edit) {
    edit_ = edit;
    if (!edit_.masks.isEmpty() && !selectedMask()) selectedMask_ = edit_.masks.last().id;
    if (edit_.masks.isEmpty()) selectedMask_.clear();
    refreshMaskList();
    view_->setMasks(edit_.masks, selectedMask_);
    refreshLabels();
    frameTimer_.start();
}
void ReviewWindow::setConfig(const RecordingConfig& config) {
    config_ = config;
    for (int i = 0; i < format_->count(); ++i) { const auto value = format_->itemData(i).toStringList(); if (value.size() == 2 && value[0] == config.format && value[1] == config.codec) { format_->blockSignals(true); format_->setCurrentIndex(i); format_->blockSignals(false); } }
    refreshLabels();
}
void ReviewWindow::setExporting(bool exporting, const QString& label) {
    exporting_ = exporting;
    for (auto* w : std::initializer_list<QWidget*>{upload_, save_, cancel_, trimStart_, trimEnd_, resetTrim_, addBlur_, addSolid_, removeMask_, maskStart_, maskEnd_, strength_, format_}) w->setEnabled(!exporting);
    status_->setText(label);
}
void ReviewWindow::showError(const QString& message) { status_->setText(message); }
void ReviewWindow::refreshMaskList() {
    masks_->blockSignals(true);
    masks_->clear();
    int row = -1;
    for (int i = 0; i < edit_.masks.size(); ++i) {
        const auto& m = edit_.masks[i];
        masks_->addItem(QStringLiteral("%1 %2×%3 at %4,%5 · %6 – %7").arg(m.kind == "solid" ? "Solid" : "Blur").arg(m.rect.width()).arg(m.rect.height()).arg(m.rect.x()).arg(m.rect.y()).arg(timeText(m.start), timeText(m.end)));
        if (m.id == selectedMask_) row = i;
    }
    masks_->setCurrentRow(row);
    masks_->blockSignals(false);
}
void ReviewWindow::refreshLabels() {
    const qint64 at = qint64(scrubber_->value()) * 1000;
    const auto end = edit_.exportEnd(options_.durationUs);
    const bool retained = at >= edit_.trimStart && at < end;
    time_->setText(timeText(at) + (retained ? QString() : QStringLiteral(" (trimmed)")));
    retained_->setText(QStringLiteral("Keeping %1 – %2 (%3 of %4)").arg(timeText(edit_.trimStart), timeText(end), timeText(end - edit_.trimStart), timeText(options_.durationUs)));
    const auto* mask = selectedMask();
    removeMask_->setEnabled(mask != nullptr);
    maskStart_->setEnabled(mask != nullptr);
    maskEnd_->setEnabled(mask != nullptr);
    strength_->setEnabled(mask && mask->kind == "blur");
    if (mask) { strength_->blockSignals(true); strength_->setValue(mask->strength); strength_->blockSignals(false); maskTimes_->setText(QStringLiteral("%1 – %2").arg(timeText(mask->start), timeText(mask->end))); }
    else maskTimes_->setText("Select a mask to set its times");
    if (!exporting_) status_->setText(config_.silent() ? QStringLiteral("%1 is silent; the export has no audio").arg(config_.format.toUpper()) : options_.audioRecorded ? QStringLiteral("System audio recorded") : QStringLiteral("No audio"));
}
void ReviewWindow::refreshFrame() {
    QImage image;
    qint64 actual = 0;
    const qint64 at = std::min<qint64>(qint64(scrubber_->value()) * 1000, std::max<qint64>(0, options_.durationUs - 1));
    debugLog(QStringLiteral("review: frame at %1").arg(at));
    if (auto e = previewFrame(options_.workingClip, edit_, config_, at, 1280, options_.canvas, image, &actual)) { debugLog("review: preview failed " + e.message); return; }
    view_->setFrame(image, at);
    debugLog(QStringLiteral("review: frame %1x%2").arg(image.width()).arg(image.height()));
}
void ReviewWindow::closeEvent(QCloseEvent* e) {
    // Closing the window is a discard; nothing was uploaded.
    if (!closing_ && !exporting_) { closing_ = true; emit cancelRequested(); }
    e->accept();
}
}
