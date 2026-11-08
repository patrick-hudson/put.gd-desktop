#pragma once
#include <QString>

// Notification servers, by the name GetServerInformation returns.
namespace putgd::notifications {
// A failure notice is critical: it stays on screen until seen. GNOME Shell,
// Plasma and Cinnamon then keep it in their list until it is dismissed, and a
// GNOME screen lock hides a banner but destroys only a transient one, so a
// refusal History never records (no destination, another installation
// running) must not be transient there. Omarchy's quickshell server also
// restores its list after a restart and showed an old failure again after
// every reboot (#1298); there, and only there, a failure carries the spec's
// "transient" hint.
inline bool failuresTransient(const QString& server) { return server == QLatin1String("quickshell"); }
}
