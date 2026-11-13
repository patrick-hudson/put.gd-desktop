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
    // Files transferring at once across every operation (#28); a multipart file uses one.
    int concurrentUploads = 3;
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
    // preferences.regionSelectionMode (#629): live (default) keeps the desktop
    // updating under the selector; screenshot_first freezes every monitor first.
    int scrollCaptureMaxHeight = 30000;
    bool scrollCaptureFromTop = true;
    QString regionSelectionMode = "live";
    // preferences.recordingFrameRate (#984): 30 (default) or 60 frames per
    // second for recordings that don't ask for a rate; the recorder may still
    // lower it while a recording runs when the machine can't keep up.
    int recordingFrameRate = 30;
    // preferences.captureDelaySeconds (#1363): 0 (default), 3, 5 or 10. The tray
    // and the dashboard start region, window, monitor and desktop captures, a
    // repeat and Capture region & edit after this delay; session requests name
    // their own. Shortcuts stay immediate unless captureDelayShortcuts is on.
    int captureDelaySeconds = 0;
    bool captureDelayShortcuts = false;
    // #1364: preferences.recordingMicrophone remembers the bar's microphone
    // toggle (off until the user turns it on); recordingMicrophoneDevice names
    // the input to open, empty for the system default; recordingClickHighlights
    // draws a ring at the pointer on each click, off by default.
    bool recordingMicrophone = false;
    QString recordingMicrophoneDevice;
    bool recordingClickHighlights = false;
    // preferences.appearance.theme (#60): system, dark or light.
    QString appearanceTheme = "system";
    bool notifications = true;
    bool launchAtLogin = false;
    // Read saved keychain credentials once when the resident starts; the
    // desktop keyring may show its own unlock prompt. Uploads never unlock.
    bool unlockCredentialsAtStartup = true;
    // preferences.updates (#293): a daily check for a newer version, on the
    // feed of this build's channel ("build") or a chosen one.
    bool checkForUpdates = true;
    QString updateChannel = "build";
    // preferences.fileManagerFolders (#1105): a folder chosen in a file manager's
    // "Upload with put.gd" uploads as one .zip ("zip", default) or file by file ("files").
    QString fileManagerFolders = "zip";
    // preferences.fileManagerMenu (#1105): the AppImage's per-user menu entries;
    // DEB and RPM entries are system files and always on.
    bool fileManagerMenu = true;
    // preferences.textRecognitionLanguages (#1428): up to three Tesseract language
    // identifiers for Copy text, in order. Empty: the desktop locale's language
    // when its data is installed, then English.
    QStringList textRecognitionLanguages;
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
    // the sharing actions plus the screen-recording, repeat and Copy text actions.
    static QStringList invocableActionNames();
    static QStringList presentationOrder();
    // Actions a schema-1 document must define; "edit" may be absent.
    static QStringList requiredActionNames();
    // The optional recording route; absent documents inherit the region route.
    static QString recordingActionName();
    // Repeat last region (#1363): a shortcut only, always on the region route.
    static QString repeatActionName();
    // Copy text (#1428): route-free. It has a hotkey and no destination, archive,
    // naming or publication, so it is in no output list and never output_required.
    static QString textActionName();
    // Shortcut rows and tray entries: presentationOrder() plus Repeat last region
    // and Copy text, which has no output and so is not in presentationOrder().
    static QStringList shortcutOrder();
    // Delays a capture can start after, in seconds; 0 is immediate.
    static QList<int> captureDelayChoices();
    // Captures that take delaySeconds: region, window, monitor, desktop, repeat,
    // edit and text. An edit is delayed only when it captures the screen
    // (delayableEditSource): the clipboard and a file have nothing to wait for.
    static bool delayableAction(const QString& action);
    static bool delayableEditSource(const QString& source);
    // Adds absent edit and record actions inheriting the region route without a shortcut,
    // an absent monitor action copying the desktop route, and absent repeat and text keys.
    static QJsonObject normalized(const QJsonObject&);
    // The document with its profiles in the given ID order. The order must name
    // every profile exactly once; nothing else in the document changes.
    static Error reorderProfiles(const QJsonObject& document, const QJsonArray& order, QJsonObject& reordered);
};
}
