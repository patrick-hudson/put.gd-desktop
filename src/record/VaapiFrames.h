#pragma once
#include <QFile>
#include <QSize>
#include <QStringList>
extern "C" {
#include <libavutil/hwcontext.h>
}
namespace putgd::record {
// A VA-API device and frame pool for `size`: the default render node first,
// then the others, since the display GPU may have no VA driver (an NVIDIA card
// beside an Intel or AMD GPU). Returns the first device whose pool initialises.
inline bool openVaapiFrames(const QSize& size, int poolSize, AVBufferRef*& device, AVBufferRef*& frames) {
    QStringList nodes{QString()};
    for (int i = 128; i < 136; ++i) { const auto node = QStringLiteral("/dev/dri/renderD%1").arg(i); if (QFile::exists(node)) nodes.append(node); }
    for (const auto& node : nodes) {
        if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, node.isEmpty() ? nullptr : node.toUtf8().constData(), nullptr, 0) < 0) { device = nullptr; continue; }
        frames = av_hwframe_ctx_alloc(device);
        if (frames) {
            auto* pool = reinterpret_cast<AVHWFramesContext*>(frames->data);
            pool->format = AV_PIX_FMT_VAAPI;
            pool->sw_format = AV_PIX_FMT_NV12;
            pool->width = size.width();
            pool->height = size.height();
            pool->initial_pool_size = poolSize;
            if (av_hwframe_ctx_init(frames) == 0) return true;
            av_buffer_unref(&frames);
        }
        av_buffer_unref(&device);
    }
    return false;
}
}
