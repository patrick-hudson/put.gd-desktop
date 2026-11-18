#include "ReviewPanel.h"
#include "Duration.h"
#include "ui/BlurRadius.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QAbstractButton>
#include <QApplication>
#include <QBoxLayout>
#include <QEvent>
#include <QFontMetrics>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QKeyEvent>
#include <QLabel>
#include <QLineEdit>
#include <QPainter>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QScrollBar>
#include <QSpinBox>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>

namespace putgd::record {
namespace {
using Ui::Space;

QString clockText(qint64 us) { return durationText(std::max<qint64>(0, us)); }
QString secondsText(qint64 us) { return QStringLiteral("%1 s").arg(QString::number(double(us) / 1e6, 'f', 1)); }
QString spanText(qint64 start, qint64 end) { return clockText(start) + QStringLiteral("–") + clockText(end); }

// "m:ss.s" or plain seconds; anything else is not a time.
std::optional<qint64> parseClock(const QString& text) {
    static const QRegularExpression minutes(QStringLiteral(R"(^\s*(\d+):(\d{1,2}(?:\.\d+)?)\s*$)"));
    static const QRegularExpression seconds(QStringLiteral(R"(^\s*(\d+(?:\.\d+)?)\s*$)"));
    if (const auto m = minutes.match(text); m.hasMatch()) {
        const double secs = m.captured(2).toDouble();
        if (secs >= 60) return std::nullopt;
        return qint64(std::llround((m.captured(1).toDouble() * 60 + secs) * 1e6));
    }
    if (const auto m = seconds.match(text); m.hasMatch()) return qint64(std::llround(m.captured(1).toDouble() * 1e6));
    return std::nullopt;
}
qint64 frameOf(qint64 us, int fps) { return std::llround(double(us) * fps / 1e6); }
qint64 snapToFrame(qint64 us, int fps, qint64 durationUs) {
    fps = std::max(1, fps);
    return std::clamp<qint64>(std::llround(double(frameOf(us, fps)) * 1e6 / fps), 0, std::max<qint64>(0, durationUs));
}

QString blurSentence() { return QStringLiteral("Blur may be reversible. Use Redact for private information."); }
QString redactSentence() { return QStringLiteral("Covered pixels are removed from the saved and uploaded clip."); }

QString focusKey(const QWidget* widget) {
    if (!widget) return {};
    return widget->objectName() + QLatin1Char(':') + widget->property("rowKey").toString();
}

// A button of the panel: a QPushButton in a Ui role (Enter activates a focused one).
QPushButton* makeButton(QWidget* parent, const QString& text, const QString& glyph, const char* role, const QString& name, const QString& objectName) {
    auto* button = new QPushButton(text, parent);
    if (!glyph.isEmpty()) {
        button->setIcon(Ui::icon(glyph));
        button->setIconSize(QSize(Ui::Metric::icon, Ui::Metric::icon));
    }
    Ui::role(button, role);
    button->setAutoDefault(true);
    button->setFocusPolicy(Qt::StrongFocus);
    button->setAccessibleName(name.isEmpty() ? text : name);
    button->setObjectName(objectName);
    return button;
}

// An IconArt glyph painted in the active scheme, so a scheme switch repaints it.
class Glyph final : public QWidget {
public:
    Glyph(const QString& name, int pixels, QWidget* parent) : QWidget(parent), icon_(Ui::icon(name)), name_(name) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setFixedSize(pixels, pixels);
        connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, [this] { icon_ = Ui::icon(name_); update(); });
    }
protected:
    void paintEvent(QPaintEvent*) override {
        QPainter p(this);
        icon_.paint(&p, rect());
    }
private:
    QIcon icon_;
    QString name_;
};

