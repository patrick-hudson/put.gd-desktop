#include "KdeScreencast.h"
#include "DebugLog.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QPointer>
#include <QSocketNotifier>
#include <QTimer>
#include <wayland-client.h>
#include "xdg-output-unstable-v1-client-protocol.h"
#include "zkde-screencast-unstable-v1-client-protocol.h"

namespace putgd::record {
struct KdeScreencast::Impl {
    struct Output { wl_output* native = nullptr; zxdg_output_v1* xdg = nullptr; uint32_t id = 0; QString name; QRect logical; };
    KdeScreencast& q;
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    zxdg_output_manager_v1* manager = nullptr;
    zkde_screencast_unstable_v1* screencast = nullptr;
    uint32_t screencastVersion = 0;
    zkde_screencast_stream_unstable_v1* stream = nullptr;
    QList<Output*> outputs;
    QSocketNotifier* notifier = nullptr;
    Done done;
    Stream pending;
    explicit Impl(KdeScreencast& owner) : q(owner) {}
    static void logical_position(void* data, zxdg_output_v1*, int32_t x, int32_t y) { auto* o = static_cast<Output*>(data); o->logical.moveTopLeft({x, y}); }
    static void logical_size(void* data, zxdg_output_v1*, int32_t w, int32_t h) { auto* o = static_cast<Output*>(data); o->logical.setSize({w, h}); }
    static void xdg_done(void*, zxdg_output_v1*) {}
    static void xdg_name(void* data, zxdg_output_v1*, const char* name) { static_cast<Output*>(data)->name = QString::fromUtf8(name); }
    static void xdg_description(void*, zxdg_output_v1*, const char*) {}
    static void wl_geometry(void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {}
    static void wl_mode(void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}
    static void wl_done(void*, wl_output*) {}
    static void wl_scale(void*, wl_output*, int32_t) {}
    static void wl_name(void* data, wl_output*, const char* name) { auto* o = static_cast<Output*>(data); if (o->name.isEmpty()) o->name = QString::fromUtf8(name); }
    static void wl_description(void*, wl_output*, const char*) {}
    void bindXdg(Output* output) {
        if (!manager || output->xdg) return;
        output->xdg = zxdg_output_manager_v1_get_xdg_output(manager, output->native);
        static const zxdg_output_v1_listener listener{logical_position, logical_size, xdg_done, xdg_name, xdg_description};
        zxdg_output_v1_add_listener(output->xdg, &listener, output);
    }
    static void global(void* data, wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
        auto* self = static_cast<Impl*>(data);
        const QString name = QString::fromUtf8(interface);
        if (name == "zxdg_output_manager_v1") {
            self->manager = static_cast<zxdg_output_manager_v1*>(wl_registry_bind(registry, id, &zxdg_output_manager_v1_interface, std::min(version, 3u)));
            for (auto* output : self->outputs) self->bindXdg(output);
        } else if (name == "wl_output") {
            auto* output = new Output;
            output->id = id;
            output->native = static_cast<wl_output*>(wl_registry_bind(registry, id, &wl_output_interface, std::min(version, 4u)));
            static const wl_output_listener listener{wl_geometry, wl_mode, wl_done, wl_scale, wl_name, wl_description};
            wl_output_add_listener(output->native, &listener, output);
            self->outputs.append(output);
            self->bindXdg(output);
        } else if (name == "zkde_screencast_unstable_v1") {
            self->screencastVersion = std::min(version, 5u);
            self->screencast = static_cast<zkde_screencast_unstable_v1*>(wl_registry_bind(registry, id, &zkde_screencast_unstable_v1_interface, self->screencastVersion));
        }
    }
    static void removed(void*, wl_registry*, uint32_t) {}
    static void created(void* data, zkde_screencast_stream_unstable_v1*, uint32_t node) {
        auto* self = static_cast<Impl*>(data);
        self->pending.node = node;
        debugLog(QStringLiteral("kde screencast: stream node %1 for %2").arg(node).arg(self->pending.output.isEmpty() ? QStringLiteral("region") : self->pending.output));
        if (auto done = std::exchange(self->done, {})) done({}, self->pending);
    }
    static void failed(void* data, zkde_screencast_stream_unstable_v1*, const char* error) {
        auto* self = static_cast<Impl*>(data);
        debugLog(QStringLiteral("kde screencast: failed: %1").arg(QString::fromUtf8(error)));
        if (auto done = std::exchange(self->done, {})) done({"recording_source_failed", "KWin refused the screen cast: " + QString::fromUtf8(error), {}}, {});
    }
    static void closed(void* data, zkde_screencast_stream_unstable_v1*) {
        auto* self = static_cast<Impl*>(data);
        debugLog("kde screencast: stream closed by the compositor");
        if (auto done = std::exchange(self->done, {})) done({"recording_source_lost", "KWin closed the screen cast", {}}, {});
    }
    bool connect() {
        if (display) return true;
        display = wl_display_connect(nullptr);
        if (!display) return false;
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener{global, removed};
        wl_registry_add_listener(registry, &listener, this);
        wl_display_roundtrip(display);  // globals
        wl_display_roundtrip(display);  // output names and logical geometry
        notifier = new QSocketNotifier(wl_display_get_fd(display), QSocketNotifier::Read, &q);
        QObject::connect(notifier, &QSocketNotifier::activated, &q, [this] { if (wl_display_prepare_read(display) == 0) wl_display_read_events(display); wl_display_dispatch_pending(display); wl_display_flush(display); });
        return true;
    }
    void listen(zkde_screencast_stream_unstable_v1* s) {
        static const zkde_screencast_stream_unstable_v1_listener listener{closed, created, failed};
        zkde_screencast_stream_unstable_v1_add_listener(s, &listener, this);
        wl_display_flush(display);
    }
    ~Impl() {
        if (stream) zkde_screencast_stream_unstable_v1_close(stream);
        for (auto* output : outputs) { if (output->xdg) zxdg_output_v1_destroy(output->xdg); if (output->native) wl_output_destroy(output->native); delete output; }
        if (screencast) zkde_screencast_unstable_v1_destroy(screencast);
        if (manager) zxdg_output_manager_v1_destroy(manager);
        if (registry) wl_registry_destroy(registry);
        if (display) { wl_display_flush(display); wl_display_disconnect(display); }
    }
};

KdeScreencast::KdeScreencast(QObject* parent) : QObject(parent), d_(std::make_unique<Impl>(*this)) {}
KdeScreencast::~KdeScreencast() = default;

bool KdeScreencast::available(QJsonObject& facts, QString* reason) {
    if (!d_->connect()) { if (reason) *reason = "wayland_unavailable"; return false; }
    QJsonArray outputs;
    for (const auto* output : d_->outputs) outputs.append(QJsonObject{{"name", output->name}, {"logical", QJsonArray{output->logical.x(), output->logical.y(), output->logical.width(), output->logical.height()}}});
    facts["outputs"] = outputs;
    if (!d_->screencast) { if (reason) *reason = "kde_screencast_unavailable"; return false; }
    facts["version"] = int(d_->screencastVersion);
    if (d_->screencastVersion < 3) { if (reason) *reason = "kde_screencast_too_old"; return false; }
    return true;
}

void KdeScreencast::streamOutput(const QString& name, Done done) {
    QJsonObject facts;
    QString reason;
    if (!available(facts, &reason)) { done({"recording_unsupported", "KWin screen casting is unavailable: " + reason, {}}, {}); return; }
    if (d_->done || d_->stream) { done({"recording_state", "A KWin screen cast is already open", {}}, {}); return; }
    Impl::Output* chosen = nullptr;
    for (auto* output : d_->outputs) if (name.isEmpty() || output->name == name) { chosen = output; break; }
    if (!chosen) { done({"recording_target", "No connected output has this name", {{"identity", name}}}, {}); return; }
    d_->pending = {0, chosen->logical, chosen->name};
    d_->done = std::move(done);
    d_->stream = zkde_screencast_unstable_v1_stream_output(d_->screencast, chosen->native, ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
    d_->listen(d_->stream);
}

void KdeScreencast::streamRegion(const QRect& logical, Done done) {
    QJsonObject facts;
    QString reason;
    if (!available(facts, &reason)) { done({"recording_unsupported", "KWin screen casting is unavailable: " + reason, {}}, {}); return; }
    if (d_->done || d_->stream) { done({"recording_state", "A KWin screen cast is already open", {}}, {}); return; }
    d_->pending = {0, logical, {}};
    d_->done = std::move(done);
    d_->stream = zkde_screencast_unstable_v1_stream_region(d_->screencast, logical.x(), logical.y(), uint32_t(logical.width()), uint32_t(logical.height()), wl_fixed_from_int(1), ZKDE_SCREENCAST_UNSTABLE_V1_POINTER_EMBEDDED);
    d_->listen(d_->stream);
}

void KdeScreencast::close() {
    // `close` is the protocol's destructor request: it ends the stream and frees
    // the proxy, so the proxy must not be destroyed a second time.
    if (d_->stream) { zkde_screencast_stream_unstable_v1_close(d_->stream); d_->stream = nullptr; if (d_->display) wl_display_flush(d_->display); debugLog("kde screencast: stream closed"); }
    if (auto done = std::exchange(d_->done, {})) done({"cancelled", "Screen cast canceled", {}}, {});
}
}
