#include "VirtualPointer.h"
#include <QList>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <wayland-client.h>
#include "wlr-virtual-pointer-unstable-v1-client-protocol.h"

namespace putgd {
struct VirtualPointer::Impl {
    struct Output { wl_output* native = nullptr; QString name; };
    wl_display* display = nullptr;
    wl_registry* registry = nullptr;
    wl_seat* seat = nullptr;
    zwlr_virtual_pointer_manager_v1* manager = nullptr;
    zwlr_virtual_pointer_v1* pointer = nullptr;
    uint32_t version = 0;
    QList<Output> outputs;
    std::mutex mutex;
    static void global(void* data, wl_registry* registry, uint32_t id, const char* interface, uint32_t version) {
        auto* self = static_cast<Impl*>(data);
        if (std::strcmp(interface, zwlr_virtual_pointer_manager_v1_interface.name) == 0 && !self->manager) {
            self->version = std::min(version, 2u);
            self->manager = static_cast<zwlr_virtual_pointer_manager_v1*>(wl_registry_bind(registry, id, &zwlr_virtual_pointer_manager_v1_interface, self->version));
        } else if (std::strcmp(interface, wl_seat_interface.name) == 0 && !self->seat) {
            self->seat = static_cast<wl_seat*>(wl_registry_bind(registry, id, &wl_seat_interface, 1));
        } else if (std::strcmp(interface, wl_output_interface.name) == 0 && self->outputs.size() < 16) {
            // Version 4 names the output; an older one can still be counted.
            Output output;
            output.native = static_cast<wl_output*>(wl_registry_bind(registry, id, &wl_output_interface, std::min(version, 4u)));
            self->outputs.append(output);
            static const wl_output_listener listener{
                [](void*, wl_output*, int32_t, int32_t, int32_t, int32_t, int32_t, const char*, const char*, int32_t) {},
                [](void*, wl_output*, uint32_t, int32_t, int32_t, int32_t) {}, [](void*, wl_output*) {}, [](void*, wl_output*, int32_t) {},
                [](void* data, wl_output* native, const char* name) { for (auto& o : static_cast<Impl*>(data)->outputs) if (o.native == native) o.name = QString::fromUtf8(name); },
                [](void*, wl_output*, const char*) {}};
            wl_output_add_listener(output.native, &listener, self);
        }
    }
    static void removed(void*, wl_registry*, uint32_t) {}
    QString connect() {
        if (display) return {};
        display = wl_display_connect(nullptr);
        if (!display) return QStringLiteral("Cannot connect to the Wayland display");
        registry = wl_display_get_registry(display);
        static const wl_registry_listener listener{global, removed};
        wl_registry_add_listener(registry, &listener, this);
        // Globals, then the output names.
        if (wl_display_roundtrip(display) < 0 || wl_display_roundtrip(display) < 0) return QStringLiteral("The Wayland connection failed");
        if (!manager) return QStringLiteral("The compositor does not offer zwlr_virtual_pointer_manager_v1");
        return {};
    }
    bool sync() { return wl_display_roundtrip(display) >= 0 && wl_display_get_error(display) == 0; }
    static uint32_t stamp() {
        timespec now{};
        clock_gettime(CLOCK_MONOTONIC, &now);
        return uint32_t(uint64_t(now.tv_sec) * 1000 + uint64_t(now.tv_nsec) / 1000000);
    }
    ~Impl() {
        if (pointer) zwlr_virtual_pointer_v1_destroy(pointer);
        if (manager) zwlr_virtual_pointer_manager_v1_destroy(manager);
        for (auto& output : outputs) wl_output_destroy(output.native);
        if (seat) wl_seat_destroy(seat);
        if (registry) wl_registry_destroy(registry);
        if (display) { wl_display_flush(display); wl_display_disconnect(display); }
    }
};

VirtualPointer::VirtualPointer() : d_(std::make_unique<Impl>()) {}
VirtualPointer::~VirtualPointer() = default;

VirtualPointer::Facts VirtualPointer::probe() {
    Impl impl;
    Facts facts;
    facts.reason = impl.connect();
    facts.available = facts.reason.isEmpty();
    facts.version = int(impl.version);
    facts.outputs = int(impl.outputs.size());
    return facts;
}

bool VirtualPointer::isOpen() const { return d_->pointer != nullptr; }

bool VirtualPointer::open(const QString& outputName, QString* reason) {
    std::lock_guard lock(d_->mutex);
    if (d_->pointer) return true;
    QString why = d_->connect();
    wl_output* output = nullptr;
    for (const auto& o : std::as_const(d_->outputs)) if (!outputName.isEmpty() && o.name == outputName) output = o.native;
    if (why.isEmpty() && !output && d_->outputs.size() > 1) why = QStringLiteral("The compositor has several outputs and none is named %1").arg(outputName);
    if (why.isEmpty() && d_->version < 2 && d_->outputs.size() > 1) why = QStringLiteral("zwlr_virtual_pointer_manager_v1 version 1 cannot map a pointer to one of several outputs");
    if (!why.isEmpty()) { if (reason) *reason = why; return false; }
    d_->pointer = d_->version >= 2 && output
        ? zwlr_virtual_pointer_manager_v1_create_virtual_pointer_with_output(d_->manager, d_->seat, output)
        : zwlr_virtual_pointer_manager_v1_create_virtual_pointer(d_->manager, d_->seat);
    if (!d_->pointer || !d_->sync()) { if (reason) *reason = QStringLiteral("The compositor refused a virtual pointer"); return false; }
    return true;
}

bool VirtualPointer::moveTo(uint32_t x, uint32_t y, uint32_t extentWidth, uint32_t extentHeight) {
    std::lock_guard lock(d_->mutex);
    if (!d_->pointer || !extentWidth || !extentHeight) return false;
    zwlr_virtual_pointer_v1_motion_absolute(d_->pointer, Impl::stamp(), std::min(x, extentWidth), std::min(y, extentHeight), extentWidth, extentHeight);
    zwlr_virtual_pointer_v1_frame(d_->pointer);
    return d_->sync();
}

bool VirtualPointer::wheel(int notches) {
    std::lock_guard lock(d_->mutex);
    if (!d_->pointer || notches == 0) return false;
    const int direction = notches > 0 ? 1 : -1;
    for (int i = 0; i < std::abs(notches); ++i) {
        zwlr_virtual_pointer_v1_axis_source(d_->pointer, WL_POINTER_AXIS_SOURCE_WHEEL);
        zwlr_virtual_pointer_v1_axis_discrete(d_->pointer, Impl::stamp(), WL_POINTER_AXIS_VERTICAL_SCROLL, wl_fixed_from_int(notchValue * direction), direction);
        zwlr_virtual_pointer_v1_frame(d_->pointer);
    }
    return d_->sync();
}
}
