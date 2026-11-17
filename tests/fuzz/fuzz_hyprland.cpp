// Hyprland's IPC (platform/hyprland/HyprlandIpc.cpp): the replies to one-request
// connections, and the line stream of events. Both come from a process put.gd
// does not control. luaString builds the literal every eval command carries.
#include "FuzzPeer.h"
#include "FuzzSupport.h"
#include "platform/hyprland/HyprlandIpc.h"
#include <QDir>
#include <QElapsedTimer>
#include <QTemporaryDir>
using namespace putgd;

namespace {
QString runtimeDirectory;
// The literal must stay one Lua string whatever it was made from.
bool singleLiteral(const QString& literal) {
    const auto bytes = literal.toUtf8();
    if (bytes.size() < 2 || bytes.front() != '"' || bytes.back() != '"') return false;
    for (qsizetype i = 1; i < bytes.size() - 1; ++i) {
        const auto c = uint8_t(bytes[i]);
        if (c < 0x20 || c == 0x7f || c >= 0x80) return false;
        if (c == '\\') { ++i; if (i >= bytes.size() - 1) return false; continue; }
        if (c == '"') return false;
    }
    return true;
}
}

extern "C" int LLVMFuzzerInitialize(int* argc, char*** argv) {
    fuzz::application(argc, argv);
    static QTemporaryDir directory("/tmp/pgfh-XXXXXX");
    runtimeDirectory = directory.path();
    QDir().mkpath(runtimeDirectory + "/hypr/sig");
    qputenv("XDG_RUNTIME_DIR", runtimeDirectory.toUtf8());
    qputenv("HYPRLAND_INSTANCE_SIGNATURE", "sig");
    return 0;
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    fuzz::Input in(data, size);
    const auto mode = in.byte();
    if (mode % 4 == 0) {
        const auto text = QString::fromUtf8(in.rest());
        FUZZ_REQUIRE(singleLiteral(HyprlandIpc::luaString(text)));
        HyprlandIpc::okReply(text.toUtf8());
        return 0;
    }
    fuzz::Script script;
    script.piece = 1 + in.number(500);
    script.halfClose = mode & 16;
    script.reply = in.rest();
    fuzz::Peer peer;
    const bool events = mode % 4 == 3;
    if (!peer.listen(runtimeDirectory + "/hypr/sig/" + (events ? ".socket2.sock" : ".socket.sock"))) return 0;
    if (mode % 4 == 2 && (mode & 32)) {
        // The quit path's blocking request.
        script.readFirst = true;
        peer.serve(script);
        QByteArray reply;
        HyprlandIpc::requestBlocking("j/clients", &reply, 400);
        FUZZ_REQUIRE(reply.size() <= 8 * 1024 * 1024 + 1);
        peer.join();
        return 0;
    }
    script.readFirst = !events;
    peer.serve(script);
    HyprlandIpc ipc;
    bool answered = false;
    int lines = 0;
    QObject::connect(&ipc, &HyprlandIpc::event, [&](const QString& name, const QString&) { FUZZ_REQUIRE(!name.isEmpty()); ++lines; });
    if (events) ipc.listen();
    else if (mode % 4 == 1) ipc.request("j/clients", [&](QByteArray, Error) { answered = true; });
    else ipc.json("j/binds", [&](QJsonDocument, Error) { answered = true; });
    QElapsedTimer clock;
    clock.start();
    // The peer writes and closes at once; the loop runs until the object has read it all.
    int settle = 0;
    while (!answered && clock.elapsed() < 700 && settle < (events ? 6 : 1000)) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
        if (peer.finished()) ++settle;
    }
    peer.join();
    (void)lines;
    return 0;
}