// A key as the gestures list shows it: mono text in a hairline box.
class KeyCap final : public QWidget {
public:
    KeyCap(const QString& text, QWidget* parent) : QWidget(parent), text_(text) {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        const QFontMetrics fm(Ui::monoFont(11));
        setFixedSize(fm.horizontalAdvance(text) + 2 * Ui::scaled(6), std::max(Ui::scaled(18), fm.height() + 4));
        connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
    }
protected:
    void paintEvent(QPaintEvent*) override {
        const auto& c = Theme::current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QPen(c.borderStrong, 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
        Ui::paintText(p, rect(), text_, c.text, Ui::monoFont(11), Qt::AlignCenter);
    }
private:
    QString text_;
};

// One entry of Edits: icon, name and span; a selected row (or a hovered or
// focused one) shows its verb in place of the span. Enter or a click selects
// it; Up / Down move between rows; Delete on a cover removes it.
class EditRow final : public QWidget {
    Q_OBJECT
public:
    EditRow(const QString& kind, const QString& id, const QString& name, const QString& span, const QString& glyph, bool selected, const QString& verb, bool verbOffered, QWidget* parent)
        : QWidget(parent), kind_(kind), name_(name), span_(span), glyph_(Ui::icon(glyph)), selected_(selected), verbOffered_(verbOffered) {
        setObjectName("reviewPanelRow");
        setProperty("rowKey", kind + QLatin1Char('/') + id);
        setFocusPolicy(Qt::StrongFocus);
        setAttribute(Qt::WA_Hover);
        setMouseTracking(true);
        setAccessibleName(QStringLiteral("%1, %2").arg(name, QString(span).replace(QStringLiteral("–"), QStringLiteral(" to "))));
        if (selected) setAccessibleDescription(QStringLiteral("Selected"));
        verb_ = makeButton(this, verb, {}, "quiet", QStringLiteral("%1 %2").arg(verb, kind == "trim" ? QStringLiteral("trim") : kind == "cut" ? QStringLiteral("cut") : name), QStringLiteral("reviewPanelRowVerb"));
        verb_->setProperty("rowKey", property("rowKey"));
        verb_->setAutoDefault(false);
        verb_->setFixedHeight(rowHeight() - Ui::space(Space::s));
        connect(verb_, &QPushButton::clicked, this, &EditRow::verbClicked);
        connect(qApp, &QApplication::focusChanged, this, [this] { refreshVerb(); });
        connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, [this] { update(); });
        refreshVerb();
    }
    static int rowHeight() { return std::max(Ui::scaled(40), QFontMetrics(Ui::font(Ui::Type::body, true)).height() + Ui::scaled(16)); }
    QSize sizeHint() const override { return {Ui::scaled(200), rowHeight()}; }
    QSize minimumSizeHint() const override { return {0, rowHeight()}; }
    QPushButton* verbButton() const { return verb_; }
signals:
    void activated();
    void verbClicked();
    void removeKey();
protected:
    void resizeEvent(QResizeEvent*) override { placeVerb(); }
    void enterEvent(QEnterEvent*) override { hovered_ = true; refreshVerb(); update(); }
    void leaveEvent(QEvent*) override { hovered_ = false; refreshVerb(); update(); }
    void mouseReleaseEvent(QMouseEvent* event) override {
        if (event->button() == Qt::LeftButton && rect().contains(event->position().toPoint())) emit activated();
        else QWidget::mouseReleaseEvent(event);
    }
    void keyPressEvent(QKeyEvent* event) override {
        switch (event->key()) {
        case Qt::Key_Return: case Qt::Key_Enter: case Qt::Key_Space: emit activated(); break;
        case Qt::Key_Down: case Qt::Key_Up: {
            const auto rows = parentWidget() ? parentWidget()->findChildren<EditRow*>(QString(), Qt::FindDirectChildrenOnly) : QList<EditRow*>{};
            const int at = int(rows.indexOf(this)) + (event->key() == Qt::Key_Down ? 1 : -1);
            if (at >= 0 && at < rows.size()) rows[at]->setFocus(Qt::TabFocusReason);
            break;
        }
        case Qt::Key_Delete: case Qt::Key_Backspace:
            if (kind_ == "cover") emit removeKey();  // never restores a cut
            else event->ignore();
            break;
        default: QWidget::keyPressEvent(event);
        }
    }
    void focusInEvent(QFocusEvent* event) override { QWidget::focusInEvent(event); update(); }
    void focusOutEvent(QFocusEvent* event) override { QWidget::focusOutEvent(event); update(); }
    void paintEvent(QPaintEvent*) override {
        const auto& c = Theme::current();
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);
        const QRectF body = QRectF(rect()).adjusted(0, 0, 0, 0);
        if (selected_) {
            Ui::paintSelection(p, body, false, 7);
            p.setPen(Qt::NoPen);
            p.setBrush(c.accent);
            p.drawRoundedRect(QRectF(0, 3, 2, height() - 6), 1, 1);
        } else if (hovered_) {
            Ui::paintSelection(p, body, false, 7);
        }
        if (hasFocus() && Ui::keyboardFocus(this)) {
            p.setPen(QPen(c.accent, 2));
            p.setBrush(Qt::NoBrush);
            p.drawRoundedRect(body.adjusted(1, 1, -1, -1), 7, 7);
        }
        const int pad = Ui::space(Space::s), gap = Ui::space(Space::m);
        const int chip = std::max(Ui::scaled(26), QFontMetrics(Ui::font()).height() + 4);
        const QRectF chipRect(pad + 2, (height() - chip) / 2.0, chip, chip);
        p.setPen(Qt::NoPen);
        p.setBrush(c.surface);
        p.drawRoundedRect(chipRect, 6, 6);
        const int glyph = std::max(Ui::scaled(15), chip - 10);
        glyph_.paint(&p, QRect(int(chipRect.center().x()) - glyph / 2, int(chipRect.center().y()) - glyph / 2, glyph, glyph));
        const QFont nameFont = Ui::font(Ui::Type::body, true), spanFont = Ui::monoFont(Ui::Type::caption);
        int right = width() - pad;
        if (verb_->isVisibleTo(this)) right = verb_->geometry().left() - gap;
        else {
            const int spanWidth = QFontMetrics(spanFont).horizontalAdvance(span_);
            Ui::paintText(p, QRectF(width() - pad - spanWidth, 0, spanWidth, height()), span_, c.textMuted, spanFont, Qt::AlignRight | Qt::AlignVCenter, Qt::ElideNone);
            right = width() - pad - spanWidth - gap;
        }
        const qreal left = chipRect.right() + gap;
        Ui::paintText(p, QRectF(left, 0, std::max<qreal>(0, right - left), height()), name_, c.text, nameFont);
    }
