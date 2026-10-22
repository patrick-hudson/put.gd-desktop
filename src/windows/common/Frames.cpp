#include "common/Frames.h"
#include "common/BuildInfo.h"
#include "common/Text.h"
#include <cstdio>
#include <io.h>
#include <fcntl.h>

namespace putgd::protocol {
std::string encodeFrame(const std::string& type, unsigned long long sequence, const json::Object& payload) {
    return json::serialize(json::Object{{"version", version}, {"seq", std::to_string(sequence)}, {"type", type}, {"payload", payload}}) + "\n";
}
bool decodeFrame(const std::string& line, Frame& out) {
    auto object = json::parseObject(line);
    if (!object || object->size() != 4 || !json::integer(object->value("version"), version, version) || !object->value("type").isString() ||
        !object->value("seq").isString() || !object->value("payload").isObject()) return false;
    const auto& seq = object->value("seq").stringRef();
    if (seq.empty() || seq.size() > 18) return false;
    for (const char c : seq) if (c < '0' || c > '9') return false;
    out.type = object->value("type").stringRef();
    if (out.type.empty() || out.type.size() > 64) return false;
    out.sequence = std::strtoull(seq.c_str(), nullptr, 10);
    out.payload = object->value("payload").toObject();
    return true;
}
json::Object buildIdentity() { return {{"version", build::version}, {"commit", build::commit}, {"platform", build::platform}}; }
bool buildMatches(const json::Object& identity) { return identity.value("version") == build::version && identity.value("commit") == build::commit; }
}

namespace putgd::frames {
Channel::Channel(HANDLE read, HANDLE write) : read_(read), write_(write) {}
Channel::~Channel() { close(); }
void Channel::close() {
    if (closed_) return;
    closed_ = true;
    if (read_ && read_ != INVALID_HANDLE_VALUE) CloseHandle(read_);
    if (write_ && write_ != INVALID_HANDLE_VALUE && write_ != read_) CloseHandle(write_);
    read_ = write_ = INVALID_HANDLE_VALUE;
}
bool Channel::send(const std::string& type, const json::Object& payload) {
    if (closed_) return false;
    const auto line = protocol::encodeFrame(type, ++sent_, payload);
    if (line.size() > protocol::maximumFrameBytes) return false;
    size_t offset = 0;
    while (offset < line.size()) {
        DWORD written = 0;
        if (!WriteFile(write_, line.data() + offset, DWORD(line.size() - offset), &written, nullptr) || written == 0) return false;
        offset += written;
    }
    return true;
}
bool Channel::readable() {
    if (buffer_.find('\n') != std::string::npos) return true;
    DWORD available = 0;
    return PeekNamedPipe(read_, nullptr, 0, nullptr, &available, nullptr) && available > 0;
}
bool Channel::fill(int timeoutMs, std::string* error) {
    // Pipes have no timed read; poll PeekNamedPipe so a dead peer cannot block forever.
    const ULONGLONG start = GetTickCount64();
    while (true) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_, nullptr, 0, nullptr, &available, nullptr)) { if (error) *error = "pipe_closed"; return false; }
        if (available > 0) break;
        if (timeoutMs >= 0 && GetTickCount64() - start >= ULONGLONG(timeoutMs)) { if (error) *error = "timeout"; return false; }
        Sleep(available ? 0 : 5);
    }
    char chunk[65536];
    DWORD read = 0;
    if (!ReadFile(read_, chunk, sizeof chunk, &read, nullptr) || read == 0) { if (error) *error = "pipe_closed"; return false; }
    buffer_.append(chunk, read);
    if (buffer_.size() > protocol::maximumFrameBytes + 1) { if (error) *error = "frame_too_large"; return false; }
    return true;
}
bool Channel::receive(protocol::Frame& frame, int timeoutMs, std::string* error) {
    if (closed_) { if (error) *error = "pipe_closed"; return false; }
    const ULONGLONG start = GetTickCount64();
    while (true) {
        const auto newline = buffer_.find('\n');
        if (newline != std::string::npos) {
            const auto line = buffer_.substr(0, newline);
            buffer_.erase(0, newline + 1);
            if (!protocol::decodeFrame(line, frame)) { if (error) *error = "invalid_frame"; return false; }
            if (frame.sequence != received_ + 1) { if (error) *error = "sequence_gap"; return false; }
            ++received_;
            return true;
        }
        int remaining = -1;
        if (timeoutMs >= 0) {
            const auto elapsed = GetTickCount64() - start;
            if (elapsed >= ULONGLONG(timeoutMs)) { if (error) *error = "timeout"; return false; }
            remaining = int(ULONGLONG(timeoutMs) - elapsed);
        }
        if (!fill(remaining, error)) return false;
    }
}
Channel& helperChannel() {
    static Channel* channel = [] {
        // Duplicate the inherited ends and point stdout at stderr so library
        // chatter cannot enter the protocol stream.
        HANDLE in = GetStdHandle(STD_INPUT_HANDLE), out = GetStdHandle(STD_OUTPUT_HANDLE);
        HANDLE inCopy = INVALID_HANDLE_VALUE, outCopy = INVALID_HANDLE_VALUE;
        DuplicateHandle(GetCurrentProcess(), in, GetCurrentProcess(), &inCopy, 0, FALSE, DUPLICATE_SAME_ACCESS);
        DuplicateHandle(GetCurrentProcess(), out, GetCurrentProcess(), &outCopy, 0, FALSE, DUPLICATE_SAME_ACCESS);
        SetStdHandle(STD_OUTPUT_HANDLE, GetStdHandle(STD_ERROR_HANDLE));
        _dup2(_fileno(stderr), _fileno(stdout));
        return new Channel(inCopy, outCopy);
    }();
    return *channel;
}
}
