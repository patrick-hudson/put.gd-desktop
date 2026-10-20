#pragma once
#include "Error.h"
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace putgd {
using ProfileValidator = std::function<Error(const QString&, const QJsonObject&)>;
using CredentialValidator = std::function<Error(const QString&, const QJsonObject&)>;

struct ActionSettings {
    QString destination;
    QString profileId;
    QString shortcut;
    QString publicationMode="default";
};
struct ArchiveSettings {
    bool keepUploads = false;
    QString root;
    QHash<QString, QString> folders;
};
struct ResourceLimits {
    // "No limit" for the artifact byte limits: put.gd applies none of its own up
    // to the configuration's 1 TiB ceiling; free disk space and the destination's
    // maximum object size still apply at import.
    static constexpr qint64 unlimitedBytes = 1099511627776LL;
    int maxFiles = 100;
    int pendingOperations = 8;
    int concurrentUploads = 1;
    qint64 maxFileBytes = 2147483648LL;
    qint64 maxBatchBytes = 2147483648LL;
    qint64 pendingArtifactBytes = 2147483648LL;
    qint64 pendingImageBytes = 67108864LL;
    qint64 clipboardTextBytes = 16777216LL;
    qint64 imagePixels = 100000000LL;
    qint64 imageBytes = 536870912LL;
    // Animated GIF/WebP exports: retained duration and output long edge.
    qint64 recordingAnimationSeconds = 60;
    int recordingAnimationEdge = 1280;
};
// Value type: an accepted operation receives its own immutable copy.
struct Settings {
    QJsonObject document;
    QHash<QString, ActionSettings> actions;
    QHash<QString, QJsonObject> profiles;
    ArchiveSettings archive;
    ResourceLimits limits;
    QString namingMode;
    QString collisionPolicy;
    QString fileTemplate = "{name}.{ext}";
    QString captureTemplate = "Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
    QString imageTemplate = "Image-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
    QString textTemplate = "Text-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
    QString recordingTemplate = "Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
    int failedArtifactDays = 7;
    int recentActivityLimit = 100;
    qint64 previewCacheBytes = 268435456LL;
    int historyRetentionDays = 0; // zero means no automatic expiry
    // Persistent log retention (#74); omitted keys use these defaults.
    int logDays = 14;
    qint64 logBytes = 52428800LL;
    // Opt-in debug logging; the resident turns it off again after 24 hours.
    bool debugLogging = false;
    QString uploadClipboardBehavior = "each_link";
    bool notifications = true;
    bool launchAtLogin = false;
    // Read saved keychain credentials once when the resident starts; the
    // desktop keyring may show its own unlock prompt. Uploads never unlock.
    bool unlockCredentialsAtStartup = true;
    // Most recent first; custom colors picked in the editor.
    static constexpr int maximumRecentColors = 6;
    QStringList editorRecentColors;

    static QJsonObject defaults();
    // maxFileBytes, maxBatchBytes and pendingArtifactBytes: each a byte count, "auto"
    // or null (no limit). Memory does not grow with file size (imports copy in 1 MiB
    // chunks and uploads stream), so "auto" sets no byte limit of put.gd's own; the
    // destination's maximum object size and the free-disk reserve bound every import.
    static QStringList artifactByteLimitKeys();
    // The resolved artifact byte limits, null for no limit.
    QJsonObject effectiveArtifactLimits() const;
    static QJsonObject developmentTemplate();
    static Error validate(const QJsonObject&, const ProfileValidator& = {}, const CredentialValidator& = {});
    static QJsonObject effectiveAction(const QJsonObject&, const QString&);
    static Settings fromValidated(const QJsonObject&);
    // Every output-routed sharing action, including the later optional "edit" action.
    static QStringList actionNames();
    // Everything the tray, the dashboard rail and a global shortcut can start:
    // the sharing actions plus the screen-recording action.
    static QStringList invocableActionNames();
    // Actions a schema-1 document must define; "edit" may be absent.
    static QStringList requiredActionNames();
    // The optional recording route; absent documents inherit the region route.
    static QString recordingActionName();
    // Adds absent edit and record actions inheriting the region route without a shortcut.
    static QJsonObject normalized(const QJsonObject&);
    // The document with its profiles in the given ID order. The order must name
    // every profile exactly once; nothing else in the document changes.
    static Error reorderProfiles(const QJsonObject& document, const QJsonArray& order, QJsonObject& reordered);
};
}