private:
    void placeVerb() {
        const QSize size = verb_->sizeHint();
        verb_->setGeometry(width() - Ui::space(Space::xs) - size.width(), (height() - verb_->height()) / 2, size.width(), verb_->height());
    }
    void refreshVerb() {
        const auto* focus = QApplication::focusWidget();
        const bool show = verbOffered_ || hovered_ || focus == this || focus == verb_;
        if (show == verb_->isVisibleTo(this) && !show) return;
        verb_->setVisible(show);
        if (show) placeVerb();
        update();
    }
    QString kind_, name_, span_;
    QIcon glyph_;
    bool selected_, verbOffered_, hovered_ = false;
    QPushButton* verb_ = nullptr;
};
}  // namespace

ReviewPanel::ReviewPanel(std::shared_ptr<MaskNames> names, QWidget* parent) : QWidget(parent), names_(std::move(names)) {
    setObjectName("reviewPanel");
    setAccessibleName("Edits panel");
    if (!names_) names_ = std::make_shared<MaskNames>();
    root_ = new QVBoxLayout(this);
    root_->setContentsMargins(0, 0, 0, 0);
    root_->setSpacing(0);
    connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
    rebuild();
}

QSize ReviewPanel::sizeHint() const {
    auto size = QWidget::sizeHint();
    size.setWidth(280);
    return size;
}

void ReviewPanel::paintEvent(QPaintEvent*) {
    // Folded into the Edits overlay, the overlay supplies the surface.
    if (property("folded").toBool()) return;
    const auto& c = Theme::current();
    QPainter p(this);
    p.fillRect(rect(), c.bg);
    p.setPen(c.border);
    p.drawLine(0, 0, 0, height());
}

void ReviewPanel::setState(const State& state) {
    state_ = state;
    names_->observe(state_.edit.masks);
    rebuild();
}

const RecordingMask* ReviewPanel::selectedCover() const {
    if (state_.selectedKind != "cover") return nullptr;
    const auto it = std::find_if(state_.edit.masks.cbegin(), state_.edit.masks.cend(), [&](const RecordingMask& m) { return m.id == state_.selectedId; });
    return it == state_.edit.masks.cend() ? nullptr : &*it;
}
const RecordingCut* ReviewPanel::selectedCut() const {
    if (state_.selectedKind != "cut") return nullptr;
    const auto it = std::find_if(state_.edit.cuts.cbegin(), state_.edit.cuts.cend(), [&](const RecordingCut& c) { return c.id == state_.selectedId; });
    return it == state_.edit.cuts.cend() ? nullptr : &*it;
}
bool ReviewPanel::anyEdit() const {
    const auto& e = state_.edit;
    return e.trimStart > 0 || (e.trimEnd && *e.trimEnd < state_.durationUs) || !e.cuts.isEmpty() || !e.masks.isEmpty();
}

QWidget* ReviewPanel::defaultControl() const { return defaultControl_; }

QWidget* ReviewPanel::firstControl() const {
    if (defaultControl_) return defaultControl_;
    for (auto* child : findChildren<QWidget*>()) {
        if (child->focusPolicy() == Qt::NoFocus || !child->isEnabled() || !child->isVisibleTo(this)) continue;
        if (qobject_cast<QLineEdit*>(child->parentWidget())) continue;  // a spin box's inner editor
        return child;
    }
    return nullptr;
}

void ReviewPanel::rebuild() {
    building_ = true;
    const auto* focus = QApplication::focusWidget();
    const QString restore = focus && isAncestorOf(focus) ? focusKey(focus) : QString();
    // Detach at once so a lookup never finds the old controls; delete later
    // because a signal handler may be running inside one of them.
    for (auto** w : {&top_, &list_, &rule_}) {
        if (*w) {
            (*w)->hide();
            (*w)->setParent(nullptr);
            (*w)->deleteLater();
            *w = nullptr;
        }
    }
    while (auto* item = root_->takeAt(0)) delete item;
    defaultControl_ = nullptr;

    if (selectedCover()) mode_ = QStringLiteral("cover");
    else if (selectedCut()) mode_ = QStringLiteral("cut");
    else if (state_.timeSelection && !state_.timeSelection->empty()) mode_ = QStringLiteral("selection");
    else if (anyEdit()) mode_ = QStringLiteral("edits");
    else mode_ = QStringLiteral("empty");
    title_ = mode_ == "edits" ? QStringLiteral("Edits") : QString();

    const int m = Ui::space(Space::l);
    if (mode_ != "edits") {
        top_ = new QWidget(this);
        auto* v = new QVBoxLayout(top_);
        v->setContentsMargins(m, m, m, m);
        v->setSpacing(Ui::space(Space::s));
        buildTop(v);
    }
    if (anyEdit()) {
        list_ = new QWidget(this);
        auto* v = new QVBoxLayout(list_);
        v->setContentsMargins(m, m, m, m);
        v->setSpacing(0);
        buildList(v);
    }
    if (top_ && list_) rule_ = Ui::rule(this);
    for (auto* w : {top_, rule_, list_}) if (w) root_->addWidget(w);
    root_->addStretch(1);
    if (isVisible()) for (auto* w : {top_, rule_, list_}) if (w) w->show();
    building_ = false;

    if (!restore.isEmpty()) {
        for (auto* w : findChildren<QWidget*>()) {
            if (w->focusPolicy() != Qt::NoFocus && focusKey(w) == restore) { w->setFocus(Qt::OtherFocusReason); break; }
        }
    }
    updateGeometry();
}

