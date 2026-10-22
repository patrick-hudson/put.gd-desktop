#pragma once
// Where the recorder's pictures come from. A source keeps the newest picture of
// its target in a top-down BGRX buffer of the canvas size and says when that
// picture reached the screen (QPC in 100-ns units), so the recorder places it on
// the frame grid by that time rather than by when it happened to be read:
// - Desktop Duplication (DXGI) for monitors, the desktop and regions that lie on
//   one output: one picture per compositor present, timed by the present;
// - Windows.Graphics.Capture for windows: the window's own content, occluded
//   parts too, timed by the frame's system-relative time;
// - GDI BitBlt for areas spanning outputs and as the fallback: read on request
//   and timed at the instant it was asked for.
// D3D11, DXGI and WinRT are loaded at run time, so the resident maps none of them.
#include "capture/Grab.h"
#include "common/Error.h"
#include <memory>
#include <string>
#include <windows.h>

namespace putgd::record {
long long now100ns(); // QPC in 100-ns units, the clock of present and frame times
long long hundredNs(long long qpcTicks);

class Source {
public:
    virtual ~Source() = default;
    // Waits until `deadline` (now100ns() units) at most, or until `wake` is set, for
    // a newer picture; `got` with its `present` time when bits() changed.
    virtual Error next(long long deadline, HANDLE wake, bool& got, long long& present) = 0;
    virtual const uint8_t* bits() const = 0;
    virtual size_t stride() const = 0;
    virtual std::string name() const = 0; // gdi | dxgi | wgc
    // The window target closed (window sources only).
    virtual bool ended() const { return false; }
};

// Desktop Duplication when `area` lies on one output and duplication works, else
// GDI; `note` says why GDI was chosen.
Error openScreenSource(const grab::Rect& area, std::unique_ptr<Source>& out, std::string& note);
// Windows.Graphics.Capture of `window` into a width x height canvas (top-left
// aligned; a window that grows is cropped, one that shrinks leaves black).
Error openWindowSource(HWND window, int width, int height, std::unique_ptr<Source>& out);
// Whether window capture (Windows.Graphics.Capture) exists here; a reason if not.
bool windowCaptureAvailable(std::string& reason);
// Whether Desktop Duplication works on the output holding `area`; a reason if not.
bool duplicationAvailable(const grab::Rect& area, std::string& reason);
// The window's size as window capture delivers it (physical pixels).
Error windowCaptureSize(HWND, int& width, int& height);
}
