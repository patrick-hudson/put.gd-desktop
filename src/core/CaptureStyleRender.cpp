#include "CaptureStyleRender.h"
#include <QBrush>
#include <QColorSpace>
#include <QFile>
#include <QImageReader>
#include <QLinearGradient>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include <mutex>
#include <vector>
#ifdef Q_OS_UNIX
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace putgd::style {
namespace {
// Three box passes over the alpha of a premultiplied ARGB image approximate a Gaussian of about `box`.
void blurAlpha(QImage& image, int box) {
    if (box < 1) return;
    const int width = image.width(), height = image.height();
    std::vector<int> line(size_t(std::max(width, height)));
    const auto pass = [&](auto at, int count) {
        // A running sum over 2*box+1 values, with the edges held at zero.
        long sum = 0;
        for (int i = 0; i < count; ++i) line[size_t(i)] = at(i);
        for (int i = 0; i <= box && i < count; ++i) sum += line[size_t(i)];
        for (int i = 0; i < count; ++i) {
            at(i, int(sum / (2 * box + 1)));
            if (i + box + 1 < count) sum += line[size_t(i + box + 1)];
            if (i - box >= 0) sum -= line[size_t(i - box)];
        }
    };
    for (int round = 0; round < 3; ++round) {
        for (int y = 0; y < height; ++y) {
            auto* row = reinterpret_cast<QRgb*>(image.scanLine(y));
            pass([&](int i, int value = -1) { if (value >= 0) row[i] = qRgba(0, 0, 0, value); return qAlpha(row[i]); }, width);
        }
        for (int x = 0; x < width; ++x)
            pass([&](int i, int value = -1) { auto* pixel = reinterpret_cast<QRgb*>(image.scanLine(i)) + x; if (value >= 0) *pixel = qRgba(0, 0, 0, value); return qAlpha(*pixel); }, height);
    }
}
// The shadow's colour: black at this opacity, the same on every backdrop.
constexpr int shadowAlpha = 96;
// A blurred rounded square holding the shadow's four corners and its edge profile. It is drawn
// as nine slices: the corners as they are, the edges stretched along their length. Its size
// depends on sigma and radius only, never on the capture, so a scrolling capture costs the same.
// The core is odd: two corner halves and the one-pixel middle column and row that the edges stretch.
struct ShadowTile { QImage image; int reach = 0, half = 0; };
ShadowTile shadowTile(double sigma, int radius) {
    static std::mutex lock;
    static double lastSigma = -1;
    static int lastRadius = -1;
    static ShadowTile last;
    std::lock_guard guard(lock);
    if (lastSigma == sigma && lastRadius == radius) return last;
    ShadowTile tile;
    tile.reach = int(std::ceil(3 * sigma)) + 1;
    tile.half = std::max(radius, 1) + 1;
    const int core = 2 * tile.half + 1;
    const int side = 2 * tile.reach + core;
    // Black with the coverage in alpha: blurring the alpha alone blurs the whole shadow.
    QImage image(side, side, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    {
        QPainter painter(&image);
        painter.setRenderHint(QPainter::Antialiasing);
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(0, 0, 0, shadowAlpha));
        painter.drawRoundedRect(QRectF(tile.reach, tile.reach, core, core), radius, radius);
    }
    blurAlpha(image, std::max(1, int(std::lround(sigma))));
    tile.image = image;
    lastSigma = sigma;lastRadius = radius;last = tile;
    return tile;
}
void paintShadow(QPainter& painter, const QRectF& box, double sigma, int radius) {
    if (sigma <= 0 || box.isEmpty()) return;
    const auto tile = shadowTile(sigma, radius);
    const double reach = tile.reach, half = tile.half;
    const QRectF outer = box.adjusted(-reach, -reach, reach, reach);
    const double side = tile.image.width();
    // Slice edges in the tile and on the canvas: a corner, the one-pixel middle stretched along the edge, a corner.
    const double sx[4] = {0, reach + half, reach + half + 1, side};
    const double dx[4] = {outer.left(), outer.left() + reach + half, outer.right() - reach - half, outer.right()};
    const double dy[4] = {outer.top(), outer.top() + reach + half, outer.bottom() - reach - half, outer.bottom()};
    if (dx[2] < dx[1] || dy[2] < dy[1]) {painter.drawImage(outer, tile.image);return;}
    painter.save();
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (int row = 0; row < 3; ++row)
        for (int column = 0; column < 3; ++column) {
            const QRectF target(QPointF(dx[column], dy[row]), QPointF(dx[column + 1], dy[row + 1]));
            const QRectF source(QPointF(sx[column], sx[row]), QPointF(sx[column + 1], sx[row + 1]));
            if (!target.isEmpty() && !source.isEmpty()) painter.drawImage(target, tile.image, source);
        }
    painter.restore();
}
}

CaptureStyle withoutPicture(const CaptureStyle& style) {
    if (style.backdrop != "picture") return style;
    auto fallback = CaptureStyle::preset("graphite");
    fallback.enabled = style.enabled;fallback.padding = style.padding;fallback.corners = style.corners;fallback.shadow = style.shadow;
    return fallback;
}

QImage loadPicture(const QString& path, const QSize& canvas) {
    if (path.isEmpty() || !canvas.isValid() || canvas.isEmpty()) return {};
    QFile file;
#ifdef Q_OS_UNIX
    const int fd = ::open(QFile::encodeName(path).constData(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    struct stat info {};
    if (fd < 0) return {};
    if (::fstat(fd, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size < 8 || info.st_size > maximumPictureFileBytes || !file.open(fd, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)) {::close(fd);return {};}
#else
    file.setFileName(path);
    if (!file.open(QIODevice::ReadOnly) || file.size() > maximumPictureFileBytes) return {};
#endif
    const auto head = file.peek(8);
    const char* format = head.startsWith("\x89PNG\r\n\x1a\n") ? "png" : head.startsWith("\xff\xd8\xff") ? "jpeg" : nullptr;
    if (!format) return {};
    // Only the PNG and JPEG readers; no plugin discovery for a named format.
    QImageReader reader(&file, format);
    reader.setAutoDetectImageFormat(false);
    reader.setAutoTransform(true);
    const auto size = reader.size();
    if (!size.isValid() || size.isEmpty() || qint64(size.width()) * size.height() > maximumPicturePixels || reader.supportsAnimation()) return {};
    // Scale to cover the canvas, never past maximumPictureEdge or the picture's own size.
    auto target = size.scaled(canvas, Qt::KeepAspectRatioByExpanding);
    if (target.width() > size.width() || target.height() > size.height()) target = size;
    if (std::max(target.width(), target.height()) > maximumPictureEdge) target.scale(maximumPictureEdge, maximumPictureEdge, Qt::KeepAspectRatio);
    reader.setScaledSize(target.expandedTo(QSize(1, 1)));
    reader.setAllocationLimit(int(std::min<qint64>(qint64(target.width()) * target.height() * 4 / (1024 * 1024) + 2, 1024)));
    auto image = reader.read();
    if (image.isNull()) return {};
    // Its pixels only, in sRGB, as the editor converts a source on load.
    if (image.colorSpace().isValid() && image.colorSpace() != QColorSpace(QColorSpace::SRgb)) image.convertToColorSpace(QColorSpace(QColorSpace::SRgb));
    return image.convertToFormat(QImage::Format_RGB32);
}

void paintBackdrop(QPainter& painter, const CaptureStyle& requested, const CaptureStyle::Geometry& geometry, const QSize& capture, const QImage& picture) {
    const auto style = requested.backdrop == "picture" && picture.isNull() ? withoutPicture(requested) : requested;
    const QRectF canvas(QPointF(0, 0), QSizeF(geometry.canvas));
    painter.save();
    if (style.backdrop == "color") painter.fillRect(canvas, QColor(style.color));
    else if (style.backdrop == "gradient") {
        // 135 degrees: from the top-left corner to the bottom-right one.
        QLinearGradient gradient(canvas.topLeft(), canvas.bottomRight());
        gradient.setColorAt(0, QColor(style.color));gradient.setColorAt(1, QColor(style.colorEnd));
        painter.fillRect(canvas, gradient);
    } else if (style.backdrop == "picture") {
        painter.fillRect(canvas, Qt::black);
        // Cover: fill the canvas, centred, cropped at the edges; never stretched.
        const auto covered = QSizeF(picture.size()).scaled(canvas.size(), Qt::KeepAspectRatioByExpanding);
        const QRectF target(QPointF((canvas.width() - covered.width()) / 2, (canvas.height() - covered.height()) / 2), covered);
        painter.setRenderHint(QPainter::SmoothPixmapTransform);
        painter.setClipRect(canvas, Qt::IntersectClip);
        painter.drawImage(target, picture);
    }
    const QRectF box(QPointF(geometry.padding, geometry.padding + geometry.offsetY), QSizeF(capture));
    paintShadow(painter, box, geometry.sigma, geometry.radius);
    painter.restore();
}

QPainterPath captureOutline(const CaptureStyle::Geometry& geometry, const QSize& capture) {
    QPainterPath path;
    const QRectF box(QPointF(geometry.padding, geometry.padding), QSizeF(capture));
    if (geometry.radius > 0) path.addRoundedRect(box, geometry.radius, geometry.radius);
    else path.addRect(box);
    return path;
}

QImage render(const QImage& source, const CaptureStyle& style, const CaptureStyle::Geometry& geometry, const QImage& picture) {
    if (source.isNull() || !geometry.valid()) return {};
    const bool opaque = style.backdrop != "none";
    QImage out(geometry.canvas, opaque ? QImage::Format_RGB32 : QImage::Format_ARGB32_Premultiplied);
    if (out.isNull()) return {};
    out.fill(opaque ? Qt::black : Qt::transparent);
    const auto capture = source.format() == QImage::Format_ARGB32_Premultiplied || source.format() == QImage::Format_RGB32 || source.format() == QImage::Format_ARGB32
                             ? source : source.convertToFormat(source.hasAlphaChannel() ? QImage::Format_ARGB32 : QImage::Format_RGB32);
    {
        QPainter painter(&out);
        paintBackdrop(painter, style, geometry, capture.size(), picture);
        if (geometry.radius > 0) {
            // The capture as a texture filling its rounded outline: antialiased corners, and each
            // pixel inside copied at its own position (an integer offset, no scaling).
            QBrush texture(capture);
            texture.setTransform(QTransform::fromTranslate(geometry.padding, geometry.padding));
            painter.setRenderHint(QPainter::Antialiasing);
            painter.setPen(Qt::NoPen);
            painter.setBrush(texture);
            painter.drawPath(captureOutline(geometry, capture.size()));
        } else painter.drawImage(QPoint(geometry.padding, geometry.padding), capture);
    }
    if (opaque) return out;
    auto result = out.convertToFormat(QImage::Format_ARGB32);
    for (int y = 0; y < result.height(); ++y) {
        auto* line = reinterpret_cast<QRgb*>(result.scanLine(y));
        for (int x = 0; x < result.width(); ++x) if (qAlpha(line[x]) == 0) line[x] = 0;
    }
    return result;
}
}
