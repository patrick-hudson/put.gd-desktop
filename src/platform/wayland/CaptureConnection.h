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
private:
 QByteArray path_;
 mutable int serverPid_=-1;
 mutable unsigned int serverUid_=0,serverGid_=0;
};
}
