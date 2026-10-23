#pragma once
// Framed JSON control channel over a pair of pipe handles (child side reads
// stdin / writes stdout; parent side holds the child's pipes). Sequence numbers
// are checked in both directions; reads are bounded by protocol::maximumFrameBytes.
#include "common/Protocol.h"
#include <functional>
#include <string>
#include <windows.h>

namespace putgd::frames {
class Channel {
public:
    Channel(HANDLE read, HANDLE write);
    ~Channel();
    Channel(const Channel&) = delete;
    // Blocking write of one frame; false once the peer is gone.
    bool send(const std::string& type, const json::Object& payload = {});
    // Blocking read of the next frame. Returns false on EOF, timeout (ms, -1 = none)
    // or protocol violation; `error` names the reason.
    bool receive(protocol::Frame& frame, int timeoutMs, std::string* error = nullptr);
    // True when at least one complete frame is buffered or bytes are readable now.
    bool readable();
    void close();
    unsigned long long sent() const { return sent_; }
private:
    bool fill(int timeoutMs, std::string* error);
    HANDLE read_, write_;
    std::string buffer_;
    unsigned long long sent_ = 0, received_ = 0;
    bool closed_ = false;
};
// The helper side: stdin/stdout of this process, with stdout redirected so stray
// prints cannot corrupt the channel (they go to stderr).
Channel& helperChannel();
}
