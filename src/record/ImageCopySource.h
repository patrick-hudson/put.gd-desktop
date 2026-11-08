#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QSize>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <gst/gst.h>

namespace putgd::record {
// The dialog-free Wayland acquisition for compositors that offer
// ext-image-copy-capture-v1 and no screencast service: Budgie 10.10 on labwc,
// Magpie, and others by protocol, never by name. Frames of one output or one
// window go into the pipeline's appsrc from a capture thread on the helper's
// own Wayland connection, through a small reused wl_shm ring. Recordings paint
// the cursor, as KWin and Mutter recordings do. The compositor sends a frame
// only when the source changed (its damage); on a still screen the last frame
// is resent once a second so the clip's timeline keeps advancing, as
// pipewiresrc's keepalive does. Buffers carry the frame's presentation time
// (CLOCK_MONOTONIC); RecordingPipeline's cadence lock stamps them.
class ImageCopySource {
public:
 struct Target {bool window=false;QString identity;};  // window with no identity: the active window now
 struct Facts {bool available=false,windows=false;QString reason,windowReason;QJsonObject protocols;};
 // A connection, three round trips, the advertised globals; nothing is captured.
 static Facts probe();
 ImageCopySource();
 ~ImageCopySource();
 ImageCopySource(const ImageCopySource&)=delete;ImageCopySource& operator=(const ImageCopySource&)=delete;
 // Resolves the target and waits (bounded) for the first buffer constraints.
 // size: the stream's upright size (a rotated output's buffer turned back); identity: the
 // output name or window identifier.
 Error open(const Target&,QSize* size,QString* identity,QString* appId=nullptr);
 // The GStreamer caps the stream's frames carry (video/x-raw, format and size).
 QString caps()const;
 // Starts pushing frames; lost() runs on the capture thread when the source
 // ends (the window closed, the output went away, the compositor stopped it).
 void attach(GstElement* appsrc,std::function<void(Error)> lost);
 void stop();
 QJsonObject status()const;
private:
 struct Impl;
 std::unique_ptr<Impl> d_;
};
}
