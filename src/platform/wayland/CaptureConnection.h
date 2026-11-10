#pragma once
#include <QByteArray>
struct wl_display;
namespace putgd {
// Resolve once for pixel capture and its visibility witnesses. Never duplicate
// a Wayland protocol stream: every caller needs an independent client.
class CaptureConnection {
public:
 CaptureConnection();
 wl_display* connect() const;
 // Only the pixel connection may reset a path pin, after its own loss.
 void disconnected();
private:
 QByteArray path_;bool inherited_=false;
 mutable int serverPid_=-1;
 mutable unsigned int serverUid_=0,serverGid_=0;
};
}