void ReviewPanel::buildTop(QVBoxLayout* v) {
    const auto heading = [&](const QString& text) {
        auto* label = Ui::label(text, "sectionTitle", top_);
        label->setObjectName("reviewPanelTitle");
        label->setWordWrap(true);
        title_ = text;
        v->addWidget(label);
    };
    const auto sentence = [&](const QString& text) {
        auto* label = Ui::label(text, "meta", top_);
        label->setWordWrap(true);
        v->addWidget(label);
        return label;
    };
    const auto spanLine = [&](qint64 start, qint64 end, const QString& tail) {
        auto* label = new QLabel(top_);
        Ui::role(label, "meta");
        label->setTextFormat(Qt::RichText);
        label->setWordWrap(true);
        label->setText(QStringLiteral("<span style=\"font-family:'%1'\">%2 – %3</span>%4")
                           .arg(Ui::monoFamily().toHtmlEscaped(), clockText(start).toHtmlEscaped(), clockText(end).toHtmlEscaped(), tail.toHtmlEscaped()));
        label->setAccessibleName(QStringLiteral("%1 to %2%3").arg(clockText(start), clockText(end), tail));
        v->addWidget(label);
    };
    const auto row = [&]() {
        auto* h = new QHBoxLayout;
        h->setContentsMargins(0, 0, 0, 0);
        h->setSpacing(Ui::space(Space::s));
        return h;
    };

    if (mode_ == "empty") {
        heading(QStringLiteral("No edits yet"));
        sentence(QStringLiteral("Upload sends the whole clip."));
        v->addSpacing(Ui::space(Space::xs));
        struct Gesture { QStringList keys; bool pointer; QString text; };
        const QList<Gesture> gestures{
            {{QStringLiteral("[ ]")}, false, QStringLiteral("Drag a bracket to trim the ends")},
            {{QStringLiteral("drag")}, true, QStringLiteral("Drag across the filmstrip to select time")},
            {{QStringLiteral("C"), QStringLiteral("Del")}, false, QStringLiteral("Cut the selected time")},
            {{QStringLiteral("X"), QStringLiteral("B")}, false, QStringLiteral("Redact or blur part of the picture for that time")},
            {{QStringLiteral("Ctrl+Z")}, false, QStringLiteral("Undo any edit")},
        };
        v->setSpacing(0);
        for (int i = 0; i < gestures.size(); ++i) {
            const auto& g = gestures[i];
            if (i) v->addWidget(Ui::rule(top_));
            auto* line = new QWidget(top_);
            auto* h = new QHBoxLayout(line);
            h->setContentsMargins(0, Ui::space(Space::s), 0, Ui::space(Space::s));
            h->setSpacing(Ui::space(Space::s));
            auto* keys = new QWidget(line);
            keys->setFixedWidth(Ui::scaled(56));
            auto* kh = new QHBoxLayout(keys);
            kh->setContentsMargins(0, 0, 0, 0);
            kh->setSpacing(Ui::space(Space::xs));
            if (g.pointer) {
                kh->addWidget(new Glyph(QStringLiteral("select"), Ui::scaled(14), keys));
                auto* drag = Ui::label(g.keys.first(), "micro", keys);
                kh->addWidget(drag);
            } else {
                for (const auto& key : g.keys) kh->addWidget(new KeyCap(key, keys));
            }
            kh->addStretch(1);
            h->addWidget(keys, 0, Qt::AlignTop);
            auto* text = Ui::label(g.text, "meta", line);
            text->setWordWrap(true);
            h->addWidget(text, 1);
            line->setAccessibleName(QStringLiteral("%1: %2").arg(g.keys.join(QStringLiteral(" or ")), g.text));
            v->addWidget(line);
        }
        return;
    }

    if (mode_ == "selection") {
        const auto span = *state_.timeSelection;
        heading(QStringLiteral("Selected time"));
        spanLine(span.start, span.end, QStringLiteral(" · ") + secondsText(span.length()));
        v->addSpacing(Ui::space(Space::xs));
        auto* buttons = row();
        auto* cut = makeButton(top_, QStringLiteral("Cut"), QStringLiteral("cut"), nullptr, QStringLiteral("Cut"), QStringLiteral("reviewPanelCut"));
        auto* redact = makeButton(top_, QStringLiteral("Redact"), QStringLiteral("redact"), nullptr, QStringLiteral("Redact"), QStringLiteral("reviewPanelRedact"));
        auto* blur = makeButton(top_, QStringLiteral("Blur"), QStringLiteral("blur"), nullptr, QStringLiteral("Blur"), QStringLiteral("reviewPanelBlur"));
        cut->setToolTip(QStringLiteral("Cut the selected time out of the clip (C)"));
        redact->setToolTip(QStringLiteral("Cover part of the picture for this time, removing the pixels (X)"));
        blur->setToolTip(QStringLiteral("Blur part of the picture for this time (B). Blur may be reversible."));
        for (auto* b : {cut, redact, blur}) { b->setIconSize(QSize(Ui::scaled(16), Ui::scaled(16))); b->setProperty("uiTight", true); }
        // Three buttons side by side while they fit the 280 px panel; the third wraps below at larger text.
        const int fit = 280 - 2 * Ui::space(Space::l);
        const int wanted = cut->sizeHint().width() + redact->sizeHint().width() + blur->sizeHint().width() + 2 * Ui::space(Space::s);
        if (wanted <= fit) {
            for (auto* b : {cut, redact, blur}) buttons->addWidget(b);
            buttons->addStretch(1);
            v->addLayout(buttons);
        } else {
            delete buttons;
            auto* grid = new QGridLayout;
            grid->setContentsMargins(0, 0, 0, 0);
            grid->setSpacing(Ui::space(Space::s));
            grid->addWidget(cut, 0, 0);
            grid->addWidget(redact, 0, 1);
            grid->addWidget(blur, 1, 0);
            grid->setColumnStretch(2, 1);
            v->addLayout(grid);
        }
        connect(cut, &QPushButton::clicked, this, &ReviewPanel::cutRequested);
        connect(redact, &QPushButton::clicked, this, [this] { emit coverRequested(QStringLiteral("solid")); });
        connect(blur, &QPushButton::clicked, this, [this] { emit coverRequested(QStringLiteral("blur")); });
        defaultControl_ = cut;
        return;
    }

    if (const auto* cut = selectedCut()) {
        heading(QStringLiteral("Cut"));
        spanLine(cut->start, cut->end, QStringLiteral(" · %1 left out of the clip.").arg(secondsText(cut->end - cut->start)));
        v->addSpacing(Ui::space(Space::xs));
        auto* buttons = row();
        auto* restore = makeButton(top_, QStringLiteral("Restore"), {}, "primary", QStringLiteral("Restore"), QStringLiteral("reviewPanelRestore"));
        auto* across = makeButton(top_, QStringLiteral("Play across"), QStringLiteral("play"), nullptr, QStringLiteral("Play across"), QStringLiteral("reviewPanelPlayAcross"));
        restore->setDefault(true);
        restore->setToolTip(QStringLiteral("Put this footage back into the clip"));
        across->setToolTip(QStringLiteral("Play from a second before the join"));
        buttons->addWidget(restore);
        buttons->addWidget(across);
        buttons->addStretch(1);
        v->addLayout(buttons);
        const QString id = cut->id;
        connect(restore, &QPushButton::clicked, this, [this, id] { emit restoreCut(id); });
        connect(across, &QPushButton::clicked, this, [this, id] { emit playAcross(id); });
        defaultControl_ = restore;
        return;
    }

    const auto& mask = *selectedCover();
    const bool blur = mask.kind == "blur";
    heading(names_->name(mask));
    sentence(blur ? blurSentence() : redactSentence());
    if (!state_.coverFirstKept && !state_.coverLastKept) {
        auto* excluded = Ui::label(QStringLiteral("Excluded from output"), "meta", top_);
        excluded->setObjectName("reviewPanelExcluded");
        v->addWidget(excluded);
    }
    addEdge(v, mask, true);
    addEdge(v, mask, false);

    auto* kindLabel = Ui::label(QStringLiteral("Cover"), "meta", top_);
    v->addSpacing(Ui::space(Space::xs));
    v->addWidget(kindLabel);
    auto* track = new Ui::SegmentTrack(top_);
    track->setObjectName("reviewPanelCoverKind");
    track->setAccessibleName("Cover type");
    track->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    const QString id = mask.id;
    for (const bool isBlur : {false, true}) {
        auto* choice = new QToolButton(track);
        choice->setObjectName(isBlur ? "reviewPanelCoverBlur" : "reviewPanelCoverRedact");
        choice->setText(isBlur ? QStringLiteral("Blur") : QStringLiteral("Redact"));
        choice->setAccessibleName(choice->text());
        choice->setToolTip(isBlur ? QStringLiteral("Blur the covered pixels. Blur may be reversible.") : QStringLiteral("Remove the covered pixels"));
        choice->setFocusPolicy(Qt::StrongFocus);
        choice->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        track->addButton(choice);
        choice->setChecked(isBlur == blur);
        connect(choice, &QToolButton::toggled, this, [this, id, isBlur](bool on) {
            if (on && !building_) emit coverKindChanged(id, isBlur ? QStringLiteral("blur") : QStringLiteral("solid"));
        });
    }
    v->addWidget(track);
    if (blur) {
        v->addSpacing(Ui::space(Space::xs));
        auto* radius = new BlurRadius(2, 64, std::clamp(mask.strength, 2, 64), true, top_);
        radius->setObjectName("reviewPanelStrengthRow");
        radius->spinBox()->setObjectName("reviewPanelStrength");
        radius->label()->setText(QStringLiteral("Strength"));
        radius->spinBox()->setAccessibleName("Blur strength");
        radius->spinBox()->setAccessibleDescription(QStringLiteral("Blur strength, 2 to 64"));
        radius->spinBox()->setToolTip(QStringLiteral("Blur strength, 2 to 64"));
        v->addWidget(radius);
        connect(radius->spinBox(), &QSpinBox::valueChanged, this, [this, id](int value) {
            if (!building_) emit coverStrengthChanged(id, value);
        });
    }
}

