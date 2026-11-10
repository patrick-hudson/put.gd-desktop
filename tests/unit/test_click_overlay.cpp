// #1364: the click ring drawn into a recording's frames, and where a click in
// desktop coordinates lands in the recorded picture. Pure pixel arithmetic: no
// capture, no encoder.
#include "record/ClickOverlay.h"
#include <QtTest>
#include <cmath>
#include <vector>

using namespace putgd::record;

namespace {
struct Frame {
    int width, height;
    std::vector<std::uint8_t> y, u, v, uv;
    Frame(int w, int h) : width(w), height(h), y(size_t(w) * h, 100), u(size_t(w / 2) * (h / 2), 128), v(size_t(w / 2) * (h / 2), 128), uv(size_t(w / 2) * (h / 2) * 2, 128) {}
    ClickOverlay::Plane luma() { return {y.data(), width, width, height, 1, 1, 1}; }
    ClickOverlay::Plane planarU() { return {u.data(), width / 2, width / 2, height / 2, 1, 2, 2}; }
    ClickOverlay::Plane planarV() { return {v.data(), width / 2, width / 2, height / 2, 1, 2, 2}; }
    ClickOverlay::Plane nv12U() { return {uv.data(), width, width / 2, height / 2, 2, 2, 2}; }
    ClickOverlay::Plane nv12V() { return {uv.data() + 1, width, width / 2, height / 2, 2, 2, 2}; }
    int lumaAt(int x, int y_) const { return y[size_t(y_) * width + x]; }
    int changed() const { int n = 0; for (auto value : y) n += value != 100; return n; }
};
constexpr std::int64_t ms = 1'000'000;
}

