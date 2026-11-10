#include "ClickCheck.h"
#include "ClipExporter.h"
#include <QImage>
#include <QJsonArray>
#include <QRect>
#include <algorithm>
#include <cmath>
#include <vector>

namespace putgd::record {
Error measureClicks(const QString& clip, const QList<ExpectedClick>& clicks, QJsonObject& result, int windowMs, int box, int floor) {
    qint64 durationUs = 0;
    QSize canvas;
    if (auto error = probeClip(clip, durationUs, canvas)) return error;
    struct Watch {
        ExpectedClick click;
        QRect area;
        QImage first;
        qint64 firstMs = -1, lastMs = -1, peakMs = -1;
        int peak = 0, frames = 0;
        double dx = 0, dy = 0, r = 0, g = 0, b = 0;
    };
    std::vector<Watch> watches;
    qint64 startMs = durationUs / 1000;
    for (const auto& click : clicks) {
        Watch watch;
        watch.click = click;
        watch.area = QRect(click.x - box, click.y - box, 2 * box + 1, 2 * box + 1).intersected(QRect(QPoint(0, 0), canvas));
        if (watch.area.isEmpty()) return {"invalid_request", QStringLiteral("The press at %1,%2 is outside the %3x%4 clip").arg(click.x).arg(click.y).arg(canvas.width()).arg(canvas.height()), {}};
        startMs = std::min(startMs, std::max<qint64>(0, click.ms - windowMs));
        watches.push_back(watch);
    }
    FrameSequence frames;
    if (auto error = frames.open(clip, startMs * 1000)) return error;
    QImage image;
    qint64 frameUs = 0;
    Error error;
    const int edge = std::max(canvas.width(), canvas.height());
    while (frames.next({}, edge, canvas, image, frameUs, &error)) {
        const qint64 ms = frameUs / 1000;
        bool later = false;
        for (auto& watch : watches) {
            if (ms > watch.click.ms + windowMs + 1200) continue;
            later = true;
            if (ms < watch.click.ms - windowMs) continue;
            const QImage part = image.copy(watch.area).convertToFormat(QImage::Format_RGB32);
            if (watch.first.isNull()) { watch.first = part; continue; }
            if (part.size() != watch.first.size()) continue;
            ++watch.frames;
            int changed = 0;
            double sx = 0, sy = 0, r = 0, g = 0, b = 0;
            for (int y = 0; y < part.height(); ++y) {
                const auto* now = reinterpret_cast<const QRgb*>(part.constScanLine(y));
                const auto* before = reinterpret_cast<const QRgb*>(watch.first.constScanLine(y));
                for (int x = 0; x < part.width(); ++x) {
                    if (std::abs(qRed(now[x]) - qRed(before[x])) + std::abs(qGreen(now[x]) - qGreen(before[x])) + std::abs(qBlue(now[x]) - qBlue(before[x])) <= 90) continue;
                    ++changed; sx += x; sy += y; r += qRed(now[x]); g += qGreen(now[x]); b += qBlue(now[x]);
                }
            }
            if (changed < floor) continue;
            if (watch.firstMs < 0) watch.firstMs = ms;
            watch.lastMs = ms;
            if (changed > watch.peak) {
                watch.peak = changed; watch.peakMs = ms;
                watch.dx = watch.area.x() + sx / changed - watch.click.x;
                watch.dy = watch.area.y() + sy / changed - watch.click.y;
                watch.r = r / changed; watch.g = g / changed; watch.b = b / changed;
            }
        }
        if (!later) break;
    }
    if (error) return error;
    QJsonArray list;
    int seen = 0;
    for (const auto& watch : watches) {
        QJsonObject entry{{"ms", double(watch.click.ms)}, {"x", watch.click.x}, {"y", watch.click.y}, {"seen", watch.firstMs >= 0}, {"frames", watch.frames}};
        if (watch.firstMs >= 0) {
            ++seen;
            entry["firstMs"] = double(watch.firstMs);
            entry["lastMs"] = double(watch.lastMs);
            entry["offsetMs"] = double(watch.firstMs - watch.click.ms);
            entry["lengthMs"] = double(watch.lastMs - watch.firstMs);
            entry["peak"] = watch.peak;
            entry["peakMs"] = double(watch.peakMs);
            entry["dx"] = std::round(watch.dx * 10) / 10;
            entry["dy"] = std::round(watch.dy * 10) / 10;
            entry["rgb"] = QJsonArray{int(std::lround(watch.r)), int(std::lround(watch.g)), int(std::lround(watch.b))};
        }
        list.append(entry);
    }
    result = QJsonObject{{"clicks", list}, {"seen", seen}, {"expected", int(clicks.size())}, {"canvas", QJsonArray{canvas.width(), canvas.height()}}, {"durationMs", double(durationUs / 1000)}};
    return {};
}
}
