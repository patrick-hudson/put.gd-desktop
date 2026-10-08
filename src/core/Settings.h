#pragma once
#include "Error.h"
#include <QHash>
#include <QJsonObject>
#include <QStringList>
#include <functional>

namespace putgd {
using ProfileValidator = std::function<Error(const QString&, const QJsonObject&)>;

struct ActionSettings {
    QString destination;
    QString profileId;
    QString shortcut;
};
struct ArchiveSettings {
    bool keepUploads = false;
    QString root;
    QHash<QString, QString> folders;
};
struct ResourceLimits {
    int maxFiles = 100;
    int pendingOperations = 8;
    int concurrentUploads = 1;
    qint64 maxFileBytes = 2147483648LL;
    qint64 maxBatchBytes = 2147483648LL;
    qint64 pendingArtifactBytes = 2147483648LL;
    qint64 clipboardTextBytes = 16777216LL;
    qint64 imagePixels = 100000000LL;
    qint64 imageBytes = 536870912LL;
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
    int failedArtifactDays = 7;
    int recentActivityLimit = 100;
    qint64 previewCacheBytes = 268435456LL;
    int historyRetentionDays = 0; // zero means no automatic expiry
    bool notifications = true;
    bool launchAtLogin = false;

    static QJsonObject defaults();
    static Error validate(const QJsonObject&, const ProfileValidator& = {});
    static Settings fromValidated(const QJsonObject&);
    static QStringList actionNames();
};
}
