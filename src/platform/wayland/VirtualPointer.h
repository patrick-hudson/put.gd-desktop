#pragma once
#include <QString>
#include <cstdint>
#include <memory>

namespace putgd {
// A wlroots virtual pointer (zwlr_virtual_pointer_v1) on its own Wayland
// connection, for the scroll capture's wheel steps and pointer placement on
// labwc (Budgie 10.10) and Hyprland (#1376). It sends absolute motion and
// wheel axis events and nothing else: no buttons, no keys. A wheel notch is
// what a real mouse sends and what wayvnc and lan-mouse send: the wheel as the
// axis source, a value of 15 with a discrete step of 1, then a frame.
// (Finger-source scrolling with an axis stop, as wlrctl sends, reads as a
// touchpad and may start kinetic scrolling.) Every call waits for the
// compositor to read it. Calls may come from any one thread at a time.
class VirtualPointer {
public:
    struct Facts { bool available = false; int version = 0; int outputs = 0; QString reason; };
    // A connection and two round trips; creates no pointer and sends no event.
    static Facts probe();
    VirtualPointer();
    ~VirtualPointer();
    VirtualPointer(const VirtualPointer&) = delete;
    VirtualPointer& operator=(const VirtualPointer&) = delete;
    // Creates the pointer, mapped to the output with this wl_output name when
    // the manager can (version 2). With several outputs and an older manager
    // there is no way to say where a position is, and this fails.
    bool open(const QString& outputName, QString* reason = nullptr);
    bool isOpen() const;
    // A position within the output: x of extentWidth, y of extentHeight.
    bool moveTo(uint32_t x, uint32_t y, uint32_t extentWidth, uint32_t extentHeight);
    // Relative motion in logical pixels, as a mouse moves (no acceleration).
    bool moveBy(double dx, double dy);
    // Positive scrolls the content down, one frame per notch.
    bool wheel(int notches);
    // What one notch carries.
    static constexpr int notchValue = 15;
private:
    struct Impl;
    std::unique_ptr<Impl> d_;
};
}
