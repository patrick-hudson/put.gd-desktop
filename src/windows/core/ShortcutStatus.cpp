#include "core/ShortcutStatus.h"

namespace putgd::shortcuts {
Presentation present(const json::Object& status, const std::string& action, const std::string& shown, bool recording) {
    const auto& entry = status.value("bindings")[action];
    const auto error = entry["error"].isObject() ? entry["error"]["code"].toString() : entry["error"].toString();
    const auto configured = entry["configured"].toString();
    const std::string dot = " \xC2\xB7 ";
    if (recording || entry["paused"].toBool()) return {"Paused" + dot + "Recording a shortcut.", "muted", true};
    if (status.value("available").isBool() && !status.value("available").toBool())
        return {"Unavailable" + dot + "Global shortcuts are not available in this session.", "warning", true};
    if (!entry.isObject()) return {"Not reported" + dot + "put.gd has not reported this shortcut yet.", "muted", true};
    if (configured.empty() && error.empty()) return {"Disabled" + dot + "No global shortcut is assigned.", "muted", true};
    if (error == "shortcut_conflict") return {"Conflict" + dot + "Another app already uses " + configured + ". Choose another shortcut.", "error", true};
    if (error == "unsupported_shortcut" || error == "shortcut_unsupported" || error == "shortcut_invalid")
        return {"Unsupported shortcut" + dot + "Windows cannot register this key combination.", "warning", true};
    if (!error.empty()) return {"Not bound" + dot + "Windows refused this shortcut. Choose another combination.", "warning", true};
    if (entry["bound"].toBool()) {
        // Bound is the resident's observation; a row that still shows the saved value hides it.
        const bool matches = configured == shown;
        return {"Active" + dot + configured, "active", !matches};
    }
    return {"Not bound" + dot + "Choose a shortcut, or restart put.gd if this persists.", "muted", true};
}
}
