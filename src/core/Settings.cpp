#include "Settings.h"
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
}

QStringList Settings::actionNames() { return {"region", "window", "desktop", "clipboard"}; }

QJsonObject Settings::defaults() {
    QJsonObject actions;
    int shortcut = 1;
    for (const auto& action : actionNames()) {
        actions[action] = QJsonObject{{"destination", "upload"}, {"profileId", QJsonValue::Null},
            {"shortcut", action == "clipboard" ? QString{} : QStringLiteral("Ctrl+Shift+%1").arg(shortcut)}};
        ++shortcut;
    }
    auto documents = QStandardPaths::writableLocation(QStandardPaths::DocumentsLocation);
    if (documents.isEmpty()) documents = QDir::homePath();
    return {{"schemaVersion", 1}, {"actions", actions}, {"profiles", QJsonArray{}},
        {"archive", QJsonObject{{"keepUploads", false}, {"root", QDir(documents).filePath("put.gd")},
            {"folders", QJsonObject{{"images", QJsonValue::Null}, {"videos", QJsonValue::Null},
                {"text", QJsonValue::Null}, {"other", QJsonValue::Null}}}}},
        {"naming", QJsonObject{{"mode", "generated"}, {"collisionPolicy", "rename"}}},
        {"limits", QJsonObject{{"maxFiles", 100}, {"pendingOperations", 8}, {"concurrentUploads", 1},
            {"maxFileBytes", 2147483648.0}, {"maxBatchBytes", 2147483648.0}, {"pendingArtifactBytes", 2147483648.0},
            {"clipboardTextBytes", 16777216.0}, {"imagePixels", 100000000.0}, {"imageBytes", 536870912.0}}},
        {"retention", QJsonObject{{"failedArtifactDays", 7}, {"recentActivityLimit", 100},
            {"previewCacheBytes", 268435456.0}, {"historyRetentionDays", QJsonValue::Null}}},
        {"preferences", QJsonObject{{"notifications", true}, {"launchAtLogin", false}}}};
}

Error Settings::validate(const QJsonObject& doc, const ProfileValidator& validateProfile) {
    if (!keys(doc, {"schemaVersion", "actions", "profiles", "archive", "naming", "limits", "retention", "preferences"}))
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
    }
    if (!doc["actions"].isObject() || !keys(doc["actions"].toObject(), actionNames()))
        return invalid("/actions", "Define exactly region, window, desktop, and clipboard actions.");
    for (const auto& name : actionNames()) {
        const auto value = doc["actions"].toObject()[name];
        const auto action = value.toObject();
        if (!value.isObject() || !keys(action, {"destination", "profileId", "shortcut"}) ||
            !choice(action["destination"], {"upload", "local"}) || !text(action["shortcut"], 0, 128) ||
            !(action["profileId"].isNull() || (identifier(action["profileId"]) && profileIds.contains(action["profileId"].toString()))))
            return invalid("/actions/" + name, "Action fields or profile references are invalid.");
        if (!action["shortcut"].toString().isEmpty()) {
            const auto sequence = QKeySequence::fromString(action["shortcut"].toString(), QKeySequence::PortableText);
            if (sequence.count() != 1 || sequence[0].key() == Qt::Key_unknown || sequence[0].key() == 0)
                return invalid("/actions/" + name + "/shortcut", "Shortcuts must contain one valid key combination.");
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
    if (!doc["naming"].isObject() || !keys(naming, {"mode", "collisionPolicy"}) ||
        !choice(naming["mode"], {"generated", "original"}) || !choice(naming["collisionPolicy"], {"rename", "skip", "overwrite"}))
        return invalid("/naming", "Upload naming or collision policy is invalid.");
    const auto limits = doc["limits"].toObject();
    const QStringList limitKeys{"maxFiles", "pendingOperations", "concurrentUploads", "maxFileBytes", "maxBatchBytes",
        "pendingArtifactBytes", "clipboardTextBytes", "imagePixels", "imageBytes"};
    if (!doc["limits"].isObject() || !keys(limits, limitKeys)) return invalid("/limits", "Resource limit fields are incomplete.");
    for (const auto& key : limitKeys) {
        qint64 maximum = 1099511627776LL; // 1 TiB, well within exact JSON integer representation
        if (key == "maxFiles") maximum = 10000;
        if (key == "pendingOperations") maximum = 1024;
        if (key == "concurrentUploads") maximum = 32;
        if (key == "imagePixels") maximum = 1000000000;
        if (!integer(limits[key], 1, maximum)) return invalid("/limits/" + key, "Resource limits must be positive bounded integers.");
    }
    const auto retention = doc["retention"].toObject();
    if (!doc["retention"].isObject() || !keys(retention, {"failedArtifactDays", "recentActivityLimit", "previewCacheBytes", "historyRetentionDays"}) ||
        !integer(retention["failedArtifactDays"], 0, 36500) || !integer(retention["recentActivityLimit"], 1, 10000) ||
        !integer(retention["previewCacheBytes"], 0, 1099511627776LL) ||
        !(retention["historyRetentionDays"].isNull() || integer(retention["historyRetentionDays"], 1, 36500)))
        return invalid("/retention", "Retention limits are invalid; null history retention means no automatic expiry.");
    const auto preferences = doc["preferences"].toObject();
    if (!doc["preferences"].isObject() || !keys(preferences, {"notifications", "launchAtLogin"}) ||
        !preferences["notifications"].isBool() || !preferences["launchAtLogin"].isBool())
        return invalid("/preferences", "Preferences must be booleans.");
    return {};
}

Settings Settings::fromValidated(const QJsonObject& doc) {
    Settings settings;
    settings.document = doc;
    const auto actions = doc["actions"].toObject();
    for (const auto& name : actionNames()) {
        const auto action = actions[name].toObject();
        settings.actions.insert(name, {action["destination"].toString(), action["profileId"].toString(), action["shortcut"].toString()});
    }
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
    settings.limits.maxFileBytes = limits["maxFileBytes"].toInteger();
    settings.limits.maxBatchBytes = limits["maxBatchBytes"].toInteger();
    settings.limits.pendingArtifactBytes = limits["pendingArtifactBytes"].toInteger();
    settings.limits.clipboardTextBytes = limits["clipboardTextBytes"].toInteger();
    settings.limits.imagePixels = limits["imagePixels"].toInteger();
    settings.limits.imageBytes = limits["imageBytes"].toInteger();
    settings.namingMode = doc["naming"].toObject()["mode"].toString();
    settings.collisionPolicy = doc["naming"].toObject()["collisionPolicy"].toString();
    const auto retention = doc["retention"].toObject();
    settings.failedArtifactDays = retention["failedArtifactDays"].toInt();
    settings.recentActivityLimit = retention["recentActivityLimit"].toInt();
    settings.previewCacheBytes = retention["previewCacheBytes"].toInteger();
    settings.historyRetentionDays = retention["historyRetentionDays"].toInt();
    settings.notifications = doc["preferences"].toObject()["notifications"].toBool();
    settings.launchAtLogin = doc["preferences"].toObject()["launchAtLogin"].toBool();
    return settings;
}
}
