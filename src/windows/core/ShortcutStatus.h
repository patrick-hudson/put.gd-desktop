#pragma once
// The effective-binding line under each Settings → Shortcuts row, worded as the
// Linux ShortcutEditor: what the resident actually registered for the action,
// from capabilities.source.shortcuts ({available, bindings: {action: {configured,
// bound, error, paused}}}), not what the draft asks for.
#include "common/Json.h"
#include <string>

namespace putgd::shortcuts {
struct Presentation {
    std::string text;           // UTF-8
    std::string tone;           // active | muted | warning | error
    bool visible = true;        // hidden when the binding is active and matches the row
};
// `status` is capabilities.source.shortcuts, `action` the action name, `shown` the
// shortcut the row displays and `recording` true while this row records a chord.
Presentation present(const json::Object& status, const std::string& action, const std::string& shown, bool recording);
}
