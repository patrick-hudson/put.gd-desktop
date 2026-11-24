#pragma once
#include <QDeadlineTimer>

struct wl_display;
namespace putgd {
// A short-lived Wayland connection whose round trips share one deadline, for
// work done on a worker thread: a compositor that stops answering fails the
// round trip instead of blocking the thread forever.
class WaylandRoundTrips {
public:
 explicit WaylandRoundTrips(int timeoutMs);
 ~WaylandRoundTrips();
 WaylandRoundTrips(const WaylandRoundTrips&)=delete;WaylandRoundTrips& operator=(const WaylandRoundTrips&)=delete;
 wl_display* display() const {return display_;}
 // wl_display_sync plus dispatch until it is done, the deadline passes or the
 // connection fails.
 bool roundTrip();
private:
 wl_display* display_=nullptr;QDeadlineTimer deadline_;
};
}