void ReviewPanel::addEdge(QVBoxLayout* v, const RecordingMask& mask, bool start) {
    const QString edge = start ? QStringLiteral("Shows from") : QStringLiteral("Until");
    const QString id = mask.id;
    const auto kept = start ? state_.coverFirstKept : state_.coverLastKept;
    const qint64 time = start ? mask.start : mask.end;
    const QString key = start ? QStringLiteral("ShowsFrom") : QStringLiteral("Until");

    v->addSpacing(Ui::space(Space::xs));
    auto* seekRow = new QHBoxLayout;
    seekRow->setContentsMargins(0, 0, 0, 0);
    auto* seek = makeButton(top_, edge + QStringLiteral(" →"), {}, "link", edge, QStringLiteral("reviewPanel") + key);
    seek->setProperty("uiFlush", true);
    seek->setAccessibleDescription(start ? QStringLiteral("Go to the first covered frame that stays in the clip (Home)")
                                         : QStringLiteral("Go to the last covered frame that stays in the clip (End)"));
    seek->setEnabled(kept.has_value());
    seek->setToolTip(kept ? seek->accessibleDescription() : QStringLiteral("Excluded from output"));
    connect(seek, &QPushButton::clicked, this, [this, id, start] { emit seekCoverEdge(id, start); });
    seekRow->addWidget(seek);
    seekRow->addStretch(1);
    v->addLayout(seekRow);

    auto* fieldRow = new QHBoxLayout;
    fieldRow->setContentsMargins(0, 0, 0, 0);
    fieldRow->setSpacing(Ui::space(Space::s));
    auto* field = new QLineEdit(clockText(time), top_);
    field->setObjectName(QStringLiteral("reviewPanel") + key + QStringLiteral("Time"));
    Ui::setMono(field, Ui::Type::technical);
    field->setAccessibleName(edge + QStringLiteral(" time"));
    field->setAccessibleDescription(QStringLiteral("Minutes:seconds.tenths in recording time; snaps to a whole frame"));
    field->setToolTip(QStringLiteral("Frame %1").arg(frameOf(time, state_.fps)));
    field->setFixedWidth(std::max(Ui::scaled(84), QFontMetrics(field->font()).horizontalAdvance(QStringLiteral("000:00.0")) + Ui::scaled(24)));
    field->setFixedHeight(std::max(Ui::scaled(32), QFontMetrics(field->font()).height() + Ui::scaled(14)));
    field->setProperty("shown", field->text());
    connect(field, &QLineEdit::editingFinished, this, [this, field, id, start] {
        if (building_) return;
        const QString shown = field->property("shown").toString();
        if (field->text().trimmed() == shown) return;
        const auto parsed = parseClock(field->text());
        if (!parsed) { field->setText(shown); return; }
        const qint64 snapped = snapToFrame(*parsed, state_.fps, state_.durationUs);
        field->setText(clockText(snapped));
        field->setProperty("shown", field->text());
        field->setToolTip(QStringLiteral("Frame %1").arg(frameOf(snapped, state_.fps)));
        emit coverEdgeEdited(id, start, snapped);
    });
    fieldRow->addWidget(field);
    auto* playhead = makeButton(top_, QStringLiteral("Set to playhead"), {}, nullptr, QStringLiteral("Set %1 to playhead").arg(edge), QStringLiteral("reviewPanel") + key + QStringLiteral("Playhead"));
    playhead->setToolTip(QStringLiteral("Set %1 to the playhead (%2)").arg(edge, start ? QStringLiteral("⇧I") : QStringLiteral("⇧O")));
    connect(playhead, &QPushButton::clicked, this, [this, id, start] { emit coverEdgeToPlayhead(id, start); });
    playhead->setFixedHeight(field->height());
    fieldRow->addWidget(playhead);
    fieldRow->addStretch(1);
    v->addLayout(fieldRow);
}

