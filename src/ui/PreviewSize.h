#pragma once
#include <QSize>
#include <algorithm>

namespace putgd::Ui {
// How big the History window's expanded preview may be, apart from any widget so a test can give it the screens
// that went wrong. A screen whose work area is empty (Cinnamon reports one while it lays its outputs out) or only a
// strip (1920x48) leaves no room for a picture; the screen's whole geometry is the next choice, however small, and a
// fixed size the last, when the screen is unknown too.
inline constexpr QSize kPreviewMinimumPicture{200, 150};
inline constexpr QSize kPreviewFallbackArea{1000, 700};
inline constexpr int kPreviewScreenSlack = 48;  // the title bar and the frame the window manager adds

struct PreviewPlan {
    QSize picture;  // the image as it is shown
    QSize dialog;   // the whole dialog: the picture plus margin (width) and overhead (height)
};

// margin and overhead are what the dialog adds around the picture, horizontally and vertically.
inline QSize previewArea(const QSize& workArea, const QSize& screenGeometry, int margin, int overhead) {
    const auto roomFor = [&](const QSize& area) {
        return area.width() - kPreviewScreenSlack - margin >= kPreviewMinimumPicture.width()
            && area.height() - kPreviewScreenSlack - overhead >= kPreviewMinimumPicture.height();
    };
    if (roomFor(workArea)) return workArea;
    // A known screen is always fitted to, even a small logical one (1280x720 at 250% is 512x288 to Qt).
    if (!screenGeometry.isEmpty()) return screenGeometry;
    return kPreviewFallbackArea;
}

inline PreviewPlan planExpandedPreview(const QSize& image, const QSize& workArea, const QSize& screenGeometry,
                                       int margin, int overhead, int maxWidth, int maxHeight) {
    const QSize area = previewArea(workArea, screenGeometry, margin, overhead);
    const QSize limit(std::max(1, std::min(maxWidth, area.width() - kPreviewScreenSlack - margin)),
                      std::max(1, std::min(maxHeight, area.height() - kPreviewScreenSlack - overhead)));
    QSize picture = image;
    if (picture.width() > limit.width() || picture.height() > limit.height()) picture.scale(limit, Qt::KeepAspectRatio);
    picture = picture.expandedTo(QSize(1, 1));  // a very thin image scales to nothing
    return {picture, picture + QSize(margin, overhead)};
}
}  // namespace putgd::Ui
