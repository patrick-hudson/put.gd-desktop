#pragma once
#include "CaptureStyle.h"
#include <QImage>
#include <QPainterPath>
#include <QRectF>

class QPainter;
// #1893: drawing a capture style. One code path for the editor's export, its canvas preview,
// Settings' preview and the image-job child's `style` task, so they all agree. It is its own
// library (putgd_capture_style_render), which only putgd-ui links; the resident never paints or decodes.
namespace putgd::style {
// The largest edge a backdrop picture is decoded to: enough for a backdrop, never a full
// photo's raster.
constexpr int maximumPictureEdge = 4096;
constexpr qint64 maximumPictureFileBytes = 64LL * 1024 * 1024;
constexpr qint64 maximumPicturePixels = 100000000LL;
// The picture at `path` decoded to cover `canvas` (scaled to fill it, centred, cropped by
// the painter), at most maximumPictureEdge on its longer side. Null when it is missing, too
// big or unreadable. Read with O_NOFOLLOW; only a regular file is opened.
QImage loadPicture(const QString& path, const QSize& canvas);
// The backdrop and the shadow, in output pixels: the canvas is (0, 0, geometry.canvas) and
// the capture sits at (padding, padding). The caller's painter may carry any transform.
// A picture style with a null picture paints Graphite instead.
void paintBackdrop(QPainter&, const CaptureStyle&, const CaptureStyle::Geometry&, const QSize& capture, const QImage& picture);
// The capture's rounded outline at (padding, padding).
QPainterPath captureOutline(const CaptureStyle::Geometry&, const QSize& capture);
// The styled image: backdrop, shadow, then the capture, its corners rounded with antialiasing.
// Inside the rounded outline every capture pixel is copied unchanged. Fully transparent pixels
// have their hidden colour cleared. Null when it can't be allocated.
QImage render(const QImage& capture, const CaptureStyle&, const CaptureStyle::Geometry&, const QImage& picture);
// The style a picture backdrop falls back to when its picture can't be read: Graphite.
CaptureStyle withoutPicture(const CaptureStyle&);
}
