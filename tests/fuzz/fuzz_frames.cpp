// The newline-framed JSON protocols between processes: UiControl (resident and the
// putgd-ui helpers, and the image, text and S3 workers), CaptureControl (the X11
// capture helper) and the session socket's reply decoder. The bytes arrive split
// at arbitrary points, and may be repeated to push a frame past its limit.
#include "FuzzSupport.h"
#include "client/SessionProtocol.h"
#include "ipc/CaptureControl.h"
#include "ipc/StdioDevice.h"
#include "ipc/UiControl.h"
#include <QIODevice>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <fcntl.h>
#include <unistd.h>
using namespace putgd;

namespace {
// What the peer's pipe looks like to the protocol: bytes arrive when the fuzzer says,
// and writes are accepted in the pieces it says (a full pipe returns 0).
class Pipe final : public QIODevice {
public:
    Pipe() { open(QIODevice::ReadWrite | QIODevice::Unbuffered); }
    bool isSequential() const override { return true; }
    qint64 bytesAvailable() const override { return incoming_.size() + QIODevice::bytesAvailable(); }
    void feed(const QByteArray& bytes) { incoming_ += bytes; emit readyRead(); }
    void accept(int budget) { budget_ = budget; emit bytesWritten(0); }
    qint64 written() const { return written_; }
protected:
    qint64 readData(char* out, qint64 count) override {
        const auto n = qMin<qint64>(count, incoming_.size());
        memcpy(out, incoming_.constData(), size_t(n));
        incoming_.remove(0, n);
        return n;
    }
    qint64 writeData(const char*, qint64 count) override {
        const auto n = qMin<qint64>(count, budget_);
        budget_ -= n;
        written_ += n;
        return n;
    }
private:
    QByteArray incoming_;
    qint64 budget_ = 0, written_ = 0;
};

const char* const uiTypes[] = {"bootstrap", "ready", "checkpoint", "command", "event", "lanes", "copyText", "shutdown"};
const char* const captureTypes[] = {"bootstrap", "ready", "preview", "begin", "result", "error", "pointer", "takeover"};

QByteArray frame(const char* type, quint64 sequence, const QByteArray& payload) {
    return QByteArray("{\"version\":1,\"type\":\"") + type + "\",\"sequence\":\"" + QByteArray::number(sequence) + "\",\"payload\":" + payload + "}\n";
}

template <class Control, size_t N>
void drive(fuzz::Input& in, const char* const (&types)[N]) {
    Pipe pipe;
    Control control(pipe);
    int frames = 0, failures = 0;
    bool failed = false;
    QObject::connect(&control, &Control::failed, [&](Error e) { ++failures; failed = true; FUZZ_REQUIRE(!e.code.isEmpty()); });
    if constexpr (std::is_same_v<Control, UiControl>)
        QObject::connect(&control, &UiControl::frame, [&](const QString&, const QString&, const QJsonObject&) { FUZZ_REQUIRE(!failed); ++frames; });
    else
        QObject::connect(&control, &CaptureControl::frame, [&](const QString&, const QJsonObject&) { FUZZ_REQUIRE(!failed); ++frames; });
    const int pick = in.byte() % 64, style = pick < 24 ? 0 : pick < 48 ? 1 : pick < 63 ? 2 : 3;
    const auto bytes = in.part();
    const int repeat = style == 2 ? 1 + int(in.number(300)) : 1;
    // A well-formed frame stream around the fuzzed payload, for depth beyond the envelope.
    quint64 sequence = 1;
    const auto piece = [&]() -> QByteArray {
        if (style == 1) return frame(types[(in.byte()) % N], sequence++, bytes);
        // One frame far longer than any limit: no newline, or a late one.
        if (style == 3) return QByteArray("{\"version\":1,\"payload\":\"") + QByteArray(int(in.number(0xffff)) * 200, 'A') + "\"}\n";
        return bytes;
    };
    const int cut = in.byte() < 8 ? 1 + in.byte() : 64 + in.byte() * 16;
    for (int r = 0; r < repeat && !failed; ++r) {
        const auto whole = piece();
        // At most ~20000 reads a run, however the stream is cut: a million one-byte reads is a slow target, not a finding.
        // The one-oversized-frame style may take many more: it is where a rescan of the unfinished frame shows.
        const qsizetype step = qMax<qsizetype>(cut, whole.size() / (style == 3 ? 300000 : 20000) + 1);
        for (qsizetype at = 0; at < whole.size() && !failed; at += step) pipe.feed(whole.mid(at, step));
    }
    // Replies the control sends back, written in fuzzed pieces.
    const QJsonObject payload{{"k", QString::fromUtf8(in.rest())}};
    for (int i = 0; i < 4; ++i) {
        if constexpr (std::is_same_v<Control, UiControl>) control.send(types[i % N], payload);
        else control.send(types[i % N], payload);
        pipe.accept(int(in.number(70000)));
    }
    FUZZ_REQUIRE(failures <= 1);
    (void)frames;
    control.stop();
}

// The same UiControl over the real pipe adapter of a helper (StdioDevice): bytes arrive
// through the kernel in pieces, and a reply to a peer that is not reading hits a full pipe.
void overPipes(fuzz::Input& in) {
    int toChild[2], fromChild[2];
    if (pipe2(toChild, O_CLOEXEC) != 0) return;
    if (pipe2(fromChild, O_CLOEXEC) != 0) { ::close(toChild[0]); ::close(toChild[1]); return; }
    {
        StdioDevice device(toChild[0], fromChild[1]);
        UiControl control(device);
        int failures = 0;
        QObject::connect(&control, &UiControl::failed, [&](Error) { ++failures; });
        const auto bytes = in.part();
        const auto big = in.number(150000);
        const int piece = 1 + int(in.number(70000));
        fcntl(toChild[1], F_SETFL, O_NONBLOCK);
        for (qsizetype at = 0; at < bytes.size(); at += piece) {
            const auto count = qMin<qsizetype>(piece, bytes.size() - at);
            (void)!::write(toChild[1], bytes.constData() + at, size_t(count));
            QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        }
        // Replies into a pipe nobody empties: the adapter must hold them, not lose or spin.
        control.send("event", QJsonObject{{"pad", QString(int(big), 'x')}});
        control.send("event", QJsonObject{{"n", 2}});
        ::close(toChild[1]);
        toChild[1] = -1;
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < 2) QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        FUZZ_REQUIRE(failures <= 1);
        control.stop();
    }
    ::close(toChild[0]);
    if (toChild[1] >= 0) ::close(toChild[1]);
    ::close(fromChild[0]);
    ::close(fromChild[1]);
}
}

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    fuzz::application(argc, argv);
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    switch (in.byte() % 4) {
    case 0: drive<UiControl>(in, uiTypes); break;
    case 1: drive<CaptureControl>(in, captureTypes); break;
    case 3: overPipes(in); break;
    default: {
        const auto wire = QString::fromUtf8(in.part());
        const auto reply = in.rest();
        Error error;
        QJsonObject result;
        SessionProtocol::decodeReply(reply, wire, error, result);
        // A reply built for this id decodes (the genuine path keeps working).
        const auto good = QJsonDocument(QJsonObject{{"schemaVersion", 1}, {"requestId", wire}, {"ok", true}, {"result", QJsonObject{{"x", QString::fromUtf8(reply)}}},
                                                    {"error", QJsonObject{{"code", ""}, {"message", ""}, {"details", QJsonObject{}}}}}).toJson(QJsonDocument::Compact) + '\n';
        FUZZ_REQUIRE(SessionProtocol::decodeReply(good, wire, error, result));
        SessionProtocol::decodeReply(good, wire + "x", error, result);
        SessionProtocol::encodeRequest(wire, wire, QString::fromUtf8(reply), QJsonObject{{"a", wire}});
        SessionProtocol::deadlineMs(wire);
    }
    }
    return 0;
}
