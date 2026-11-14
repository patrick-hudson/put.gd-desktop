// The scroll-capture fixture window (#1376): a frameless window at a fixed
// place showing the bar-coded ruler page of ScrollRuler.h, scrolled by the
// wheel and by nothing else. Options: a sticky header and footer, smooth
// scrolling, pixels per notch, a starting row, a tail that loads late, and an
// application that answers the wheel late (`--answer-ms`), and a page that
// a person scrolls (`--self-scroll PX,MS,WAIT`: PX every MS after WAIT ms,
// down to the end, with no input event at all).
// It prints `ready X Y W H` once it has painted, then `position N` whenever
// the scroll position settles. On a Wayland desktop a client cannot place its
// own window, so `--fullscreen` fills one output instead and the capture takes
// a full-width band of it. Test input only; never desktop proof.
#include "fixtures/ScrollRuler.h"
#include <QApplication>
#include <QCommandLineParser>
#include <QFont>
#include <QPainter>
#include <QTimer>
#include <QVariantAnimation>
#include <QWheelEvent>
#include <QWidget>
#include <algorithm>
#include <cstdio>

namespace ruler = putgd::test::ruler;

class Page final : public QWidget {
public:
    int rows = 60, notch = 100, header = 0, footer = 0, lazyRows = 0, lazyMs = 250, answerMs = 0;
    bool smooth = false;
    explicit Page() : QWidget(nullptr, Qt::FramelessWindowHint | Qt::X11BypassWindowManagerHint | Qt::WindowDoesNotAcceptFocus) {
        setAttribute(Qt::WA_OpaquePaintEvent);
        setAttribute(Qt::WA_ShowWithoutActivating);
        animation_.setEasingCurve(QEasingCurve::OutCubic);
        connect(&animation_, &QVariantAnimation::valueChanged, this, [this](const QVariant& value) { setPosition(value.toInt()); });
        connect(&animation_, &QVariantAnimation::finished, this, [this] { settled(); });
        lazy_.setSingleShot(true);
        connect(&lazy_, &QTimer::timeout, this, [this] { loaded_ = true; update(); });
        answer_.setSingleShot(true);
        connect(&answer_, &QTimer::timeout, this, [this] { setPosition(target_); settled(); });
    }
    void start(int row) {
        loaded_ = lazyRows <= 0;
        position_ = target_ = std::clamp(row * ruler::rowHeight, 0, maxScroll());
    }
    // As a person scrolls: by px every ms, beginning after wait ms, to the end.
    void selfScroll(int px, int ms, int wait) {
        connect(&self_, &QTimer::timeout, this, [this, px] {
            target_ = std::clamp(target_ + px, 0, maxScroll());
            setPosition(target_);
            settled();
            if (target_ >= maxScroll()) self_.stop();
        });
        QTimer::singleShot(wait, this, [this, ms] { self_.start(ms); });
    }
protected:
    void wheelEvent(QWheelEvent* event) override {
        // One notch is 120 units of angle delta; a positive delta scrolls up.
        // In proportion, as applications scroll: a desktop may deliver a
        // fraction of a notch or several at once (XWayland turns KWin's 15.0
        // into one and a half).
        const int pixels = -event->angleDelta().y() * notch / 120;
        if (!pixels) return;
        target_ = std::clamp(target_ + pixels, 0, maxScroll());
        if (smooth) {
            animation_.stop();
            animation_.setDuration(160);
            animation_.setStartValue(position_);
            animation_.setEndValue(target_);
            animation_.start();
        } else if (answerMs > 0) {
            // The page moves this long after the last wheel step of a burst.
            answer_.start(answerMs);
        } else {
            setPosition(target_);
            settled();
        }
        event->accept();
    }
    void paintEvent(QPaintEvent*) override {
        QPainter painter(this);
        const int w = width(), h = height(), cell = ruler::cellWidth(w), left = ruler::codeLeft();
        const int top = header, bottom = h - footer, count = visibleRows();
        painter.fillRect(rect(), QColor::fromRgb(ruler::light));
        QFont font = painter.font();
        font.setPixelSize(13);
        painter.setFont(font);
        for (int y = top; y < bottom; ++y) {
            const int line = position_ + (y - top);
            const int row = line / ruler::rowHeight, within = line % ruler::rowHeight;
            if (row >= count) break;
            const uint16_t bits = ruler::pattern(row, within);
            for (int c = 0; c < ruler::cells; ++c)
                if (bits & (0x8000 >> c)) painter.fillRect(left + c * cell, y, cell, 1, QColor::fromRgb(ruler::dark));
        }
        // The row numbers, in the label column (never decoded).
        painter.setPen(QColor::fromRgb(ruler::dark));
        painter.setClipRect(0, top, left, bottom - top);
        for (int row = position_ / ruler::rowHeight; row < count && row * ruler::rowHeight - position_ < bottom - top; ++row)
            painter.drawText(QRect(4, top + row * ruler::rowHeight - position_ + 6, left - 8, 20), Qt::AlignLeft | Qt::AlignVCenter, QString::number(row));
        painter.setClipping(false);
        // The scrollbar: a thumb whose place says how far down the page is.
        const int track = bottom - top, total = std::max(1, count * ruler::rowHeight);
        const int thumb = std::max(24, int(qint64(track) * track / std::max(track, total)));
        const int thumbTop = top + (maxScroll() > 0 ? int(qint64(position_) * (track - thumb) / maxScroll()) : 0);
        painter.fillRect(w - ruler::scrollbarWidth, top, ruler::scrollbarWidth, track, QColor(0xe0, 0xe0, 0xe0));
        painter.fillRect(w - ruler::scrollbarWidth + 2, thumbTop, ruler::scrollbarWidth - 4, thumb, QColor(0x80, 0x80, 0x80));
        if (header) painter.fillRect(0, 0, w, header, QColor::fromRgb(ruler::headerColor));
        if (footer) painter.fillRect(0, bottom, w, footer, QColor::fromRgb(ruler::footerColor));
        if (!announced_) {
            announced_ = true;
            // After this paint has reached the X server.
            QTimer::singleShot(50, this, [this] { std::printf("ready %d %d %d %d\n", x(), y(), width(), height()); std::fflush(stdout); });
        }
    }
private:
    int visibleRows() const { return loaded_ ? rows : std::max(0, rows - lazyRows); }
    int maxScroll() const { return std::max(0, visibleRows() * ruler::rowHeight - (height() - header - footer)); }
    void setPosition(int position) {
        if (position == position_) return;
        position_ = position;
        update();
    }
    void settled() {
        std::printf("position %d\n", position_);
        std::fflush(stdout);
        // The tail arrives a while after the page reached its (first) end.
        if (!loaded_ && position_ >= maxScroll() && !lazy_.isActive()) lazy_.start(lazyMs);
    }
    QVariantAnimation animation_;
    QTimer lazy_, answer_, self_;
    int position_ = 0, target_ = 0;
    bool loaded_ = true, announced_ = false;
};

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.addOptions({{"geometry", "X,Y,W,H in desktop pixels", "rect", "100,60,640,600"}, {"rows", "rows on the page", "n", "60"}, {"notch", "pixels per wheel notch", "px", "100"},
        {"header", "sticky header height", "px", "0"}, {"footer", "sticky footer height", "px", "0"}, {"start", "the row at the top at start", "row", "0"},
        {"lazy-tail", "rows that load only after the end was reached", "n", "0"}, {"lazy-ms", "how long the tail takes", "ms", "250"}, {"smooth", "animate each scroll"}, {"answer-ms", "move this long after the wheel", "ms", "0"},
        {"fullscreen", "fill the output instead of taking --geometry"}, {"self-scroll", "scroll by itself as a person would: PX every MS after WAIT ms", "px,ms,wait"}});
    parser.process(app);
    const auto geometry = parser.value("geometry").split(',');
    if (geometry.size() != 4) return 2;
    Page page;
    page.rows = parser.value("rows").toInt();
    page.notch = parser.value("notch").toInt();
    page.header = parser.value("header").toInt();
    page.footer = parser.value("footer").toInt();
    page.lazyRows = parser.value("lazy-tail").toInt();
    page.lazyMs = parser.value("lazy-ms").toInt();
    page.smooth = parser.isSet("smooth");
    page.answerMs = parser.value("answer-ms").toInt();
    page.setGeometry(geometry[0].toInt(), geometry[1].toInt(), geometry[2].toInt(), geometry[3].toInt());
    page.start(parser.value("start").toInt());
    if (parser.isSet("self-scroll")) {
        const auto parts = parser.value("self-scroll").split(',');
        if (parts.size() != 3) return 2;
        page.selfScroll(parts[0].toInt(), parts[1].toInt(), parts[2].toInt());
    }
    if (parser.isSet("fullscreen")) {
        // An ordinary window for the compositor to make full screen.
        page.setWindowFlags(Qt::Window);
        page.showFullScreen();
    } else page.show();
    return app.exec();
}
