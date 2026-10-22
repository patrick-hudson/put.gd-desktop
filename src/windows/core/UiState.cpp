#include "core/UiState.h"
#include "common/Protocol.h"
#include <algorithm>
#include <cctype>
#include <vector>

namespace putgd::uistate {
namespace {
Error invalid() { return {"invalid_request", "Invalid request fields or parameter types", {}}; }
bool among(const std::string& value, std::initializer_list<const char*> list) {
    return std::any_of(list.begin(), list.end(), [&](const char* item) { return value == item; });
}
bool only(const json::Object& object, std::initializer_list<const char*> required, std::initializer_list<const char*> optional = {}) {
    for (const auto& pair : object) if (!among(pair.first, required) && !among(pair.first, optional)) return false;
    for (const char* key : required) if (!object.contains(key)) return false;
    return true;
}
bool text(const json::Object& object, const char* key, size_t maximum) {
    const auto& value = object.value(key);
    return value.isString() && !value.stringRef().empty() && value.stringRef().size() <= maximum;
}
bool optionalBool(const json::Object& object, const char* key) { return !object.contains(key) || object.value(key).isBool(); }
// A Windows absolute path: a drive letter with a separator, or a UNC share.
bool absolutePath(const std::string& path) {
    if (path.size() >= 3 && std::isalpha(static_cast<unsigned char>(path[0])) && path[1] == ':' && (path[2] == '\\' || path[2] == '/')) return true;
    return path.size() > 2 && (path[0] == '\\' || path[0] == '/') && path[0] == path[1];
}
}

std::string commandFor(const std::string& method) {
    if (method == "ui.moveDestination") return "moveDestination";
    if (method == "ui.diagnostics") return "diagnostics";
    if (method == "ui.historySaveDiagnostics") return "historySaveDiagnostics";
    if (method == "ui.fileDialog") return "fileDialog";
    return {};
}

Error validateCommand(const std::string& method, const json::Object& p) {
    if (method == "ui.moveDestination") {
        if (!only(p, {"direction"}, {"profileId"}) || !p.value("direction").isString() || !among(p.value("direction").stringRef(), {"up", "down"}) ||
            (p.contains("profileId") && !text(p, "profileId", 64)))
            return invalid();
        return {};
    }
    if (method == "ui.diagnostics") {
        if (!only(p, {"action"}, {"enabled", "includeNames", "folder", "operation", "upload"}) || !p.value("action").isString() ||
            !among(p.value("action").stringRef(), {"inspect", "setDebugLogging", "clearLogs", "openPackage", "setPackage", "savePackage", "showInFolder", "closePackage"}) ||
            !optionalBool(p, "enabled") || !optionalBool(p, "includeNames") || !optionalBool(p, "upload") ||
            (p.contains("folder") && (!text(p, "folder", 4096) || !absolutePath(p.value("folder").stringRef()))) || (p.contains("operation") && !text(p, "operation", 128)))
            return invalid();
        return {};
    }
    if (method == "ui.historySaveDiagnostics") {
        if (!only(p, {"id"}) || !text(p, "id", 128)) return invalid();
        return {};
    }
    if (method == "ui.fileDialog") {
        if (!only(p, {"dialog", "action"}, {"field", "id"}) || !text(p, "dialog", 32) || !text(p, "action", 16)) return invalid();
        const auto& dialog = p.value("dialog").stringRef();
        if (!among(dialog, {"editImage", "historySaveAs", "outputFolder", "diagnosticsFolder"}) || !among(p.value("action").stringRef(), {"open", "inspect", "cancel"})) return invalid();
        if (p.contains("field") && (dialog != "outputFolder" || !text(p, "field", 16) || !among(p.value("field").stringRef(), {"root", "images", "videos", "text", "other"}))) return invalid();
        if (p.contains("id") && (dialog != "historySaveAs" || !text(p, "id", 128))) return invalid();
        return {};
    }
    if (method == "ui.viewState") return p.empty() ? Error{} : invalid();
    return {"unknown_method", "Unknown session method", {}};
}

Error validateEditorFileDialog(const json::Object& p) {
    if (!only(p, {"id", "action"}) || !text(p, "id", 256) || !p.value("action").isString() || !among(p.value("action").stringRef(), {"open", "inspect", "cancel"})) return invalid();
    return {};
}

Error validateState(const json::Object& state) {
    if (state.empty()) return {};
    if (!json::integer(state.value("schemaVersion"), 1, 1)) return {"ui_state_invalid", "The window state has an unsupported schema version", {}};
    for (const auto& pair : state) {
        if (pair.first == "schemaVersion") continue;
        if (!among(pair.first, {"main", "settings", "history", "activity", "appearance"}) || !pair.second.isObject())
            return {"ui_state_invalid", "The window state has an unclassified field", {{"field", pair.first}}};
    }
    if (json::serialize(state).size() > protocol::maximumUiStateBytes) return {"ui_state_limit", "Close some windows or reduce the draft before closing this window.", {}};
    return {};
}

json::Object viewState(const json::Object& checkpoint, const std::string& helperState) {
    json::Object result{{"state", helperState}};
    const auto& settings = checkpoint.value("settings");
    if (settings.isObject() && settings.objectRef().contains("captureRouting"))
        result["settings"] = json::Object{{"section", settings.objectRef().value("section")}, {"captureRouting", settings.objectRef().value("captureRouting")}};
    if (checkpoint.value("appearance").isObject()) result["appearance"] = checkpoint.value("appearance");
    return result;
}
}
