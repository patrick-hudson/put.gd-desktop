#pragma once
// environment.json for diagnostics packages and the "Resident started" record:
// the Windows equivalents of the Linux fields (docs/diagnostics.md, "Contents").
// os (product, edition, display version, version and build with its update
// revision, installation type), architecture (native and process), session
// (console or remote, session ID, window station), displays (count, and each
// monitor's effective DPI and scale factor, its size and whether it is the
// primary; no pixels), system DPI, locale and UI language, total RAM, free bytes
// under the data folder, the notification area, and the resident kind.
#include "common/Json.h"
#include <string>

namespace putgd::environment {
json::Object collect(const std::string& dataRoot, bool trayAvailable);
// The short form for the startup record: os, architecture, session, displays.
json::Object summary(const json::Object& collected);
}
