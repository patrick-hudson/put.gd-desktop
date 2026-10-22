#pragma once
// Configuration schema 1: the same document, defaults, validation rules and
// effective-route semantics as the Linux SettingsService (docs/configuration.md).
#include "common/Error.h"
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace putgd {
using ProfileValidator = std::function<Error(const std::string& provider, const json::Object& settings)>;
using CredentialValidator = std::function<Error(const std::string& provider, const json::Object& credentials)>;

struct ActionSettings { std::string destination, profileId, shortcut, publicationMode = "default"; };
struct ArchiveSettings { bool keepUploads = false; std::string root; std::map<std::string, std::string> folders; };
struct ResourceLimits {
    static constexpr long long unlimitedBytes = 1099511627776LL;
    int maxFiles = 100, pendingOperations = 8, concurrentUploads = 3;
    long long maxFileBytes = 2147483648LL, maxBatchBytes = 2147483648LL, pendingArtifactBytes = 2147483648LL;
    long long pendingImageBytes = 67108864LL, clipboardTextBytes = 16777216LL, imagePixels = 100000000LL, imageBytes = 536870912LL;
    long long recordingAnimationSeconds = 60; int recordingAnimationEdge = 1280;
};
struct Settings {
    json::Object document;
    std::map<std::string, ActionSettings> actions;
    std::map<std::string, json::Object> profiles;
    std::vector<std::string> profileOrder;
    ArchiveSettings archive;
    ResourceLimits limits;
    std::string namingMode, collisionPolicy;
    std::string fileTemplate = "{name}.{ext}";
    std::string captureTemplate = "Screen-Capture-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
    std::string imageTemplate = "Image-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
    std::string textTemplate = "Text-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
    std::string recordingTemplate = "Screen-Recording-{yyyy}-{MM}-{dd}-{HH}-{mm}-{ss}.{ext}";
    int failedArtifactDays = 7, recentActivityLimit = 100;
    long long previewCacheBytes = 268435456LL;
    int historyRetentionDays = 0;
    int logDays = 14; long long logBytes = 52428800LL; bool debugLogging = false, crashDumps = false;
    std::string uploadClipboardBehavior = "each_link";
    std::string appearanceTheme = "system";
    bool notifications = true, launchAtLogin = false, unlockCredentialsAtStartup = true;
    bool checkForUpdates = true; std::string updateChannel = "build";
    static constexpr int maximumRecentColors = 6;
    std::vector<std::string> editorRecentColors;

    // The documents folder differs per platform; the caller supplies it.
    static json::Object defaults(const std::string& documentsFolder);
    static json::Object developmentTemplate(const std::string& documentsFolder);
    static std::vector<std::string> artifactByteLimitKeys();
    json::Object effectiveArtifactLimits() const;
    static Error validate(const json::Object&, const ProfileValidator& = {}, const CredentialValidator& = {});
    static json::Object effectiveAction(const json::Object&, const std::string&);
    static Settings fromValidated(const json::Object&);
    static std::vector<std::string> actionNames();
    static std::vector<std::string> invocableActionNames();
    static std::vector<std::string> requiredActionNames();
    static std::string recordingActionName();
    static json::Object normalized(const json::Object&);
    static Error reorderProfiles(const json::Object& document, const json::Array& order, json::Object& reordered);
    const ActionSettings& action(const std::string& name) const;
};
}
