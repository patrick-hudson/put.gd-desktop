#include "core/ByteSize.h"
#include "Settings.h"
#include "RecordingModel.h"
#include "TextJobProtocol.h"
#include "UploadNaming.h"
#include <QDir>
#include <QJsonArray>
#include <QKeySequence>
#include <QRegularExpression>
#include <QStandardPaths>
#include <cmath>

namespace putgd {
namespace {
Error invalid(const QString& path, const QString& message) {
    return {QStringLiteral("config_invalid"), message, {{QStringLiteral("path"), path}}};
}
bool keys(const QJsonObject& object, const QStringList& expected) {
    if (object.size() != expected.size()) return false;
    for (const auto& key : expected) if (!object.contains(key)) return false;
    return true;
}
bool integer(const QJsonValue& value, qint64 min, qint64 max) {
    if (!value.isDouble()) return false;
    const double number = value.toDouble();
    return std::isfinite(number) && std::floor(number) == number && number >= min && number <= max;
}
bool text(const QJsonValue& value, int min = 0, int max = 4096) {
    if (!value.isString()) return false;
    const auto count = value.toString().toUcs4().size();
    return count >= min && count <= max;
}
bool choice(const QJsonValue& value, const QStringList& options) {
    return value.isString() && options.contains(value.toString());
}
bool identifier(const QJsonValue& value) {
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$"));
    return value.isString() && pattern.match(value.toString()).hasMatch();
}
bool folder(const QJsonValue& value, bool nullable) {
    return (nullable && value.isNull()) || (text(value, 1) && QDir::isAbsolutePath(value.toString()));
}
constexpr qint64 formerArtifactDefault = 2147483648LL; // 2 GiB, saved explicitly before "auto" existed
bool byteLimit(const QJsonValue& value) {
    return value.isNull() || value == QJsonValue(QStringLiteral("auto")) || integer(value, 1, ResourceLimits::unlimitedBytes);
}
}

QStringList Settings::artifactByteLimitKeys() { return {"maxFileBytes", "maxBatchBytes", "pendingArtifactBytes"}; }
QJsonObject Settings::effectiveArtifactLimits() const {
    auto value = [](qint64 bytes) { return bytes >= ResourceLimits::unlimitedBytes ? QJsonValue(QJsonValue::Null) : QJsonValue(double(bytes)); };
    return {{"maxFileBytes", value(limits.maxFileBytes)}, {"maxBatchBytes", value(limits.maxBatchBytes)},
            {"pendingArtifactBytes", value(limits.pendingArtifactBytes)}};
}

QStringList Settings::actionNames() { return {"region", "window", "desktop", "clipboard", "edit", "monitor", "scroll"}; }
QStringList Settings::requiredActionNames() { return {"region", "window", "desktop", "clipboard"}; }
QString Settings::recordingActionName() { return QStringLiteral("record"); }
QString Settings::repeatActionName() { return QStringLiteral("repeat"); }
QString Settings::textActionName() { return QStringLiteral("text"); }
QStringList Settings::invocableActionNames() { return actionNames() << recordingActionName() << repeatActionName() << textActionName(); }
QStringList Settings::presentationOrder() { return {"region", "window", "monitor", "desktop", "edit", "record", "scroll", "clipboard"}; }
QStringList Settings::shortcutOrder() { return {"region", "repeat", "window", "monitor", "desktop", "edit", "record", "scroll", "text", "clipboard"}; }
QList<int> Settings::captureDelayChoices() { return {0, 3, 5, 10}; }
bool Settings::delayableAction(const QString& action) { return QStringList{"region", "window", "monitor", "desktop", "repeat", "edit", "text"}.contains(action); }
bool Settings::delayableEditSource(const QString& source) { return QStringList{"region", "window", "desktop"}.contains(source); }

QJsonObject Settings::normalized(const QJsonObject& original) {
    auto doc = original;
    // Before "auto" existed every configuration saved the 2 GiB default
    // explicitly; that exact value now means "auto".
    if (doc.value("limits").isObject()) {
        auto limits = doc.value("limits").toObject();
        bool migrated = false;
        for (const auto& key : artifactByteLimitKeys())
            if (limits.value(key).isDouble() && limits.value(key).toDouble() == double(formerArtifactDefault)) { limits[key] = QStringLiteral("auto"); migrated = true; }
        if (migrated) doc["limits"] = limits;
    }
    auto actions = doc.value("actions").toObject();
    if (!doc.value("actions").isObject() || !actions.value("region").isObject()) return doc;
    // Edit & upload and screen recording were added after schema 1 shipped.
    // Older documents inherit the region output route with no shortcut; the
    // user assigns a key explicitly. Edit copies the route; record keeps
    // following it (a shortcut-only entry) until a document routes it itself.
    bool changed = false;
    if (!actions.contains("scroll")) { auto a=actions["region"].toObject(); a["shortcut"]=QString{}; actions["scroll"]=a; changed=true; }
    if (!actions.contains("edit")) {
        auto inherited = actions["region"].toObject();
        inherited["shortcut"] = QString{};
        actions["edit"] = inherited;
        changed = true;
    }
    if (!actions.contains(recordingActionName())) {
        actions[recordingActionName()] = QJsonObject{{"shortcut", QString{}}};
        changed = true;
    }
    // Capture monitor and Repeat last region (#1363) came later still. Monitor
    // copies the desktop route; repeat is a key only and always follows region.
    if (!actions.contains("monitor") && actions["desktop"].isObject()) {
        auto inherited = actions["desktop"].toObject();
        inherited["shortcut"] = QString{};
        actions["monitor"] = inherited;
        changed = true;
    }
    if (!actions.contains(repeatActionName())) {
        actions[repeatActionName()] = QJsonObject{{"shortcut", QString{}}};
        changed = true;
    }
    // Copy text (#1428) has no route at all: a hotkey, unassigned until the user sets one.
    if (!actions.contains(textActionName())) {
        actions[textActionName()] = QJsonObject{{"shortcut", QString{}}};
        changed = true;
    }
    if (!changed) return doc;
    auto result = doc;
    result["actions"] = actions;
    return result;
}

QJsonObject Settings::defaults() {
    const Settings initial;
    QJsonObject actions;
    int shortcut = 1;
    for (const auto& action : actionNames()) {
        actions[action] = QJsonObject{{"destination", "upload"}, {"profileId", QJsonValue::Null},
            {"shortcut", shortcut > 3 ? QString{} : QStringLiteral("Ctrl+Shift+%1").arg(shortcut)}};
        ++shortcut;
    }
    // Recording keeps following the region route; only its key is its own.
    actions[recordingActionName()] = QJsonObject{{"shortcut", QString{}}};
    actions[repeatActionName()] = QJsonObject{{"shortcut", QString{}}};
    actions[textActionName()] = QJsonObject{{"shortcut", QString{}}};
    auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty()) documents = QDir::homePath();
    return {{"schemaVersion", 1}, {"actions", actions}, {"profiles", QJsonArray{}},
        {"archive", QJsonObject{{"keepUploads", false}, {"root", QDir(documents).filePath("put.gd")},
            {"folders", QJsonObject{{"images", QJsonValue::Null}, {"videos", QJsonValue::Null},
                {"text", QJsonValue::Null}, {"other", QJsonValue::Null}}}}},
        {"naming", QJsonObject{{"mode", "original"}, {"collisionPolicy", "rename"},
            {"fileTemplate", initial.fileTemplate},
            {"captureTemplate", initial.captureTemplate},
            {"imageTemplate", initial.imageTemplate},
            {"textTemplate", initial.textTemplate},
            {"recordingTemplate", initial.recordingTemplate}}},
        {"limits", QJsonObject{{"maxFiles", 100}, {"pendingOperations", 8}, {"concurrentUploads", initial.limits.concurrentUploads},
            {"maxFileBytes", "auto"}, {"maxBatchBytes", "auto"}, {"pendingArtifactBytes", "auto"},
            {"pendingImageBytes", initial.limits.pendingImageBytes},
            {"recordingAnimationSeconds", initial.limits.recordingAnimationSeconds},
            {"recordingAnimationEdge", initial.limits.recordingAnimationEdge},
            {"clipboardTextBytes", 16777216.0}, {"imagePixels", 100000000.0}, {"imageBytes", 536870912.0}}},
        {"retention", QJsonObject{{"failedArtifactDays", 7}, {"recentActivityLimit", 100},
            {"previewCacheBytes", 268435456.0}, {"historyRetentionDays", QJsonValue::Null}}},
        {"preferences", QJsonObject{{"scrollCaptureMaxHeight",30000},{"scrollCaptureFromTop",true},{"notifications", true}, {"launchAtLogin", false}, {"uploadClipboardBehavior", "each_link"}}}};
}