class ClickOverlayContract : public QObject {
    Q_OBJECT
private slots:
    void aRingOpensAtThePressAndFades() {
        ClickOverlay overlay;
        overlay.add(QPointF(160, 120), ClickOverlay::Left, 1000 * ms);
        QCOMPARE(overlay.received(), 1);
        // Nothing before the press and nothing once the ring has faded.
        QVERIFY(!overlay.active(999 * ms));
        QVERIFY(overlay.active(1000 * ms));
        QVERIFY(overlay.active(1000 * ms + ClickOverlay::lifeNs - 1));
        QVERIFY(!overlay.active(1000 * ms + ClickOverlay::lifeNs));
        Frame before(320, 240);
        overlay.draw(before.luma(), before.planarU(), before.planarV(), 990 * ms);
        QCOMPARE(before.changed(), 0);
        Frame after(320, 240);
        overlay.draw(after.luma(), after.planarU(), after.planarV(), 1000 * ms + ClickOverlay::lifeNs);
        QCOMPARE(after.changed(), 0);
        // Early: a small ring around the point, its centre untouched, in the button's colour.
        Frame early(320, 240);
        overlay.draw(early.luma(), early.planarU(), early.planarV(), 1010 * ms);
        const auto amber = ClickOverlay::colour(ClickOverlay::Left);
        QCOMPARE(early.lumaAt(160, 120), 100);
        int nearest = 1000, farthest = 0;
        for (int x = 160; x < 320; ++x) if (early.lumaAt(x, 120) != 100) { nearest = std::min(nearest, x - 160); farthest = std::max(farthest, x - 160); }
        QVERIFY2(nearest >= 3 && farthest <= 14, qPrintable(QString("%1..%2").arg(nearest).arg(farthest)));
        bool reachedColour = false;
        for (int x = 160; x < 180; ++x) reachedColour = reachedColour || std::abs(early.lumaAt(x, 120) - amber[0]) <= 2;
        QVERIFY(reachedColour);
        // Later: the ring is wider and, in its second half, fainter.
        Frame late(320, 240);
        overlay.draw(late.luma(), late.planarU(), late.planarV(), 1000 * ms + ClickOverlay::lifeNs * 9 / 10);
        int lateFarthest = 0, strongest = 0;
        for (int x = 160; x < 320; ++x) if (late.lumaAt(x, 120) != 100) { lateFarthest = std::max(lateFarthest, x - 160); strongest = std::max(strongest, std::abs(late.lumaAt(x, 120) - 100)); }
        QVERIFY2(lateFarthest > farthest + 6, qPrintable(QString::number(lateFarthest)));
        QVERIFY2(strongest < std::abs(amber[0] - 100) / 2, qPrintable(QString::number(strongest)));
        // The ring is a ring in every direction.
        Frame round(320, 240);
        overlay.draw(round.luma(), round.planarU(), round.planarV(), 1200 * ms);
        int right = 0, down = 0;
        for (int d = 0; d < 60; ++d) { if (round.lumaAt(160 + d, 120) != 100) right = d; if (round.lumaAt(160, 120 + d) != 100) down = d; }
        QVERIFY(right > 10 && std::abs(right - down) <= 1);
        QVERIFY(overlay.drawn() >= 3);
    }
    void chromaCarriesTheButtonColourInBothFrameLayouts() {
        for (const int button : {ClickOverlay::Left, ClickOverlay::Middle, ClickOverlay::Right}) {
            ClickOverlay overlay;
            overlay.add(QPointF(100, 100), button, 0);
            Frame planar(200, 200), semi(200, 200);
            overlay.draw(planar.luma(), planar.planarU(), planar.planarV(), 200 * ms);
            overlay.draw(semi.luma(), semi.nv12U(), semi.nv12V(), 200 * ms);
            QCOMPARE(planar.y, semi.y);
            // NV12 interleaves what I420 keeps apart: the same chroma, sample for sample.
            int touched = 0;
            for (size_t i = 0; i < planar.u.size(); ++i) {
                QCOMPARE(int(semi.uv[2 * i]), int(planar.u[i]));
                QCOMPARE(int(semi.uv[2 * i + 1]), int(planar.v[i]));
                touched += planar.u[i] != 128 || planar.v[i] != 128;
            }
            const auto colour = ClickOverlay::colour(button);
            // The off-white ring barely moves the chroma; the two coloured buttons move it their own way.
            int furthest = 0;
            for (size_t i = 0; i < planar.u.size(); ++i) furthest = std::max({furthest, std::abs(int(planar.u[i]) - 128), std::abs(int(planar.v[i]) - 128)});
            if (button == ClickOverlay::Middle) QVERIFY2(furthest <= 3, qPrintable(QString::number(furthest)));
            else QVERIFY2(touched > 20 && furthest > 20, qPrintable(QString("%1 %2").arg(touched).arg(furthest)));
            QVERIFY(colour[0] > 140);
        }
        QVERIFY(ClickOverlay::colour(ClickOverlay::Left) != ClickOverlay::colour(ClickOverlay::Right));
    }
    void ringsNearAnEdgeStayInsideTheFrame() {
        ClickOverlay overlay;
        overlay.setScale(2.0);
        for (const auto& point : {QPointF(0, 0), QPointF(319, 239), QPointF(-30, 100), QPointF(400, 400)}) overlay.add(point, ClickOverlay::Left, 0);
        // Guard bytes around each plane would show a write outside it.
        Frame frame(320, 240);
        frame.y.insert(frame.y.begin(), 320, 7); frame.y.insert(frame.y.end(), 320, 7);
        ClickOverlay::Plane luma{frame.y.data() + 320, 320, 320, 240, 1, 1, 1};
        overlay.draw(luma, frame.planarU(), frame.planarV(), 300 * ms);
        for (int i = 0; i < 320; ++i) { QCOMPARE(int(frame.y[size_t(i)]), 7); QCOMPARE(int(frame.y[frame.y.size() - 1 - size_t(i)]), 7); }
        // Twice the density, twice the ring.
        ClickOverlay small, large;
        large.setScale(2.0);
        small.add(QPointF(160, 120), 1, 0); large.add(QPointF(160, 120), 1, 0);
        Frame a(320, 240), b(320, 240);
        small.draw(a.luma(), a.planarU(), a.planarV(), 440 * ms); large.draw(b.luma(), b.planarU(), b.planarV(), 440 * ms);
        int reachA = 0, reachB = 0;
        for (int d = 0; d < 120; ++d) { if (a.lumaAt(160 + d, 120) != 100) reachA = d; if (b.lumaAt(160 + d, 120) != 100) reachB = d; }
        QVERIFY2(reachB > reachA * 3 / 2, qPrintable(QString("%1 %2").arg(reachA).arg(reachB)));
    }
    void aBurstKeepsTheNewestClicksAndClearForgetsThem() {
        ClickOverlay overlay;
        for (int i = 0; i < 100; ++i) overlay.add(QPointF(10 + i, 10), 1, i * ms);
        QCOMPARE(overlay.received(), 100);
        QVERIFY(overlay.active(100 * ms));
        overlay.clear();
        QCOMPARE(overlay.received(), 0);
        QVERIFY(!overlay.active(100 * ms));
        Frame frame(320, 240);
        overlay.draw(frame.luma(), frame.planarU(), frame.planarV(), 100 * ms);
        QCOMPARE(frame.changed(), 0);
    }
    void clicksMapFromTheDesktopIntoThePicture() {
        QPointF at;
        // A region at (300, 250) recorded pixel for pixel.
        QVERIFY(mapClick(QPointF(940, 610), QRectF(300, 250, 1280, 720), QSize(1280, 720), &at));
        QCOMPARE(at, QPointF(640, 360));
        // Outside the recorded area: not in the clip.
        QVERIFY(!mapClick(QPointF(299, 400), QRectF(300, 250, 1280, 720), QSize(1280, 720), &at));
        QVERIFY(!mapClick(QPointF(1600, 400), QRectF(300, 250, 1280, 720), QSize(1280, 720), &at));
        // A logical area on a 2x output: the canvas has twice the pixels.
        QVERIFY(mapClick(QPointF(110, 60), QRectF(100, 50, 640, 360), QSize(1280, 720), &at));
        QCOMPARE(at, QPointF(20, 20));
        // A window that shrank sits centred in its starting canvas, with borders.
        QVERIFY(mapClick(QPointF(500, 400), QRectF(400, 300, 400, 300), QSize(800, 300), &at));
        QCOMPARE(at, QPointF(300, 100));
        // No area, no click (a window recording whose window is unknown).
        QVERIFY(!mapClick(QPointF(1, 1), QRectF(), QSize(800, 600), &at));
        QVERIFY(!mapClick(QPointF(1, 1), QRectF(0, 0, 10, 10), QSize(), &at));
    }
};
QTEST_APPLESS_MAIN(ClickOverlayContract)
#include "test_click_overlay.moc"
