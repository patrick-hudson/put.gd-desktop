// Not a test and not shipped. Draws the review timeline the way
// mock/review4.html does (1160 px wide; a trim, one cut, two covers with one
// selected, the playhead and a skim flag) in both themes, with placeholder
// tiles and with generated pictures, at 2x so details are readable.
//   render_filmstrip_timeline <out-dir>      (PUTGD_TEXT_SCALE=1.5 for 150 % text)
#include "record/FilmstripTimeline.h"
#include "ui/Fonts.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QApplication>
#include <QImage>
#include <QMouseEvent>
#include <QPainter>
#include <cmath>
#include <cstdio>

using namespace putgd;
using namespace putgd::record;

namespace {
constexpr qint64 duration = 32000000;
// A stand-in for recorded footage: wallpaper, a browser window and a sign-in card.
QImage footage(qint64 us) {
    QImage image(128, 72, QImage::Format_ARGB32_Premultiplied);
    const double t = us / 1e6;
    QPainter p(&image);
    p.setRenderHint(QPainter::Antialiasing);
    QLinearGradient wallpaper(0, 0, 128, 72);
    wallpaper.setColorAt(0, QColor("#26314a"));
    wallpaper.setColorAt(1, QColor("#141b28"));
    p.fillRect(image.rect(), wallpaper);
    p.fillRect(QRect(0, 66, 128, 6), QColor("#2b313d"));
    if (t >= 2) {
        p.setBrush(QColor("#f4f4f2"));
        p.setPen(Qt::NoPen);
        p.drawRoundedRect(QRectF(8, 4, 112, 60), 3, 3);
        if (t < 12.6) {
            p.setBrush(Qt::white);
            p.drawRoundedRect(QRectF(34, 16, 60, 40), 3, 3);
            p.setBrush(QColor("#3b6cf0"));
            p.drawRect(QRectF(40, 46, 48, 5));
            p.setBrush(QColor("#d4d4ce"));
            p.drawRect(QRectF(40, 28, 48, 4));
            p.drawRect(QRectF(40, 37, 48, 4));
        } else if (t < 19.5) {
            p.setBrush(QColor("#e6e6e1"));
            p.drawRect(QRectF(16, 24, 96, 3));
            p.setBrush(QColor("#3b6cf0"));
            p.drawRect(QRectF(16, 24, 96 * (t - 12.6) / 6.9, 3));
        } else {
            p.setBrush(QColor("#e9f5ee"));
            p.drawRect(QRectF(16, 14, 96, 16));
            p.setBrush(QColor("#2f9e5d"));
            p.drawEllipse(QRectF(20, 17, 9, 9));
        }
    }
    return image;
}
RecordingEdit edit() {
    RecordingEdit e;
    e.trimStart = 2600000;
    e.trimEnd = 29400000;
    e.cuts = {RecordingCut{"c1", 13000000, 19500000}};
    RecordingMask redact, blur;
    redact.id = "m1"; redact.kind = "solid"; redact.start = 8200000; redact.end = 12600000;
    blur.id = "m2"; blur.kind = "blur"; blur.start = 22100000; blur.end = 24900000;
    e.masks = {redact, blur};
    return e;
}
void render(const QString& path, bool tiles, bool selectCut) {
    FilmstripTimeline timeline(std::make_shared<MaskNames>());
    timeline.setDuration(duration, 30);
    timeline.setEdit(edit());
    timeline.setSelectedEdit(selectCut ? "cut" : "cover", selectCut ? "c1" : "m1");
    timeline.setPlayhead(10400000);
    if (tiles) timeline.setTileSource(footage);
    timeline.resize(1160, timeline.sizeHint().height());
    timeline.show();
    // Hover at 5.3 s: the skim line and its flag.
    QMouseEvent move(QEvent::MouseMove, QPointF(timeline.xFor(5300000), timeline.stripRect().center().y()), QPointF(), Qt::NoButton, Qt::NoButton, Qt::NoModifier);
    QApplication::sendEvent(&timeline, &move);
    QImage image(timeline.size() * 2, QImage::Format_ARGB32_Premultiplied);
    image.setDevicePixelRatio(2);
    image.fill(Theme::current().bg);
    timeline.render(&image);
    image.save(path);
    std::printf("%s %dx%d (widget height %d)\n", qPrintable(path), image.width(), image.height(), timeline.height());
}
}

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    Fonts::load();
    Ui::install(app);
    const QString out = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral(".");
    const QString scale = Ui::textScale() > 1.2 ? QStringLiteral("150") : QStringLiteral("100");
    for (const auto scheme : {Theme::Scheme::Dark, Theme::Scheme::Light}) {
        Theme::setScheme(scheme);
        for (const bool tiles : {false, true}) {
            const QString base = QStringLiteral("%1/timeline-%2-%3-%4").arg(out, Theme::name(scheme), scale, tiles ? "tiles" : "placeholders");
            render(base + ".png", tiles, false);
        }
        render(QStringLiteral("%1/timeline-%2-%3-cut-selected.png").arg(out, Theme::name(scheme), scale), true, true);
    }
    return 0;
}
