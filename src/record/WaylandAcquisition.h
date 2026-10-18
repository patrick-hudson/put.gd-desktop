#pragma once
#include <QProcessEnvironment>
#include <QString>

namespace putgd::record {
// The session PipeWire daemon's socket as `pipewiresrc` would find it (the
// `PIPEWIRE_REMOTE` name under `PIPEWIRE_RUNTIME_DIR`, `XDG_RUNTIME_DIR` or
// `USERPROFILE`), or an empty string. The compositor-native Wayland paths hand
// over a node id and need it; the portal passes its own connection instead.
QString pipewireSocket(const QProcessEnvironment& env);
}