void ReviewPanel::buildList(QVBoxLayout* v) {
    auto* heading = Ui::label(QStringLiteral("Edits"), "sectionTitle", list_);
    heading->setObjectName("reviewPanelEditsTitle");
    v->addWidget(heading);
    v->addSpacing(Ui::space(Space::s));

    struct Item { QString kind, id, name, glyph; qint64 start, end; int order; };
    QList<Item> items;
    const auto& e = state_.edit;
    if (e.trimStart > 0 || (e.trimEnd && *e.trimEnd < state_.durationUs))
        items.push_back({QStringLiteral("trim"), {}, QStringLiteral("Trim"), QStringLiteral("trim-reset"), e.trimStart, e.exportEnd(state_.durationUs), 0});
    for (const auto& cut : e.cuts) items.push_back({QStringLiteral("cut"), cut.id, QStringLiteral("Cut"), QStringLiteral("cut"), cut.start, cut.end, 1});
    for (const auto& mask : e.masks)
        items.push_back({QStringLiteral("cover"), mask.id, names_->name(mask), mask.kind == "solid" ? QStringLiteral("redact") : QStringLiteral("blur"), mask.start, mask.end, 1});
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.start != b.start ? a.start < b.start : a.order < b.order; });

    auto* rows = new QWidget(list_);
    auto* column = new QVBoxLayout(rows);
    column->setContentsMargins(0, 0, 0, 0);
    column->setSpacing(0);
    for (const auto& item : items) {
        const bool selected = state_.selectedKind == item.kind && (item.kind == "trim" || state_.selectedId == item.id);
        const bool cut = item.kind == "cut";
        const QString verb = item.kind == "trim" ? QStringLiteral("Reset") : cut ? QStringLiteral("Restore") : QStringLiteral("Remove");
        // The part above already offers Restore for a selected cut.
        const bool offered = selected && !cut;
        auto* row = new EditRow(item.kind, item.id, item.name, spanText(item.start, item.end), item.glyph, selected, verb, offered, rows);
        const QString kind = item.kind, id = item.id;
        connect(row, &EditRow::activated, this, [this, kind, id] { emit editActivated(kind, id); });
        connect(row->verbButton(), &QPushButton::clicked, this, [this, kind, id] {
            if (kind == "trim") emit resetTrim();
            else if (kind == "cut") emit restoreCut(id);
            else emit removeCover(id);
        });
        connect(row, &EditRow::removeKey, this, [this, id] { emit removeCover(id); });
        column->addWidget(row);
    }
    v->addWidget(rows);
}

