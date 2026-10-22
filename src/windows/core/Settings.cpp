#include "core/Settings.h"
#include "common/Shortcut.h"
#include "common/Text.h"
#include "core/Naming.h"
#include <algorithm>
#include <cmath>

namespace putgd {
namespace {
using json::Object; using json::Value; using json::Array;
Error invalid(const std::string& path, const std::string& message) { return {"config_invalid", message, {{"path", path}}}; }
bool keys(const Object& object, const std::vector<std::string>& expected) {
    if (object.size() != expected.size()) return false;
    for (const auto& key : expected) if (!object.contains(key)) return false;
    return true;
}
bool textValue(const Value& v, long min = 0, long max = 4096) {
    if (!v.isString()) return false;
    const auto count = text::codePoints(v.stringRef());
    return count >= min && count <= max;
}
bool choice(const Value& v, const std::vector<std::string>& options) {
    return v.isString() && std::find(options.begin(), options.end(), v.stringRef()) != options.end();
}
bool identifier(const Value& v) { return v.isString() && text::matchesIdentifier(v.stringRef()); }
bool absolutePath(const std::string& p) {
    // Windows drive or UNC paths, plus POSIX roots so portable tests can use them.
    if (p.size() >= 3 && ((p[0] >= 'A' && p[0] <= 'Z') || (p[0] >= 'a' && p[0] <= 'z')) && p[1] == ':' && (p[2] == '\\' || p[2] == '/')) return true;
    if (text::startsWith(p, "\\\\") && p.size() > 2) return true;
    return !p.empty() && p[0] == '/';
}
bool folder(const Value& v, bool nullable) { return (nullable && v.isNull()) || (textValue(v, 1) && absolutePath(v.stringRef())); }
constexpr double formerArtifactDefault = 2147483648.0;
bool byteLimit(const Value& v) { return v.isNull() || v == "auto" || json::integer(v, 1, double(ResourceLimits::unlimitedBytes)); }
bool contains(const std::vector<std::string>& list, const std::string& value) { return std::find(list.begin(), list.end(), value) != list.end(); }
}

std::vector<std::string> Settings::artifactByteLimitKeys() { return {"maxFileBytes", "maxBatchBytes", "pendingArtifactBytes"}; }
Object Settings::effectiveArtifactLimits() const {
    auto value = [](long long bytes) { return bytes >= ResourceLimits::unlimitedBytes ? Value() : Value(double(bytes)); };
    return {{"maxFileBytes", value(limits.maxFileBytes)}, {"maxBatchBytes", value(limits.maxBatchBytes)}, {"pendingArtifactBytes", value(limits.pendingArtifactBytes)}};
}
std::vector<std::string> Settings::actionNames() { return {"region", "window", "desktop", "clipboard", "edit"}; }
std::vector<std::string> Settings::requiredActionNames() { return {"region", "window", "desktop", "clipboard"}; }
std::string Settings::recordingActionName() { return "record"; }
std::vector<std::string> Settings::invocableActionNames() { auto names = actionNames(); names.push_back(recordingActionName()); return names; }

Object Settings::normalized(const Object& original) {
    auto doc = original;
    if (doc.value("limits").isObject()) {
        auto limits = doc.value("limits").toObject();
        bool migrated = false;
        for (const auto& key : artifactByteLimitKeys())
            if (limits.value(key).isNumber() && limits.value(key).toDouble() == formerArtifactDefault) { limits[key] = "auto"; migrated = true; }
        if (migrated) doc["limits"] = limits;
    }
    auto actions = doc.value("actions").toObject();
    if (!doc.value("actions").isObject() || !actions.value("region").isObject()) return doc;
    bool changed = false;
    if (!actions.contains("edit")) {
        auto inherited = actions.value("region").toObject();
        inherited["shortcut"] = "";
        actions["edit"] = inherited;
        changed = true;
    }
    if (!actions.contains(recordingActionName())) { actions[recordingActionName()] = Object{{"shortcut", ""}}; changed = true; }
    if (!changed) return doc;
    doc["actions"] = actions;
    return doc;
}

Object Settings::defaults(const std::string& documents) {
    const Settings initial;
    Object actions;
    int shortcut = 1;
    for (const auto& action : actionNames()) {
        actions[action] = Object{{"destination", "upload"}, {"profileId", Value()},
            {"shortcut", action == "clipboard" || action == "edit" ? std::string() : "Ctrl+Shift+" + std::to_string(shortcut)}};
        ++shortcut;
    }
    actions[recordingActionName()] = Object{{"shortcut", ""}};
    const char separator = documents.find('\\') != std::string::npos || (documents.size() > 1 && documents[1] == ':') ? '\\' : '/';
    const std::string root = documents.empty() ? std::string("put.gd") : documents + separator + "put.gd";
    return {{"schemaVersion", 1}, {"actions", actions}, {"profiles", Array{}},
        {"archive", Object{{"keepUploads", false}, {"root", root},
            {"folders", Object{{"images", Value()}, {"videos", Value()}, {"text", Value()}, {"other", Value()}}}}},
        {"naming", Object{{"mode", "original"}, {"collisionPolicy", "rename"}, {"fileTemplate", initial.fileTemplate},
            {"captureTemplate", initial.captureTemplate}, {"imageTemplate", initial.imageTemplate}, {"textTemplate", initial.textTemplate},
            {"recordingTemplate", initial.recordingTemplate}}},
        {"limits", Object{{"maxFiles", 100}, {"pendingOperations", 8}, {"concurrentUploads", initial.limits.concurrentUploads},
            {"maxFileBytes", "auto"}, {"maxBatchBytes", "auto"}, {"pendingArtifactBytes", "auto"},
            {"pendingImageBytes", initial.limits.pendingImageBytes},
            {"recordingAnimationSeconds", initial.limits.recordingAnimationSeconds}, {"recordingAnimationEdge", initial.limits.recordingAnimationEdge},
            {"clipboardTextBytes", 16777216.0}, {"imagePixels", 100000000.0}, {"imageBytes", 536870912.0}}},
        {"retention", Object{{"failedArtifactDays", 7}, {"recentActivityLimit", 100}, {"previewCacheBytes", 268435456.0}, {"historyRetentionDays", Value()}}},
        {"preferences", Object{{"notifications", true}, {"launchAtLogin", false}, {"uploadClipboardBehavior", "each_link"}}}};
}

Object Settings::developmentTemplate(const std::string& documents) {
    auto document = defaults(documents);
    document["profiles"] = Array{Object{{"id", "development-s3"}, {"label", "put.gd development destination"}, {"provider", "s3"},
        {"settings", Object{{"bucket", "put.gd"}, {"region", "us-east-2"}, {"endpoint", "https://s3.us-east-2.amazonaws.com"}, {"objectPrefix", ""},
            {"storageClass", "STANDARD"}, {"aclMode", "public-read"}, {"publicUrlBase", "https://put.gd"}, {"addressing", "path"},
            {"connectTimeoutMs", 5000}, {"requestTimeoutMs", 60000}, {"lowSpeedBytesPerSecond", 1}, {"lowSpeedTimeSeconds", 30}}},
        {"credentials", Object{{"mode", "keychain"}, {"reference", "putgd/development-s3"}}}}};
    auto actions = document.value("actions").toObject();
    for (const auto& name : actionNames()) { auto action = actions.value(name).toObject(); action["profileId"] = "development-s3"; actions[name] = action; }
    document["actions"] = actions;
    return document;
}

Error Settings::validate(const Object& original, const ProfileValidator& validateProfile, const CredentialValidator& validateCredentials) {
    const auto doc = normalized(original);
    auto root = doc; root.remove("captureDefaults");
    if (!keys(root, {"schemaVersion", "actions", "profiles", "archive", "naming", "limits", "retention", "preferences"}))
        return invalid("/", "Configuration has missing or unsupported fields.");
    if (!json::integer(doc.value("schemaVersion"), 1, 1)) return invalid("/schemaVersion", "Unsupported configuration schema version.");
    if (!doc.value("profiles").isArray() || doc.value("profiles").arrayRef().size() > 32)
        return invalid("/profiles", "Profiles must be an array of at most 32 destinations.");
    std::vector<std::string> profileIds;
    for (const auto& entry : doc.value("profiles").arrayRef()) {
        if (!entry.isObject()) return invalid("/profiles", "Each profile must be an object.");
        const auto& profile = entry.objectRef();
        if (!keys(profile, {"id", "label", "provider", "settings", "credentials"}) || !identifier(profile.value("id")) || !textValue(profile.value("label"), 1, 128) ||
            !identifier(profile.value("provider")) || !profile.value("settings").isObject() || !profile.value("credentials").isObject())
            return invalid("/profiles", "Profile fields or identifiers are invalid.");
        const auto id = profile.value("id").toString();
        if (contains(profileIds, id)) return invalid("/profiles", "Profile identifiers must be unique.");
        profileIds.push_back(id);
        const auto& credentials = profile.value("credentials").objectRef();
        if (!keys(credentials, {"mode", "reference"}) || !choice(credentials.value("mode"), {"keychain", "session"}) ||
            (credentials.value("mode") == "keychain" && !textValue(credentials.value("reference"), 1, 256)) ||
            (credentials.value("mode") == "session" && !credentials.value("reference").isNull()))
            return invalid("/profiles/credentials", "Use a keychain reference or explicitly selected session credentials.");
        if (!validateProfile) return invalid("/profiles/provider", "Provider validation is unavailable.");
        if (auto error = validateProfile(profile.value("provider").toString(), profile.value("settings").objectRef())) return error;
        if (validateCredentials) if (auto error = validateCredentials(profile.value("provider").toString(), credentials)) return error;
    }
    if (doc.contains("captureDefaults")) {
        const auto output = doc.value("captureDefaults").toObject();
        if (!doc.value("captureDefaults").isObject() || !keys(output, {"destination", "profileId", "publicationMode"}) ||
            !choice(output.value("destination"), {"upload", "local"}) || !choice(output.value("publicationMode"), {"default", "share", "private"}) ||
            !(output.value("profileId").isNull() || (identifier(output.value("profileId")) && contains(profileIds, output.value("profileId").toString()))))
            return invalid("/captureDefaults", "Choose a valid default output and destination.");
    }
    auto definedActions = doc.value("actions").toObject();
    const bool hasRecord = definedActions.contains(recordingActionName());
    definedActions.remove(recordingActionName());
    if (!doc.value("actions").isObject() || !keys(definedActions, actionNames()))
        return invalid("/actions", "Define exactly region, window, desktop, clipboard, and optional edit and record actions.");
    auto validatedActions = actionNames();
    if (hasRecord) validatedActions.push_back(recordingActionName());
    for (const auto& name : validatedActions) {
        const Value value = doc.value("actions").objectRef().value(name);
        const auto action = value.toObject();
        const bool inheritedRecord = name == recordingActionName() && value.isObject() && keys(action, {"shortcut"});
        if (inheritedRecord) {
            if (!textValue(action.value("shortcut"), 0, 128)) return invalid("/actions/" + name, "Action fields or profile references are invalid.");
        } else if (!value.isObject() || !(keys(action, {"destination", "profileId", "shortcut"}) || keys(action, {"destination", "profileId", "shortcut", "publicationMode"})) ||
            !choice(action.value("destination"), {"upload", "local", "default"}) || !textValue(action.value("shortcut"), 0, 128) ||
            !(action.value("profileId").isNull() || (identifier(action.value("profileId")) && contains(profileIds, action.value("profileId").toString()))))
            return invalid("/actions/" + name, "Action fields or profile references are invalid.");
        if (!inheritedRecord && action.value("destination") == "default" && (!doc.contains("captureDefaults") || !action.value("profileId").isNull() || action.value("publicationMode").toString("default") != "default"))
            return invalid("/actions/" + name, "Inherited actions need capture defaults and no separate output values.");
        if (action.contains("publicationMode") && !choice(action.value("publicationMode"), {"default", "share", "private"}))
            return invalid("/actions/" + name + "/publicationMode", "Choose default, share or private publication.");
        if (!action.value("shortcut").toString().empty() && !shortcut::valid(action.value("shortcut").stringRef()))
            return invalid("/actions/" + name + "/shortcut", "Shortcuts must contain one valid key combination.");
    }
    const auto archive = doc.value("archive").toObject();
    if (!doc.value("archive").isObject() || !keys(archive, {"keepUploads", "root", "folders"}) || !archive.value("keepUploads").isBool() ||
        !folder(archive.value("root"), false) || !archive.value("folders").isObject() || !keys(archive.value("folders").objectRef(), {"images", "videos", "text", "other"}))
        return invalid("/archive", "Archive settings require an absolute default folder and four category overrides.");
    for (const auto* category : {"images", "videos", "text", "other"})
        if (!folder(archive.value("folders").objectRef().value(category), true)) return invalid(std::string("/archive/folders/") + category, "Folder overrides must be null or absolute paths.");
    const auto naming = doc.value("naming").toObject();
    const std::vector<std::string> templateKeys{"fileTemplate", "captureTemplate", "imageTemplate", "textTemplate", "recordingTemplate"};
    auto namingKeys = templateKeys; namingKeys.push_back("mode"); namingKeys.push_back("collisionPolicy");
    if (!doc.value("naming").isObject() || !naming.contains("mode") || !naming.contains("collisionPolicy") ||
        !choice(naming.value("mode"), {"generated", "original", "template"}) || !choice(naming.value("collisionPolicy"), {"rename", "skip", "overwrite"}))
        return invalid("/naming", "Upload naming or collision policy is invalid.");
    for (const auto& field : naming) if (!contains(namingKeys, field.first)) return invalid("/naming", "Upload naming contains an unsupported field.");
    for (const auto& field : templateKeys) {
        if (!naming.contains(field)) continue;
        if (!textValue(naming.value(field), 1, 4096)) return invalid("/naming/" + field, "Filename templates must be nonempty strings.");
        if (auto error = naming::validateTemplate(naming.value(field).stringRef())) return invalid("/naming/" + field, error.message);
    }
    const auto limits = doc.value("limits").toObject();
    auto requiredLimits = limits;
    requiredLimits.remove("pendingImageBytes"); requiredLimits.remove("recordingAnimationSeconds"); requiredLimits.remove("recordingAnimationEdge");
    const std::vector<std::string> limitKeys{"maxFiles", "pendingOperations", "concurrentUploads", "maxFileBytes", "maxBatchBytes", "pendingArtifactBytes", "clipboardTextBytes", "imagePixels", "imageBytes"};
    if (!doc.value("limits").isObject() || !keys(requiredLimits, limitKeys)) return invalid("/limits", "Resource limit fields are incomplete.");
    if (limits.contains("pendingImageBytes") && !json::integer(limits.value("pendingImageBytes"), 1, 1099511627776.0)) return invalid("/limits/pendingImageBytes", "The pending image budget must be a positive bounded integer.");
    if (limits.contains("recordingAnimationSeconds") && !json::integer(limits.value("recordingAnimationSeconds"), 1, 86400)) return invalid("/limits/recordingAnimationSeconds", "Animated export duration must be 1 to 86400 seconds.");
    if (limits.contains("recordingAnimationEdge") && !json::integer(limits.value("recordingAnimationEdge"), 16, 16384)) return invalid("/limits/recordingAnimationEdge", "Animated export size must be 16 to 16384 pixels.");
    for (const auto& key : limitKeys) {
        double maximum = 1099511627776.0;
        if (key == "maxFiles") maximum = 10000;
        if (key == "pendingOperations") maximum = 1024;
        if (key == "concurrentUploads") maximum = 32;
        if (key == "imagePixels") maximum = 1000000000;
        if (contains(artifactByteLimitKeys(), key)) {
            if (!byteLimit(limits.value(key))) return invalid("/limits/" + key, "File, batch and waiting-upload limits are a positive byte count up to 1 TiB, \"auto\" or null for no limit.");
            continue;
        }
        if (!json::integer(limits.value(key), 1, maximum)) return invalid("/limits/" + key, "Resource limits must be positive bounded integers.");
    }
    const auto retention = doc.value("retention").toObject();
    std::vector<std::string> retentionKeys{"failedArtifactDays", "recentActivityLimit", "previewCacheBytes", "historyRetentionDays"};
    for (const auto* optional : {"logDays", "logBytes"}) if (retention.contains(optional)) retentionKeys.push_back(optional);
    if (!doc.value("retention").isObject() || !keys(retention, retentionKeys) || !json::integer(retention.value("failedArtifactDays"), 0, 36500) ||
        !json::integer(retention.value("recentActivityLimit"), 1, 10000) || !json::integer(retention.value("previewCacheBytes"), 0, 1099511627776.0) ||
        !(retention.value("historyRetentionDays").isNull() || json::integer(retention.value("historyRetentionDays"), 1, 36500)))
        return invalid("/retention", "Retention limits are invalid; null history retention means no automatic expiry.");
    if (retention.contains("logDays") && !json::integer(retention.value("logDays"), 1, 365)) return invalid("/retention/logDays", "Log retention is 1 to 365 days.");
    if (retention.contains("logBytes") && !json::integer(retention.value("logBytes"), 1048576, 1073741824)) return invalid("/retention/logBytes", "Log storage is 1 MiB to 1 GiB.");
    const auto preferences = doc.value("preferences").toObject();
    std::vector<std::string> preferenceKeys{"notifications", "launchAtLogin"};
    for (const auto* optional : {"uploadClipboardBehavior", "unlockCredentialsAtStartup", "editorRecentColors", "diagnostics", "appearance", "updates"})
        if (preferences.contains(optional)) preferenceKeys.push_back(optional);
    if (!doc.value("preferences").isObject() || !keys(preferences, preferenceKeys) || !preferences.value("notifications").isBool() || !preferences.value("launchAtLogin").isBool())
        return invalid("/preferences", "Notifications and launch at login must be booleans.");
    if (preferences.contains("unlockCredentialsAtStartup") && !preferences.value("unlockCredentialsAtStartup").isBool())
        return invalid("/preferences/unlockCredentialsAtStartup", "Unlocking saved credentials at startup must be true or false.");
    if (preferences.contains("editorRecentColors")) {
        const auto& colors = preferences.value("editorRecentColors");
        bool valid = colors.isArray() && colors.arrayRef().size() <= size_t(maximumRecentColors);
        if (valid) for (const auto& color : colors.arrayRef()) valid = valid && color.isString() && text::matchesColor(color.stringRef());
        if (!valid) return invalid("/preferences/editorRecentColors", "Recent editor colors are up to 6 lowercase #rrggbb values.");
    }
    if (preferences.contains("diagnostics")) {
        const auto diagnostics = preferences.value("diagnostics").toObject();
        const bool hasFolder = diagnostics.contains("packageFolder"), hasDumps = diagnostics.contains("crashDumps");
        std::vector<std::string> expected{"debugLogging"};
        if (hasFolder) expected.push_back("packageFolder");
        if (hasDumps) expected.push_back("crashDumps");
        if (!preferences.value("diagnostics").isObject() || !keys(diagnostics, expected) || !diagnostics.value("debugLogging").isBool() ||
            (hasFolder && !folder(diagnostics.value("packageFolder"), true)) || (hasDumps && !diagnostics.value("crashDumps").isBool()))
            return invalid("/preferences/diagnostics", "Diagnostics preferences hold debugLogging (true or false), an optional absolute packageFolder and an optional crashDumps (true or false).");
    }
    if (preferences.contains("updates")) {
        const auto updates = preferences.value("updates").toObject();
        std::vector<std::string> updateKeys;
        for (const auto* optional : {"check", "channel"}) if (updates.contains(optional)) updateKeys.push_back(optional);
        if (!preferences.value("updates").isObject() || !keys(updates, updateKeys) || (updates.contains("check") && !updates.value("check").isBool()) ||
            (updates.contains("channel") && !choice(updates.value("channel"), {"build", "stable", "nightly"})))
            return invalid("/preferences/updates", "Updates hold an optional check (true or false) and channel (build, stable or nightly).");
    }
    if (preferences.contains("appearance")) {
        const auto appearance = preferences.value("appearance").toObject();
        std::vector<std::string> appearanceKeys{"theme"};
        for (const auto* optional : {"accent", "overrides"}) if (appearance.contains(optional)) appearanceKeys.push_back(optional);
        const auto overrides = appearance.value("overrides").toObject();
        const bool overridesEmpty = !appearance.contains("overrides") || (appearance.value("overrides").isObject() && keys(overrides, {"dark", "light"}) &&
            overrides.value("dark").isObject() && overrides.value("light").isObject() && overrides.value("dark").objectRef().empty() && overrides.value("light").objectRef().empty());
        if (!preferences.value("appearance").isObject() || !keys(appearance, appearanceKeys) || !choice(appearance.value("theme"), {"system", "dark", "light"}) ||
            (appearance.contains("accent") && !appearance.value("accent").isNull()) || !overridesEmpty)
            return invalid("/preferences/appearance", "Appearance holds theme (system, dark or light); accent is null and overrides are {\"dark\": {}, \"light\": {}} until custom colours are supported.");
    }
    if (preferences.contains("uploadClipboardBehavior") && !choice(preferences.value("uploadClipboardBehavior"), {"each_link", "list"}))
        return invalid("/preferences/uploadClipboardBehavior", "Choose each_link or list for upload clipboard behavior.");
    return {};
}

Object Settings::effectiveAction(const Object& original, const std::string& name) {
    const auto doc = normalized(original);
    auto action = doc.value("actions").objectRef().value(name).toObject();
    if (name == recordingActionName() && !action.contains("destination")) {
        auto region = doc.value("actions").objectRef().value("region").toObject();
        region["shortcut"] = action.value("shortcut");
        action = region;
    }
    if (action.value("destination") == "default") {
        const auto output = doc.value("captureDefaults").toObject();
        for (const auto* key : {"destination", "profileId", "publicationMode"}) action[key] = output.value(key);
    }
    return action;
}

Settings Settings::fromValidated(const Object& original) {
    const auto doc = normalized(original);
    Settings s;
    s.document = doc;
    for (const auto& name : invocableActionNames()) {
        const auto action = effectiveAction(doc, name);
        s.actions[name] = {action.value("destination").toString(), action.value("profileId").toString(), action.value("shortcut").toString(), action.value("publicationMode").toString("default")};
    }
    for (const auto& entry : doc.value("profiles").arrayRef()) { const auto id = entry.objectRef().value("id").toString(); s.profiles[id] = entry.objectRef(); s.profileOrder.push_back(id); }
    const auto archive = doc.value("archive").toObject();
    s.archive.keepUploads = archive.value("keepUploads").toBool();
    s.archive.root = archive.value("root").toString();
    for (const auto& pair : archive.value("folders").objectRef()) s.archive.folders[pair.first] = pair.second.toString();
    const auto limits = doc.value("limits").toObject();
    auto bytes = [](const Value& v) { return v.isNull() || v.isString() ? ResourceLimits::unlimitedBytes : v.toInteger(); };
    s.limits.maxFiles = limits.value("maxFiles").toInt();
    s.limits.pendingOperations = limits.value("pendingOperations").toInt();
    s.limits.concurrentUploads = limits.value("concurrentUploads").toInt();
    s.limits.maxFileBytes = bytes(limits.value("maxFileBytes"));
    s.limits.maxBatchBytes = bytes(limits.value("maxBatchBytes"));
    s.limits.pendingArtifactBytes = bytes(limits.value("pendingArtifactBytes"));
    s.limits.pendingImageBytes = limits.value("pendingImageBytes").toInteger(s.limits.pendingImageBytes);
    s.limits.clipboardTextBytes = limits.value("clipboardTextBytes").toInteger();
    s.limits.imagePixels = limits.value("imagePixels").toInteger();
    s.limits.imageBytes = limits.value("imageBytes").toInteger();
    s.limits.recordingAnimationSeconds = limits.value("recordingAnimationSeconds").toInteger(s.limits.recordingAnimationSeconds);
    s.limits.recordingAnimationEdge = limits.value("recordingAnimationEdge").toInt(s.limits.recordingAnimationEdge);
    const auto naming = doc.value("naming").toObject();
    s.namingMode = naming.value("mode").toString();
    s.collisionPolicy = naming.value("collisionPolicy").toString();
    s.fileTemplate = naming.value("fileTemplate").toString(s.fileTemplate);
    s.captureTemplate = naming.value("captureTemplate").toString(s.captureTemplate);
    s.imageTemplate = naming.value("imageTemplate").toString(s.imageTemplate);
    s.textTemplate = naming.value("textTemplate").toString(s.textTemplate);
    s.recordingTemplate = naming.value("recordingTemplate").toString(s.recordingTemplate);
    const auto retention = doc.value("retention").toObject();
    s.failedArtifactDays = retention.value("failedArtifactDays").toInt();
    s.recentActivityLimit = retention.value("recentActivityLimit").toInt();
    s.previewCacheBytes = retention.value("previewCacheBytes").toInteger();
    s.historyRetentionDays = retention.value("historyRetentionDays").toInt();
    s.logDays = retention.value("logDays").toInt(s.logDays);
    s.logBytes = retention.value("logBytes").toInteger(s.logBytes);
    const auto preferences = doc.value("preferences").toObject();
    s.uploadClipboardBehavior = preferences.value("uploadClipboardBehavior").toString("each_link");
    s.notifications = preferences.value("notifications").toBool();
    s.launchAtLogin = preferences.value("launchAtLogin").toBool();
    s.unlockCredentialsAtStartup = preferences.value("unlockCredentialsAtStartup").toBool(true);
    s.debugLogging = preferences.value("diagnostics").toObject().value("debugLogging").toBool(false);
    s.crashDumps = preferences.value("diagnostics").toObject().value("crashDumps").toBool(false);
    s.appearanceTheme = preferences.value("appearance").toObject().value("theme").toString("system");
    s.checkForUpdates = preferences.value("updates").toObject().value("check").toBool(true);
    s.updateChannel = preferences.value("updates").toObject().value("channel").toString("build");
    for (const auto& color : preferences.value("editorRecentColors").toArray()) s.editorRecentColors.push_back(color.toString());
    return s;
}
const ActionSettings& Settings::action(const std::string& name) const {
    static const ActionSettings none;
    const auto it = actions.find(name);
    return it == actions.end() ? none : it->second;
}
Error Settings::reorderProfiles(const Object& document, const Array& order, Object& reordered) {
    const auto profiles = document.value("profiles").toArray();
    std::map<std::string, Object> byId;
    for (const auto& entry : profiles) byId[entry.objectRef().value("id").toString()] = entry.objectRef();
    std::vector<std::string> seen, missing, extra, duplicate;
    for (const auto& value : order) {
        if (!value.isString()) return {"profile_order_invalid", "The order must be a list of destination IDs.", {}};
        const auto id = value.stringRef();
        if (contains(seen, id)) { if (!contains(duplicate, id)) duplicate.push_back(id); continue; }
        seen.push_back(id);
        if (!byId.count(id)) extra.push_back(id);
    }
    for (const auto& entry : profiles) { const auto id = entry.objectRef().value("id").toString(); if (!contains(seen, id)) missing.push_back(id); }
    if (!missing.empty() || !extra.empty() || !duplicate.empty())
        return {"profile_order_invalid", "The order must list every destination ID exactly once.",
            {{"missing", json::stringArray(missing)}, {"extra", json::stringArray(extra)}, {"duplicate", json::stringArray(duplicate)}}};
    Array result;
    for (const auto& id : seen) result.push_back(byId[id]);
    reordered = document;
    reordered["profiles"] = result;
    return {};
}
}