QJsonObject Settings::developmentTemplate() {
    auto document=defaults();
    document["profiles"]=QJsonArray{QJsonObject{{"id","development-s3"},{"label","put.gd development destination"},{"provider","s3"},
        {"settings",QJsonObject{{"bucket","put.gd"},{"region","us-east-2"},{"endpoint","https://s3.us-east-2.amazonaws.com"},{"objectPrefix",""},{"storageClass","STANDARD"},{"aclMode","public-read"},{"publicUrlBase","https://put.gd"},{"addressing","path"},{"connectTimeoutMs",5000},{"requestTimeoutMs",60000},{"lowSpeedBytesPerSecond",1},{"lowSpeedTimeSeconds",30}}},
        {"credentials",QJsonObject{{"mode","keychain"},{"reference","putgd/development-s3"}}}}};
    auto actions=document["actions"].toObject();for(const auto& name:actionNames()) {auto action=actions[name].toObject();action["profileId"]="development-s3";actions[name]=action;}document["actions"]=actions;return document;
}

Error Settings::validate(const QJsonObject& original, const ProfileValidator& validateProfile, const CredentialValidator& validateCredentials) {
    const auto doc = normalized(original);
    auto root=doc;root.remove("captureDefaults");
    if (!keys(root, {"schemaVersion", "actions", "profiles", "archive", "naming", "limits", "retention", "preferences"}))
        return invalid("/", "Configuration has missing or unsupported fields.");
    if (!integer(doc["schemaVersion"], 1, 1)) return invalid("/schemaVersion", "Unsupported configuration schema version.");
    if (!doc["profiles"].isArray() || doc["profiles"].toArray().size() > 32)
        return invalid("/profiles", "Profiles must be an array of at most 32 destinations.");
    QStringList profileIds;
    for (const auto& entry : doc["profiles"].toArray()) {
        if (!entry.isObject()) return invalid("/profiles", "Each profile must be an object.");
        const auto profile = entry.toObject();
        if (!keys(profile, {"id", "label", "provider", "settings", "credentials"}) ||
            !identifier(profile["id"]) || !text(profile["label"], 1, 128) ||
            !identifier(profile["provider"]) || !profile["settings"].isObject() || !profile["credentials"].isObject())
            return invalid("/profiles", "Profile fields or identifiers are invalid.");
        const auto id = profile["id"].toString();
        if (profileIds.contains(id)) return invalid("/profiles", "Profile identifiers must be unique.");
        profileIds.append(id);
        const auto credentials = profile["credentials"].toObject();
        if (!keys(credentials, {"mode", "reference"}) || !choice(credentials["mode"], {"keychain", "session"}) ||
            (credentials["mode"] == "keychain" && !text(credentials["reference"], 1, 256)) ||
            (credentials["mode"] == "session" && !credentials["reference"].isNull()))
            return invalid("/profiles/credentials", "Use a keychain reference or explicitly selected session credentials.");
        if (!validateProfile) return invalid("/profiles/provider", "Provider validation is unavailable.");
        if (auto error = validateProfile(profile["provider"].toString(), profile["settings"].toObject())) return error;
        if (validateCredentials)
            if (auto error = validateCredentials(profile["provider"].toString(), credentials)) return error;
    }
    if(doc.contains("captureDefaults")) {
        const auto output=doc["captureDefaults"].toObject();
        if(!doc["captureDefaults"].isObject()||!keys(output,{"destination","profileId","publicationMode"})||
           !choice(output["destination"],{"upload","local"})||!choice(output["publicationMode"],{"default","share","private"})||
           !(output["profileId"].isNull()||(identifier(output["profileId"])&&profileIds.contains(output["profileId"].toString()))))
            return invalid("/captureDefaults","Choose a valid default output and destination.");
    }
    auto definedActions = doc["actions"].toObject();
    const bool hasRecord = definedActions.contains(recordingActionName());
    definedActions.remove(recordingActionName());
    definedActions.remove(repeatActionName());
    definedActions.remove(textActionName());
    if (!doc["actions"].isObject() || !keys(definedActions, actionNames()))
        return invalid("/actions", "Define exactly region, window, desktop, clipboard, and optional edit, monitor, record, repeat and text actions.");
    auto validatedActions = actionNames();
    if (hasRecord) validatedActions.append(recordingActionName());
    validatedActions.append(repeatActionName());
    validatedActions.append(textActionName());
    for (const auto& name : validatedActions) {
        // A copy: operator[] on the temporary object would return a reference into it.
        const QJsonValue value = doc["actions"].toObject().value(name);
        const auto action = value.toObject();
        // Repeat last region is a key only: it always uses the region route.
        if (name == repeatActionName() && !(value.isObject() && keys(action, {"shortcut"})))
            return invalid("/actions/" + name, "Repeat last region holds only a hotkey; it uses the region capture's output.");
        // Copy text has no route: nothing but its hotkey is accepted.
        if (name == textActionName() && !(value.isObject() && keys(action, {"shortcut"})))
            return invalid("/actions/" + name, "Copy text holds only a hotkey; it has no output.");
        // A shortcut-only record entry follows the region route.
        const bool inheritedRecord = (name == recordingActionName() || name == repeatActionName() || name == textActionName()) && value.isObject() && keys(action, {"shortcut"});
        if (inheritedRecord) {
            if (!text(action["shortcut"], 0, 128)) return invalid("/actions/" + name, "Action fields or profile references are invalid.");
        } else if (!value.isObject() || !(keys(action, {"destination", "profileId", "shortcut"}) || keys(action, {"destination", "profileId", "shortcut", "publicationMode"})) ||
            !choice(action["destination"], {"upload", "local", "default"}) || !text(action["shortcut"], 0, 128) ||
            !(action["profileId"].isNull() || (identifier(action["profileId"]) && profileIds.contains(action["profileId"].toString()))))
            return invalid("/actions/" + name, "Action fields or profile references are invalid.");
        if(!inheritedRecord&&action["destination"]=="default"&&(!doc.contains("captureDefaults")||!action["profileId"].isNull()||action["publicationMode"].toString("default")!="default"))
            return invalid("/actions/"+name,"Inherited actions need capture defaults and no separate output values.");
        if(action.contains("publicationMode") && !choice(action["publicationMode"],{"default","share","private"}))
            return invalid("/actions/"+name+"/publicationMode","Choose default, share or private publication.");
        if (!action["shortcut"].toString().isEmpty()) {
            const auto sequence = QKeySequence::fromString(action["shortcut"].toString(), QKeySequence::PortableText);
            if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown || sequence[0].key() == 0)
                return invalid("/actions/" + name + "/shortcut", "Hotkeys must contain one valid key combination.");
        }
    }
    const auto archive = doc["archive"].toObject();
    if (!doc["archive"].isObject() || !keys(archive, {"keepUploads", "root", "folders"}) ||
        !archive["keepUploads"].isBool() || !folder(archive["root"], false) || !archive["folders"].isObject() ||
        !keys(archive["folders"].toObject(), {"images", "videos", "text", "other"}))
        return invalid("/archive", "Archive settings require an absolute default folder and four category overrides.");
    for (const auto& category : QStringList{"images", "videos", "text", "other"})
        if (!folder(archive["folders"].toObject()[category], true))
            return invalid("/archive/folders/" + category, "Folder overrides must be null or absolute paths.");
    const auto naming = doc["naming"].toObject();
    const QStringList templateKeys{"fileTemplate", "captureTemplate", "imageTemplate", "textTemplate", "recordingTemplate"};
    auto namingKeys = templateKeys;
    namingKeys.append({"mode", "collisionPolicy"});
    if (!doc["naming"].isObject() || !naming.contains("mode") || !naming.contains("collisionPolicy") ||
        !choice(naming["mode"], {"generated", "original", "template"}) || !choice(naming["collisionPolicy"], {"rename", "skip", "overwrite"}))
        return invalid("/naming", "Upload naming or collision policy is invalid.");
    for (auto field = naming.begin(); field != naming.end(); ++field)
        if (!namingKeys.contains(field.key())) return invalid("/naming", "Upload naming contains an unsupported field.");
    for (const auto& field : templateKeys) {
        if (!naming.contains(field)) continue;
        if (!text(naming[field], 1, 4096)) return invalid("/naming/" + field, "Filename templates must be nonempty strings.");
        if (auto error = UploadNaming::validateTemplate(naming[field].toString()))
            return invalid("/naming/" + field, error.message);
    }
    const auto limits = doc["limits"].toObject();
    auto requiredLimits = limits;
    requiredLimits.remove("pendingImageBytes");
    requiredLimits.remove("recordingAnimationSeconds");
    requiredLimits.remove("recordingAnimationEdge");
    const QStringList limitKeys{"maxFiles", "pendingOperations", "concurrentUploads", "maxFileBytes", "maxBatchBytes",
        "pendingArtifactBytes", "clipboardTextBytes", "imagePixels", "imageBytes"};
    if (!doc["limits"].isObject() || !keys(requiredLimits, limitKeys)) return invalid("/limits", "Resource limit fields are incomplete.");
    if (limits.contains("pendingImageBytes") && !integer(limits["pendingImageBytes"], 1, 1099511627776LL))
        return invalid("/limits/pendingImageBytes", "The pending image budget must be a positive bounded integer.");
    if (limits.contains("recordingAnimationSeconds") && !integer(limits["recordingAnimationSeconds"], 1, 86400))
        return invalid("/limits/recordingAnimationSeconds", "Animated export duration must be 1 to 86400 seconds.");
    if (limits.contains("recordingAnimationEdge") && !integer(limits["recordingAnimationEdge"], 16, 16384))
        return invalid("/limits/recordingAnimationEdge", "Animated export size must be 16 to 16384 pixels.");
    for (const auto& key : limitKeys) {
        qint64 maximum = 1099511627776LL; // 1 TiB, well within exact JSON integer representation
        if (key == "maxFiles") maximum = 10000;
        if (key == "pendingOperations") maximum = 1024;
        if (key == "concurrentUploads") maximum = 32;
        if (key == "imagePixels") maximum = 1000000000;
        if (artifactByteLimitKeys().contains(key)) {
            if (!byteLimit(limits[key])) return invalid("/limits/" + key, ByteSize::labels("File, batch and waiting-upload limits are a positive byte count up to 1 TiB, \"auto\" or null for no limit."));
            continue;
        }
        if (!integer(limits[key], 1, maximum)) return invalid("/limits/" + key, "Resource limits must be positive bounded integers.");
    }
    const auto retention = doc["retention"].toObject();
    QStringList retentionKeys{"failedArtifactDays", "recentActivityLimit", "previewCacheBytes", "historyRetentionDays"};
    for (const auto* optional : {"logDays", "logBytes"})
        if (retention.contains(optional)) retentionKeys.append(optional);
    if (!doc["retention"].isObject() || !keys(retention, retentionKeys) ||
        !integer(retention["failedArtifactDays"], 0, 36500) || !integer(retention["recentActivityLimit"], 1, 10000) ||
        !integer(retention["previewCacheBytes"], 0, 1099511627776LL) ||
        !(retention["historyRetentionDays"].isNull() || integer(retention["historyRetentionDays"], 1, 36500)))
        return invalid("/retention", "Retention limits are invalid; null history retention means no automatic expiry.");
    if (retention.contains("logDays") && !integer(retention["logDays"], 1, 365))
        return invalid("/retention/logDays", "Log retention is 1 to 365 days.");
    if (retention.contains("logBytes") && !integer(retention["logBytes"], 1048576, 1073741824))
        return invalid("/retention/logBytes", ByteSize::labels("Log storage is 1 MiB to 1 GiB."));
    const auto preferences = doc["preferences"].toObject();
    QStringList preferenceKeys{"notifications", "launchAtLogin"};
    for (const auto* optional : {"scrollCaptureMaxHeight", "scrollCaptureFromTop", "uploadClipboardBehavior", "regionSelectionMode", "captureDelaySeconds", "captureDelayShortcuts", "recordingFrameRate", "recordingMicrophone", "recordingMicrophoneDevice", "recordingClickHighlights", "unlockCredentialsAtStartup", "editorRecentColors", "textRecognitionLanguages", "diagnostics", "appearance", "updates", "fileManagerFolders", "fileManagerMenu"})
        if (preferences.contains(optional)) preferenceKeys.append(optional);
    if (!doc["preferences"].isObject() || !keys(preferences, preferenceKeys) ||
        !preferences["notifications"].isBool() || !preferences["launchAtLogin"].isBool())
        return invalid("/preferences", "Notifications and launch at login must be booleans.");
    if (preferences.contains("unlockCredentialsAtStartup") && !preferences["unlockCredentialsAtStartup"].isBool())
        return invalid("/preferences/unlockCredentialsAtStartup", "Unlocking saved credentials at startup must be true or false.");
    if (preferences.contains("editorRecentColors")) {
        const auto colors = preferences["editorRecentColors"];
        bool valid = colors.isArray() && colors.toArray().size() <= Settings::maximumRecentColors;
        if (valid) for (const auto& color : colors.toArray())
            valid = valid && color.isString() && QRegularExpression("^#[0-9a-f]{6}$").match(color.toString()).hasMatch();
        if (!valid) return invalid("/preferences/editorRecentColors", "Recent editor colors are up to 6 lowercase #rrggbb values.");
    }
    if (preferences.contains("diagnostics")) {
        // debugLogging, and the folder Create diagnostics package last saved to (#122).
        const auto diagnostics = preferences["diagnostics"].toObject();
        const bool hasFolder = diagnostics.contains("packageFolder");
        if (!preferences["diagnostics"].isObject() || !keys(diagnostics, hasFolder ? QStringList{"debugLogging", "packageFolder"} : QStringList{"debugLogging"}) ||
            !diagnostics["debugLogging"].isBool() || (hasFolder && !folder(diagnostics["packageFolder"], true)))
            return invalid("/preferences/diagnostics", "Diagnostics preferences hold debugLogging (true or false) and an optional absolute packageFolder.");
    }
    if (preferences.contains("updates")) {
        // #293: both keys optional; check defaults to true, channel to "build".
        const auto updates = preferences["updates"].toObject();
        QStringList updateKeys;
        for (const auto* optional : {"check", "channel"}) if (updates.contains(optional)) updateKeys.append(optional);
        if (!preferences["updates"].isObject() || !keys(updates, updateKeys) || (updates.contains("check") && !updates["check"].isBool()) ||
            (updates.contains("channel") && !choice(updates["channel"], {"build", "stable", "nightly"})))
            return invalid("/preferences/updates", "Updates hold an optional check (true or false) and channel (build, stable or nightly).");
    }
    if (preferences.contains("appearance")) {
        // #60: the theme now; the accent and per-scheme overrides keep their
        // place for phase 2 and must stay at their built-in values until then.
        const auto appearance = preferences["appearance"].toObject();
        QStringList appearanceKeys{"theme"};
        for (const auto* optional : {"accent", "overrides"}) if (appearance.contains(optional)) appearanceKeys.append(optional);
        const auto overrides = appearance["overrides"].toObject();
        const bool overridesEmpty = !appearance.contains("overrides") ||
            (appearance["overrides"].isObject() && keys(overrides, {"dark", "light"}) && overrides["dark"].isObject() && overrides["light"].isObject() &&
             overrides["dark"].toObject().isEmpty() && overrides["light"].toObject().isEmpty());
        if (!preferences["appearance"].isObject() || !keys(appearance, appearanceKeys) || !choice(appearance["theme"], {"system", "dark", "light"}) ||
            (appearance.contains("accent") && !appearance["accent"].isNull()) || !overridesEmpty)
            return invalid("/preferences/appearance", "Appearance holds theme (system, dark or light); accent is null and overrides are {\"dark\": {}, \"light\": {}} until custom colours are supported.");
    }
    if (preferences.contains("uploadClipboardBehavior") && !choice(preferences["uploadClipboardBehavior"], {"each_link", "list"}))
        return invalid("/preferences/uploadClipboardBehavior", "Choose each_link or list for upload clipboard behavior.");
    if (preferences.contains("scrollCaptureMaxHeight") && !integer(preferences["scrollCaptureMaxHeight"],2000,60000)) return invalid("/preferences/scrollCaptureMaxHeight", "Choose 2000 to 60000 pixels.");
    if (preferences.contains("scrollCaptureFromTop") && !preferences["scrollCaptureFromTop"].isBool()) return invalid("/preferences/scrollCaptureFromTop", "Choose true or false.");
    if (preferences.contains("regionSelectionMode") && !choice(preferences["regionSelectionMode"], {"live", "screenshot_first"}))
        return invalid("/preferences/regionSelectionMode", "Choose live or screenshot_first for region selection.");
    if (preferences.contains("captureDelaySeconds") && !(preferences["captureDelaySeconds"].isDouble() && integer(preferences["captureDelaySeconds"], 0, 10) && captureDelayChoices().contains(preferences["captureDelaySeconds"].toInt())))
        return invalid("/preferences/captureDelaySeconds", "Choose 0, 3, 5 or 10 seconds for the capture delay.");
    if (preferences.contains("captureDelayShortcuts") && !preferences["captureDelayShortcuts"].isBool())
        return invalid("/preferences/captureDelayShortcuts", "Delaying hotkey captures is true or false.");
    if (preferences.contains("recordingFrameRate") && !(integer(preferences["recordingFrameRate"], 30, 30) || integer(preferences["recordingFrameRate"], 60, 60)))
        return invalid("/preferences/recordingFrameRate", "Choose 30 or 60 frames per second for recordings.");
    if (preferences.contains("recordingMicrophone") && !preferences["recordingMicrophone"].isBool())
        return invalid("/preferences/recordingMicrophone", "The recording microphone toggle is true or false.");
    if (preferences.contains("recordingMicrophoneDevice") && !recording::validMicrophoneDevice(preferences["recordingMicrophoneDevice"]))
        return invalid("/preferences/recordingMicrophoneDevice", "The recording microphone is the name of an input device, at most 256 characters; leave it empty for the system default.");
    if (preferences.contains("recordingClickHighlights") && !preferences["recordingClickHighlights"].isBool())
        return invalid("/preferences/recordingClickHighlights", "Click highlights in recordings are true or false.");
    if (preferences.contains("fileManagerFolders") && !choice(preferences["fileManagerFolders"], {"zip", "files"}))
        return invalid("/preferences/fileManagerFolders", "Choose zip or files for folders uploaded from a file manager.");
    if (preferences.contains("fileManagerMenu") && !preferences["fileManagerMenu"].isBool())
        return invalid("/preferences/fileManagerMenu", "The file manager menu setting must be true or false.");
    if (preferences.contains("textRecognitionLanguages")) {
        // Identifiers only: whether a language's data is installed is checked when a job loads it.
        const auto languages = preferences["textRecognitionLanguages"];
        QStringList identifiers;
        bool valid = languages.isArray();
        if (valid) for (const auto& language : languages.toArray()) { valid = valid && language.isString(); identifiers.append(language.toString()); }
        if (!valid || !textjob::validLanguages(identifiers))
            return invalid("/preferences/textRecognitionLanguages", "Text recognition languages are one to three different Tesseract language identifiers, such as eng or chi_sim.");
    }
    return {};
}

