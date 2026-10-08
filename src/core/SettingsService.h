#pragma once
#include "Settings.h"
#include <QFileSystemWatcher>
#include <QObject>
#include <QTimer>

namespace putgd {
class SettingsService final : public QObject {
    Q_OBJECT
public:
    explicit SettingsService(QString path, ProfileValidator validator = {}, QObject* parent = nullptr);
    const QString& path() const { return path_; }
    const QString& revision() const { return acceptedRevision_; }
    const QJsonObject& document() const { return settings_.document; }
    Settings snapshot() const { return settings_; }
    QJsonObject metadata() const;
    Result reload();
    Result save(const QJsonObject& candidate, const QString& expectedRevision);
    Result patch(const QJsonObject& patchObject, const QString& expectedRevision);
    static QJsonObject defaults() { return Settings::defaults(); }
signals:
    void accepted(const QString& revision);
    void statusChanged();
private:
    void armWatcher();
    void scheduleReload();
    QString path_;
    ProfileValidator validator_;
    Settings settings_;
    QString acceptedRevision_;
    QString observedRevision_;
    Error lastError_;
    bool watching_ = false;
    QFileSystemWatcher watcher_;
    QTimer debounce_;
    QTimer fallback_;
};
}