QJsonObject ReviewPanel::inspect() const {
    QJsonArray rows;
    const auto& e = state_.edit;
    struct Item { QString kind, id, name; qint64 start, end; int order; };
    QList<Item> items;
    if (anyEdit()) {
        if (e.trimStart > 0 || (e.trimEnd && *e.trimEnd < state_.durationUs))
            items.push_back({QStringLiteral("trim"), {}, QStringLiteral("Trim"), e.trimStart, e.exportEnd(state_.durationUs), 0});
        for (const auto& cut : e.cuts) items.push_back({QStringLiteral("cut"), cut.id, QStringLiteral("Cut"), cut.start, cut.end, 1});
        for (const auto& mask : e.masks) items.push_back({QStringLiteral("cover"), mask.id, names_->name(mask), mask.start, mask.end, 1});
    }
    std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.start != b.start ? a.start < b.start : a.order < b.order; });
    for (const auto& item : items) {
        const bool selected = state_.selectedKind == item.kind && (item.kind == "trim" || state_.selectedId == item.id);
        rows.append(QJsonObject{{"kind", item.kind}, {"id", item.id}, {"name", item.name},
                                {"span", QJsonObject{{"start", double(item.start)}, {"end", double(item.end)}}}, {"selected", selected}});
    }
    QJsonArray buttons;
    for (auto* part : {top_, list_}) {
        if (!part) continue;
        // Creation order is layout order.
        for (auto* child : part->findChildren<QWidget*>()) {
            if (!child->isVisibleTo(this)) continue;
            if (const auto* button = qobject_cast<QAbstractButton*>(child)) {
                buttons.append(QJsonObject{{"name", button->accessibleName().isEmpty() ? button->text() : button->accessibleName()}, {"enabled", button->isEnabled()}});
            }
        }
    }
    return QJsonObject{{"mode", mode_}, {"title", title_}, {"rows", rows}, {"buttons", buttons}};
}

// ---- EditsOverlay ----

namespace {
constexpr int overlayWidth = 300;

QLayout* ownerLayout(QLayout* layout, QWidget* widget) {
    if (!layout) return nullptr;
    if (layout->indexOf(widget) >= 0) return layout;
    for (int i = 0; i < layout->count(); ++i)
        if (auto* found = ownerLayout(layout->itemAt(i)->layout(), widget)) return found;
    return nullptr;
}
}  // namespace

EditsOverlay::EditsOverlay(QWidget* window) : QFrame(window), window_(window) {
    setObjectName("editsOverlay");
    setAccessibleName("Edits");
    setFocusPolicy(Qt::StrongFocus);
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(1, 1, 1, 1);
    layout->setSpacing(0);
    scroll_ = new QScrollArea(this);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidgetResizable(true);
    scroll_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll_->setFocusPolicy(Qt::NoFocus);
    Ui::transparentView(scroll_);
    layout->addWidget(scroll_);
    connect(&Theme::Notifier::instance(), &Theme::Notifier::changed, this, qOverload<>(&QWidget::update));
    hide();
}

bool EditsOverlay::isOpen() const { return isVisible() && panel_; }

