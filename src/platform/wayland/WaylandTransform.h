#pragma once
#include <QImage>
#include <QSize>
#include <QTransform>
#include <wayland-client-protocol.h>

namespace putgd {
// The compositor fills a capture buffer in its own orientation; wl_output.transform
// (or the frame's transform event) says how that buffer was turned. Shared by the
// resident's screenshots (ImageCopyCapture) and the recorder (ImageCopySource), so
// both undo it the same way: turn by the transform's angle, then mirror a flipped
// one (checked against grim for every transform in test_budgie_labwc).
inline QImage waylandUpright(const QImage& image,uint32_t transform){
 if(transform==WL_OUTPUT_TRANSFORM_NORMAL||transform>WL_OUTPUT_TRANSFORM_FLIPPED_270)return image;
 QTransform turn;turn.rotate(90.0*(transform%4));
 auto out=(transform%4)?image.transformed(turn):image.copy();
 if(transform>=WL_OUTPUT_TRANSFORM_FLIPPED)out=out.mirrored(true,false);
 return out;
}
// The upright size of a buffer: a quarter turn swaps width and height.
inline QSize waylandUprightSize(QSize buffer,uint32_t transform){return transform<=WL_OUTPUT_TRANSFORM_FLIPPED_270&&(transform%2)?buffer.transposed():buffer;}
}