QJsonObject Settings::effectiveAction(const QJsonObject& original,const QString& name) {
    const auto doc=normalized(original);
    auto action=doc["actions"].toObject()[name].toObject();
    if((name==recordingActionName()||name==repeatActionName())&&!action.contains("destination")) {
        // A shortcut-only record or repeat entry follows the region route with its own key.
        auto inherited=effectiveAction(doc,"region");
        inherited["shortcut"]=action["shortcut"];
        return inherited;
    }
    if(action["destination"]=="default") {
        const auto output=doc["captureDefaults"].toObject();
        for(const auto& key:QStringList{"destination","profileId","publicationMode"})action[key]=output[key];
    }
    return action;
}

Settings Settings::fromValidated(const QJsonObject& original) {
    const auto doc = normalized(original);
    Settings settings;
    settings.document = doc;
    for (const auto& name : actionNames()) {
        const auto action = effectiveAction(doc,name);
        settings.actions.insert(name, {action["destination"].toString(), action["profileId"].toString(), action["shortcut"].toString(),action["publicationMode"].toString("default")});
    }
    // Recording follows the region route unless the document routes it itself (effectiveAction).
    const auto record = effectiveAction(doc, recordingActionName());
    settings.actions.insert(recordingActionName(), {record["destination"].toString(), record["profileId"].toString(), record["shortcut"].toString(), record["publicationMode"].toString("default")});
    const auto repeat = effectiveAction(doc, repeatActionName());
    settings.actions.insert(repeatActionName(), {repeat["destination"].toString(), repeat["profileId"].toString(), repeat["shortcut"].toString(), repeat["publicationMode"].toString("default")});
    // Route-free: no destination or profile, only the hotkey.
    settings.actions.insert(textActionName(), {QString{}, QString{}, doc["actions"].toObject()[textActionName()].toObject()["shortcut"].toString(), QStringLiteral("default")});
    for (const auto& entry : doc["profiles"].toArray()) settings.profiles.insert(entry.toObject()["id"].toString(), entry.toObject());
    const auto archive = doc["archive"].toObject();
    settings.archive.keepUploads = archive["keepUploads"].toBool();
    settings.archive.root = archive["root"].toString();
    const auto folders = archive["folders"].toObject();
    for (auto i = folders.begin(); i != folders.end(); ++i) settings.archive.folders.insert(i.key(), i.value().toString());
    const auto limits = doc["limits"].toObject();
    settings.limits.maxFiles = limits["maxFiles"].toInt();
    settings.limits.pendingOperations = limits["pendingOperations"].toInt();
    settings.limits.concurrentUploads = limits["concurrentUploads"].toInt();
    auto artifactLimit = [&](const char* key) {
        const auto value = limits[QLatin1String(key)];
        return value.isNull() || value.isString() ? ResourceLimits::unlimitedBytes : value.toInteger();
    };
    settings.limits.maxFileBytes = artifactLimit("maxFileBytes");
    settings.limits.maxBatchBytes = artifactLimit("maxBatchBytes");
    settings.limits.pendingArtifactBytes = artifactLimit("pendingArtifactBytes");
    settings.limits.pendingImageBytes = limits["pendingImageBytes"].toInteger(settings.limits.pendingImageBytes);
    settings.limits.recordingAnimationSeconds = limits["recordingAnimationSeconds"].toInteger(settings.limits.recordingAnimationSeconds);
    settings.limits.recordingAnimationEdge = limits["recordingAnimationEdge"].toInt(settings.limits.recordingAnimationEdge);
    settings.limits.clipboardTextBytes = limits["clipboardTextBytes"].toInteger();
    settings.limits.imagePixels = limits["imagePixels"].toInteger();
    settings.limits.imageBytes = limits["imageBytes"].toInteger();
    settings.namingMode = doc["naming"].toObject()["mode"].toString();
    settings.collisionPolicy = doc["naming"].toObject()["collisionPolicy"].toString();
    const auto naming = doc["naming"].toObject();
    settings.fileTemplate = naming["fileTemplate"].toString(settings.fileTemplate);
    settings.captureTemplate = naming["captureTemplate"].toString(settings.captureTemplate);
    settings.imageTemplate = naming["imageTemplate"].toString(settings.imageTemplate);
    settings.textTemplate = naming["textTemplate"].toString(settings.textTemplate);
    settings.recordingTemplate = naming["recordingTemplate"].toString(settings.recordingTemplate);
    const auto retention = doc["retention"].toObject();
    settings.failedArtifactDays = retention["failedArtifactDays"].toInt();
    settings.recentActivityLimit = retention["recentActivityLimit"].toInt();
    settings.previewCacheBytes = retention["previewCacheBytes"].toInteger();
    settings.historyRetentionDays = retention["historyRetentionDays"].toInt();
    settings.logDays = retention["logDays"].toInt(14);
    settings.logBytes = retention["logBytes"].toInteger(52428800LL);
    settings.debugLogging = doc["preferences"].toObject()["diagnostics"].toObject()["debugLogging"].toBool();
    settings.uploadClipboardBehavior = doc["preferences"].toObject()["uploadClipboardBehavior"].toString("each_link");
    settings.scrollCaptureMaxHeight = doc["preferences"].toObject()["scrollCaptureMaxHeight"].toInt(30000);
    settings.scrollCaptureFromTop = doc["preferences"].toObject()["scrollCaptureFromTop"].toBool(true);
    settings.regionSelectionMode = doc["preferences"].toObject()["regionSelectionMode"].toString("live");
    settings.recordingFrameRate = doc["preferences"].toObject()["recordingFrameRate"].toInt(30);
    settings.recordingMicrophone = doc["preferences"].toObject()["recordingMicrophone"].toBool(false);
    settings.recordingMicrophoneDevice = doc["preferences"].toObject()["recordingMicrophoneDevice"].toString();
    settings.recordingClickHighlights = doc["preferences"].toObject()["recordingClickHighlights"].toBool(false);
    settings.captureDelaySeconds = doc["preferences"].toObject()["captureDelaySeconds"].toInt(0);
    settings.captureDelayShortcuts = doc["preferences"].toObject()["captureDelayShortcuts"].toBool(false);
    settings.appearanceTheme = doc["preferences"].toObject()["appearance"].toObject()["theme"].toString("system");
    settings.notifications = doc["preferences"].toObject()["notifications"].toBool();
    settings.launchAtLogin = doc["preferences"].toObject()["launchAtLogin"].toBool();
    settings.unlockCredentialsAtStartup = doc["preferences"].toObject()["unlockCredentialsAtStartup"].toBool(true);
    settings.checkForUpdates = doc["preferences"].toObject()["updates"].toObject()["check"].toBool(true);
    settings.updateChannel = doc["preferences"].toObject()["updates"].toObject()["channel"].toString("build");
    settings.fileManagerFolders = doc["preferences"].toObject()["fileManagerFolders"].toString("zip");
    settings.fileManagerMenu = doc["preferences"].toObject()["fileManagerMenu"].toBool(true);
    for (const auto& color : doc["preferences"].toObject()["editorRecentColors"].toArray()) settings.editorRecentColors.append(color.toString());
    for (const auto& language : doc["preferences"].toObject()["textRecognitionLanguages"].toArray()) settings.textRecognitionLanguages.append(language.toString());
    return settings;
}
Error Settings::reorderProfiles(const QJsonObject& document, const QJsonArray& order, QJsonObject& reordered) {
    const auto profiles = document["profiles"].toArray();
    QHash<QString, QJsonObject> byId;
    for (const auto& entry : profiles) byId.insert(entry.toObject()["id"].toString(), entry.toObject());
    QStringList seen, missing, extra, duplicate;
    for (const auto& value : order) {
        if (!value.isString()) return {"profile_order_invalid", "The order must be a list of destination IDs.", {}};
        const auto id = value.toString();
        if (seen.contains(id)) { if (!duplicate.contains(id)) duplicate.append(id); continue; }
        seen.append(id);
        if (!byId.contains(id)) extra.append(id);
    }
    for (const auto& entry : profiles) if (!seen.contains(entry.toObject()["id"].toString())) missing.append(entry.toObject()["id"].toString());
    if (!missing.isEmpty() || !extra.isEmpty() || !duplicate.isEmpty())
        return {"profile_order_invalid", "The order must list every destination ID exactly once.",
            {{"missing", QJsonArray::fromStringList(missing)}, {"extra", QJsonArray::fromStringList(extra)}, {"duplicate", QJsonArray::fromStringList(duplicate)}}};
    QJsonArray result;
    for (const auto& id : seen) result.append(byId.value(id));
    reordered = document;
    reordered["profiles"] = result;
    return {};
}
}