void EditsOverlay::openFor(ReviewPanel* panel, QWidget* toggle) {
    if (!panel || !toggle || !window_) return;
    if (isOpen()) dismiss();
    panel_ = panel;
    toggle_ = toggle;
    previousParent_ = panel->parentWidget();
    previousLayout_ = nullptr;
    boxIndex_ = gridRow_ = -1;
    wasHidden_ = panel->isHidden();
    minimumWidth_ = panel->minimumWidth();
    maximumWidth_ = panel->maximumWidth();
    if (previousParent_) {
        if (auto* owner = ownerLayout(previousParent_->layout(), panel)) {
            previousLayout_ = owner;
            const int index = owner->indexOf(panel);
            if (auto* box = qobject_cast<QBoxLayout*>(owner)) {
                boxIndex_ = index;
                boxStretch_ = box->stretch(index);
                alignment_ = box->itemAt(index)->alignment();
            } else if (auto* grid = qobject_cast<QGridLayout*>(owner)) {
                grid->getItemPosition(index, &gridRow_, &gridColumn_, &gridRowSpan_, &gridColumnSpan_);
                alignment_ = grid->itemAt(index)->alignment();
            }
            owner->removeWidget(panel);
        }
    }
    panel->setMinimumWidth(0);
    panel->setMaximumWidth(QWIDGETSIZE_MAX);
    panel->setProperty("folded", true);
    scroll_->setWidget(panel);
    panel->setAutoFillBackground(false);  // QScrollArea fills its widget; the overlay supplies the surface
    panel->show();
    place();
    raise();
    show();
    qApp->installEventFilter(this);
    window_->installEventFilter(this);
    panel->installEventFilter(this);
    // Focus on the default control, else the first one, else the overlay (Esc still works).
    if (auto* target = panel->firstControl()) target->setFocus(Qt::PopupFocusReason);
    else setFocus(Qt::PopupFocusReason);
}

void EditsOverlay::place() {
    if (!window_ || !toggle_ || !panel_) return;
    const QPoint below = toggle_->mapTo(window_, QPoint(toggle_->width(), toggle_->height()));
    const int margin = Ui::space(Space::s);
    const int x = std::clamp(below.x() - overlayWidth, margin, std::max(margin, window_->width() - overlayWidth - margin));
    const int y = below.y() + Ui::space(Space::m);
    const int inner = overlayWidth - 2;
    const int natural = panel_->layout() && panel_->layout()->hasHeightForWidth() ? panel_->layout()->heightForWidth(inner) : 0;
    const int wanted = std::max({natural, panel_->sizeHint().height(), panel_->minimumSizeHint().height()}) + 2;
    const int available = std::max(Ui::scaled(120), window_->height() - y - margin);
    setGeometry(x, y, overlayWidth, std::min(wanted, available));
}

void EditsOverlay::giveBack() {
    auto* panel = panel_.data();
    if (!panel) return;
    scroll_->takeWidget();
    panel->setProperty("folded", false);
    panel->setMinimumWidth(minimumWidth_);
    panel->setMaximumWidth(maximumWidth_);
    panel->setParent(previousParent_.data());
    if (previousLayout_) {
        if (auto* box = qobject_cast<QBoxLayout*>(previousLayout_.data()))
            box->insertWidget(std::min(boxIndex_, box->count()), panel, boxStretch_, alignment_);
        else if (auto* grid = qobject_cast<QGridLayout*>(previousLayout_.data()); grid && gridRow_ >= 0)
            grid->addWidget(panel, gridRow_, gridColumn_, gridRowSpan_, gridColumnSpan_, alignment_);
    }
    if (wasHidden_) panel->hide();
    else panel->show();
}

void EditsOverlay::dismiss() {
    if (!panel_) { hide(); return; }
    qApp->removeEventFilter(this);
    if (window_) window_->removeEventFilter(this);
    panel_->removeEventFilter(this);
    giveBack();
    hide();
    panel_ = nullptr;
    if (toggle_) toggle_->setFocus(Qt::OtherFocusReason);
    emit closed();
}

void EditsOverlay::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Escape) {
        dismiss();
        event->accept();
        return;
    }
    QFrame::keyPressEvent(event);
}

bool EditsOverlay::eventFilter(QObject* watched, QEvent* event) {
    if (!isOpen()) return false;
    switch (event->type()) {
    case QEvent::ShortcutOverride: {
        // The window's own Escape shortcut must not take the key first.
        const auto* key = static_cast<QKeyEvent*>(event);
        const auto* focus = QApplication::focusWidget();
        if (key->key() == Qt::Key_Escape && focus && (focus == this || isAncestorOf(focus))) event->accept();
        break;
    }
    case QEvent::MouseButtonPress: case QEvent::TouchBegin: {
        const auto* target = qobject_cast<QWidget*>(watched);
        if (!target || QApplication::activePopupWidget()) break;
        if (target == this || isAncestorOf(target)) break;
        if (toggle_ && (target == toggle_ || toggle_->isAncestorOf(target))) break;  // the window's toggle decides
        if (window_ && target->window() == window_->window()) dismiss();
        break;
    }
    case QEvent::Resize: case QEvent::Move:
        if (watched == window_) place();
        break;
    case QEvent::LayoutRequest:
        if (watched == panel_) place();
        break;
    default: break;
    }
    return false;
}

void EditsOverlay::paintEvent(QPaintEvent*) {
    const auto& c = Theme::current();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(c.border, 1));
    p.setBrush(c.surface);
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), Ui::Metric::containerRadius, Ui::Metric::containerRadius);
}
}  // namespace putgd::record

#include "ReviewPanel.moc"
