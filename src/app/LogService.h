#pragma once
#include "core/SettingsService.h"
#include <QDateTime>
#include <QObject>
#include <QTimer>
#include <functional>
namespace putgd {
// Applies the logging settings to the resident log (#74): retention limits,
// cleanup once a day, and debug logging, which turns itself off 24 hours after
// it was turned on. When it was turned on is kept in the state folder, so a
// restart does not extend it.
class LogService final:public QObject {
 Q_OBJECT
public:
 static constexpr qint64 debugLifetimeMs=24LL*60*60*1000;
 static constexpr int cleanupIntervalMs=24*60*60*1000;
 LogService(SettingsService&,QString stateRoot,QObject* parent=nullptr);
 void apply();
 // {enabled, enabledAt, expiresAt} for diagnostics.
 QJsonObject debugStatus() const;
 // Test seam: the current UTC time.
 void setClock(std::function<QDateTime()> clock) {clock_=std::move(clock);}
 // Test seam: how often retention cleanup runs (cleanupIntervalMs by default).
 void setCleanupInterval(int ms) {daily_.setInterval(ms);}
 int cleanupInterval() const {return daily_.interval();}
private:
 void expire();
 QDateTime now() const {return clock_?clock_():QDateTime::currentDateTimeUtc();}
 SettingsService& settings_;
 QString statePath_;
 QTimer daily_,expiry_;
 QDateTime enabledAt_;
 bool active_=false;
 std::function<QDateTime()> clock_;
};
}
