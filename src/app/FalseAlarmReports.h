#pragma once
#include "core/Error.h"
#include "core/Settings.h"
#include <QByteArray>
#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>
#include <atomic>

namespace putgd {
// #2134: false-alarm reports, saved when someone says the sensitive text check's warning was wrong (Not
// sensitive). Each is one folder beside the logs, $XDG_STATE_HOME/putgd/false-alarms/
// putgd-false-alarm-<UTC yyyyMMddTHHmmsszzz>Z-<8 hex>/, holding the original image as it was checked
// (original.png, or original.jpg for a JPEG file), the image with the suggested covers (covered.png) and
// report.json. Folders are 0700 and files 0600, written under a hidden name and renamed whole.
//
// A deliberate exception to "diagnostics hold no pixels or recognized text", approved for debugging false
// alarms; docs/design/diagnostics.md records it and its bounds: these follow the log retention settings
// (logDays, then logBytes for the folder, oldest first, the newest report always kept), Clear logs removes
// them, and a diagnostics package includes them only within its own 64 MiB allowance.
// Only the resident writes here. Safe to call from any thread.
class FalseAlarmReports final {
public:
 explicit FalseAlarmReports(QString directory);
 const QString& directory() const {return directory_;}
 void setRetention(int days,qint64 bytes) {days_.store(days);bytes_.store(bytes);}
 struct Saved {QString name,path;qint64 bytes=0;int pruned=0;};
 // Writes one report and then applies retention, keeping it. `originalSuffix` is png or jpg.
 // `report` gets `files` (each file's name, bytes and SHA-256) before it is written.
 Saved save(const QByteArray& original,const QString& originalSuffix,const QByteArray& covered,QJsonObject report,Error&,const QDateTime& now=QDateTime::currentDateTimeUtc()) const;
 // Reports last written more than `days` ago go, then the oldest until the folder fits `bytes`; the newest
 // stays. Returns how many were removed.
 int prune(const QDateTime& now=QDateTime::currentDateTimeUtc(),const QString& keep={}) const;
 // `left`: report folders (and hidden half-written ones) that are still there afterwards; -1 when the folder
 // itself isn't this user's own directory and was not touched.
 struct Removed {int reports=0;qint64 bytes=0;int left=0;};
 Removed clear() const;
 // diagnostics.falseAlarms: {directory, reports [{name, created, bytes, operationId, reportedFrom, found,
 // covered}], count, bytes, retention {days, bytes}}, newest first. No text, no pictures.
 QJsonObject list() const;
 // For a diagnostics package: whole reports, newest first, that fit `budget` bytes. With `operationId`,
 // that operation's reports; otherwise those created from `from` to `to`.
 struct Packed {QString name;QList<QPair<QString,QByteArray>> files;qint64 bytes=0;};
 // `invalid`: reports in the window that aren't whole (a file missing, extra or not as report.json records it).
 struct Selection {QList<Packed> reports;int omitted=0;qint64 bytes=0;int eligible=0;int invalid=0;};
 // `only`, when set: just these names (a preview's list). eligible = included + omitted + invalid.
 static Selection select(const QString& directory,const QDateTime& from,const QDateTime& to,const QString& operationId,qint64 budget,bool read=true,const QStringList* only=nullptr);
 static constexpr qint64 bundleBytes=64LL*1024*1024;
 // The settings a report records: the check's kinds, recognition languages, Unhinged mode, Quick redact's saved
 // words and patterns as counts only, capture style and Keep a local copy on or off, and the destination's
 // provider. No label, bucket, address, credential or link.
 static QJsonObject settingsSummary(const Settings&,const QString& provider);
 // The desktop the report was made on: desktop, session type, os-release ID and VERSION_ID, kernel,
 // architecture and Qt's runtime version.
 static QJsonObject environment();
 // The whole report.json: the helper's part (exactly reportedFrom, image, check, recognition, findings, words
 // and codes) under the resident's own fields. A helper part of any other shape is refused.
 static QJsonObject compose(const QJsonObject& helper,const QJsonObject& resident,Error&);
 static bool reportName(const QString& name);
private:
 QString directory_;
 std::atomic_int days_{14};std::atomic<qint64> bytes_{52428800};
};
}
