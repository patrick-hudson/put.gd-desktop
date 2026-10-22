#pragma once
// Portable rules of the resident-to-window command channel and the window's view
// state (docs/ui-process-protocol.md "View commands", docs/ui-process-state.md):
// the ui.* session parameters the resident checks before forwarding a command,
// the checkpoint the resident keeps and the projection ui.viewState returns.
#include "common/Error.h"
#include <string>

namespace putgd::uistate {
inline constexpr int commandVersion = 1;
inline constexpr int commandDeadlineMs = 10000;
inline constexpr size_t maximumOutstandingCommands = 8;

// The helper command for a ui.* session method (moveDestination, diagnostics,
// historySaveDiagnostics, fileDialog), or empty for any other method.
std::string commandFor(const std::string& method);
// Checks a ui.* method's params as the Linux resident does; invalid_request otherwise.
Error validateCommand(const std::string& method, const json::Object& params);
// editor.fileDialog: {id, action: open|inspect|cancel}.
Error validateEditorFileDialog(const json::Object& params);

// A window checkpoint: schemaVersion 1 with only the main, settings, history,
// activity and appearance objects, compact JSON within protocol::maximumUiStateBytes.
Error validateState(const json::Object& state);
// ui.viewState: {state} plus settings {section, captureRouting} when the checkpoint
// has a capture routing, and the derived appearance. Drafts never leave the resident.
json::Object viewState(const json::Object& checkpoint, const std::string& helperState);
}
