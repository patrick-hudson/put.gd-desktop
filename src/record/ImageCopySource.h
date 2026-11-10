#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QPoint>
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
// the cursor, as KWin and Mutter recordings do: the compositor does, or, with
// `pointer` (#1364, click highlights on labwc), this source does. Then the
// session is created without paint_cursors, a pointer cursor session reports
// the cursor's position, hotspot and image, and each frame is copied and the
// cursor blended into the copy; a cursor that moves over a still screen pushes
// the last frame again with the cursor at its new place. That is the one way
// labwc tells a client where the pointer is: its cursor session reads the
// output's hardware cursor, which a painted-cursor capture takes away
// (wlroots locks software cursors for it). The compositor sends a frame
// only when the source changed (its damage); on a still screen the last frame
// is resent once a second so the clip's timeline keeps advancing, as
// pipewiresrc's keepalive does. Buffers carry the frame's presentation time
// (CLOCK_MONOTONIC); RecordingPipeline's cadence lock stamps them.
class ImageCopySource {
public:
 // With its own cursor (#1364) the source also pushes frames that only move the cursor over unchanged
 // content, at most this often (60 a second), flagged GST_BUFFER_FLAG_DROPPABLE: the cadence lock gives
 // such a frame a slot only at the slot's time, so it never takes one from the screen's next change.
 static constexpr qint64 cursorIntervalNs=16'000'000;
 struct Target {bool window=false;QString identity;};  // window with no identity: the active window now
 // pointer: an output's pointer cursor session can be asked for (a seat with a pointer).
 struct Facts {bool available=false,windows=false,pointer=false;int outputs=0;QString reason,windowReason;QJsonObject protocols;};
 // A connection, three round trips, the advertised globals; nothing is captured.
 static Facts probe();
 ImageCopySource();
 ~ImageCopySource();
 ImageCopySource(const ImageCopySource&)=delete;ImageCopySource& operator=(const ImageCopySource&)=delete;
 // Resolves the target and waits (bounded) for the first buffer constraints.
 // size: the stream's upright size (a rotated output's buffer turned back); identity: the
 // output name or window identifier.
 // pointer: draw the cursor here and know where it is (outputs only, upright ones);
 // *pointerOpened says whether that is what the stream does.
 // cursorHidden: no cursor in the frames at all (the scroll capture, #1376): the
 // session is created without paint_cursors and nothing draws one. It wins over pointer.
 Error open(const Target&,QSize* size,QString* identity,QString* appId=nullptr,bool pointer=false,bool* pointerOpened=nullptr,bool cursorHidden=false);
 // Where the pointer's hotspot is in the stream's pixels, any thread. False when the
 // stream does not track it, or the cursor is off this output or hidden by its client
 // right now; *ever says whether the compositor has reported it at all since open.
 bool pointer(QPoint* at,bool* ever=nullptr)const;
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
