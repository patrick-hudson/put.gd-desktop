#pragma once
// The resident owns the Windows clipboard for results and reads offers for
// imports. Revisions are the system clipboard sequence number, so a user copy
// after an upload started is visible without reading any payload.
#include "common/Error.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace putgd {
struct ClipboardOffer {
    std::string kind;              // files, image, text, none
    std::vector<std::string> files;
    std::string imageFormat;       // "png" or "dib"
    std::string bytes;             // PNG bytes, DIB bytes or UTF-8 text
    unsigned long long revision = 0;
};
class Clipboard {
public:
    explicit Clipboard(HWND owner);
    ~Clipboard();
    unsigned long long revision() const; // GetClipboardSequenceNumber
    // Which offers exist right now, without reading payloads.
    std::vector<std::string> formats() const;
    // Reads the current offer by priority: files, then an image, then text. Bounded by limits.
    ClipboardOffer read(long long maximumTextBytes, long long maximumImageBytes, int maximumFiles, Error& error);
    // Writes text; `revision` receives the sequence number acknowledged after our write.
    Error writeText(const std::string& utf8, unsigned long long& revision);
    // Writes PNG bytes plus a DIBV5 copy (both produced by the image job).
    Error writeImage(const std::string& pngBytes, const std::string& dibBytes, unsigned long long& revision);
    Error writeFiles(const std::vector<std::string>& paths, unsigned long long& revision);
    std::optional<unsigned long long> lastCopyRevision() const { return lastCopy_; }
    // WM_CLIPBOARDUPDATE arrived: a change we did not make is reported to the callback.
    void onUserChange(std::function<void(unsigned long long)> callback) { userChange_ = std::move(callback); }
    void handleUpdate();
private:
    bool open();
    HWND owner_;
    std::optional<unsigned long long> lastCopy_;
    std::function<void(unsigned long long)> userChange_;
    unsigned pngFormat_ = 0;
};
}
