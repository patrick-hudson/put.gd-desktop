#pragma once
#include "core/Error.h"
#include "core/Settings.h"
#include "ToplevelMatch.h"
#include <QImage>
#include <QJsonObject>
#include <QObject>
#include <functional>
#include <memory>
#include <optional>

namespace putgd {
// Screen and window pixels through ext-image-copy-capture-v1 on an independent,
// Core-compatible Wayland connection. Nothing here names a compositor: every
// capability comes from the advertised globals, so any compositor that offers
// these protocols (labwc, Mir, Hyprland) gets the same behaviour.
//
// Windows come from ext-foreign-toplevel-list-v1, the only list that connects to
// capture. Only wlr-foreign-toplevel-management reports the activated window, so
// the active window is the one ext handle whose app ID and title match the one
// activated wlr handle. Neither protocol reports a PID or geometry.
//
// Screenshots never include the cursor. Each capture maps one wl_shm buffer and
// releases the compositor buffer and pool when the frame is ready; the returned
// image owns the mapping until its last copy is dropped.
class ImageCopyCapture final:public QObject {
 Q_OBJECT
public:
 using Toplevel=WaylandToplevel;
 // image carries density 1; a rotated output's buffer is already turned upright.
 using Reply=std::function<void(QImage,Error)>;
 explicit ImageCopyCapture(QObject* parent=nullptr);
 ~ImageCopyCapture() override;
 // Per-protocol facts: which globals exist, at which versions, and why a
 // capture kind is unavailable.
 QJsonObject status() const;
 bool outputsAvailable(QString* reason=nullptr) const;
 bool windowsAvailable(QString* reason=nullptr) const;
 QList<Toplevel> toplevels() const;
 // After a compositor round trip, so state from before the call has arrived.
 // Fails with capture_target unless exactly one window matches.
 void freezeActive(std::function<void(Toplevel,Error)>);
 void captureOutput(const QString& name,const ResourceLimits&,Reply);
 void captureToplevel(const QString& identifier,const ResourceLimits&,Reply);
 // Ends every request in flight with cancelled, including a frame still being turned upright.
 void cancel();
signals:
 void changed();
private:
 struct Impl;
 std::unique_ptr<Impl> d_;
};
// wl_shm formats a capture accepts, mapped to the QImage format that reads the
// same bytes. Invalid means the format is not used.
QImage::Format shmImageFormat(uint32_t shmFormat);
}
