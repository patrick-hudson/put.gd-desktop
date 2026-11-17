#pragma once
// Shared pieces of the libFuzzer targets in this directory (see README.md here).
#include <QByteArray>
#include <QCoreApplication>
#include <QtGlobal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace fuzz {
// Reads typed values out of the fuzzer's bytes; reads past the end yield zeros, so
// every input is a valid input. (clang's FuzzedDataProvider.h is not on every distro.)
class Input {
public:
    Input(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    size_t remaining() const { return size_ - at_; }
    uint8_t byte() { return at_ < size_ ? data_[at_++] : 0; }
    uint32_t number(uint32_t maximum) { return maximum ? uint32_t(((uint32_t(byte()) << 8) | byte()) % (uint64_t(maximum) + 1)) : 0; }
    // A part whose length is the next two bytes; the last part takes what is left.
    QByteArray part() {
        const size_t want = (size_t(byte()) << 8) | byte();
        return take(want < remaining() ? want : remaining());
    }
    QByteArray rest() { return take(remaining()); }
private:
    QByteArray take(size_t count) {
        QByteArray out(reinterpret_cast<const char*>(data_ + at_), qsizetype(count));
        at_ += count;
        return out;
    }
    const uint8_t* data_;
    size_t size_, at_ = 0;
};

// A Qt message handler that says nothing, so a million qWarning()s do not slow a run.
inline void quiet(QtMsgType, const QMessageLogContext&, const QString&) {}

// Targets that need an application object (QTimer, QObject events) call this from
// LLVMFuzzerInitialize.
inline void application(int* argc, char*** argv) {
    qInstallMessageHandler(quiet);
    static QCoreApplication* app = new QCoreApplication(*argc, *argv);
    (void)app;
}

[[noreturn]] inline void bug(const char* what) {
    std::fprintf(stderr, "fuzz property violated: %s\n", what);
    std::abort();
}
}
#define FUZZ_REQUIRE(condition) do { if (!(condition)) ::fuzz::bug(#condition); } while (0)
