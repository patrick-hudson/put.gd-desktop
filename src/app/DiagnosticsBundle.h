#pragma once
#include "OperationService.h"
#include <QDateTime>
#include <QDir>
#include <QSet>
#include <QRegularExpression>
namespace putgd {
// What `diagnostics.bundle` asked for (#74 §3).
struct BundleRequest {
 QString operationId;
 qint64 sinceMs=7LL*24*60*60*1000;
 bool includeNames=false;
 QString output; // absolute .zip path or existing folder; empty means the default folder
 bool preview=false; // report what the package would hold; write nothing
 // After saving locally, upload to a working destination (#123): profileId, or
 // the default capture destination; copyLink also puts the link on the clipboard.
 bool upload=false,copyLink=false;
 QString profileId;
};
// Replaces names, object keys, URLs and private paths in a diagnostics package
// with stable per-package hashes (the same value always gets the same token in
// one package), unless names are included. Known folders become placeholders
// (<home>, <config>, <data>, <state>, <runtime>, <archive>) in both modes.
// The log's hard redaction (credentials, tokens, signed URLs) always applies.
class PackageRedactor {
public:
 explicit PackageRedactor(bool includeNames);
 void root(const QString& path,const QString& placeholder);
 // A name, key or URL that free text may contain (from history or settings).
 void literal(const QString& kind,const QString& value);
 // "<kind:12 hex[.ext]>"; names and keys keep their extension.
 QString token(const QString& kind,const QString& value) const;
 QString text(const QString&) const;
 // Hashes string values by their key (name, url, key, bucket, path ...) and
 // reduces credential references to their mode.
 QJsonValue json(const QJsonValue&,const QString& key={}) const;
 QString paths(QString) const;
 bool includeNames() const {return names_;}
private:
 void compile() const;
 bool names_;QByteArray key_;
 struct Root {QString path,placeholder;QRegularExpression pattern;};
 QList<Root> roots_;
 QHash<QString,QString> literals_;
 mutable QRegularExpression literalPattern_;mutable bool compiled_=false;
};
// Builds diagnostics packages: a .zip with manifest.json, environment.json,
// diagnostics.json, config.redacted.json, operations.json and logs/putgd.jsonl,
// at most 20 MiB (the oldest log records go first). Packages are saved locally
// only; the default folder <archive root>/diagnostics belongs to put.gd, which
// keeps its newest 10 packages and none older than 30 days. A package saved
// anywhere else is never deleted. See docs/diagnostics.md.
class DiagnosticsBundles final:public QObject {
 Q_OBJECT
public:
 static constexpr qint64 maximumBytes=20LL*1024*1024;
 static constexpr int keptPackages=10,keptDays=30,maximumOperations=50;
 static constexpr qint64 maximumSinceMs=365LL*24*60*60*1000;
 struct Context {
  QString databasePath,logsDirectory,defaultFolder,home;
  QHash<QString,QString> roots; // path -> placeholder
  QJsonObject settings,diagnostics,environment;
  QDateTime now=QDateTime::currentDateTimeUtc();
  // coredumpctl to ask for systemd-coredump's stack traces; empty finds it on
  // PATH, "-" skips it (tests).
  QString coredumpctl;
 };
 static constexpr int maximumCoredumps=5;
 static constexpr qsizetype coredumpTextBytes=32*1024;
 // systemd-coredump's text stack traces for put.gd executables since `from`
 // (#124 option B): {available, reason?, entries[{time, pid, signal,
 // executable, storage, stack}]}. Never the core file itself.
 static QJsonObject coredumps(const QString& coredumpctl,const QDateTime& from,const PackageRedactor&);
 DiagnosticsBundles(OperationService&,QString stateRoot,QObject* parent=nullptr);
 // The full diagnostics document (helpers' lastExit included) and the parts of
 // environment.json only the session thread knows (screens, tray, portal).
 void setDiagnostics(std::function<QJsonObject()> callback) {diagnostics_=std::move(callback);}
 void setEnvironment(std::function<QJsonObject()> callback) {environment_=std::move(callback);}
 // The Settings "Test connection" path for one destination (#123).
 using DestinationTest=std::function<void(const QString& profileId,std::function<void(Error)>)>;
 void setDestinationTest(DestinationTest test) {destinationTest_=std::move(test);}
 // A successful check is reused for this long with the same settings revision,
 // so the dialog's check and the upload that follows test the destination once.
 static constexpr qint64 checkReuseMs=10LL*60*1000;
 // The upload destination a package goes to without --profile: the region
 // capture's destination, else the first action that uploads. Empty if none.
 static QString defaultUploadProfile(const Settings&);
 // Whether an error says the destination's credentials or configuration are
 // wrong, rather than a passing network or service problem.
 static bool credentialOrConfigurationError(const Error&);
 // A Test connection that passed in Settings (or here): a failed upload
 // recorded before it no longer blocks uploads to that destination.
 void destinationTested(const QString& profileId);
 // diagnostics.uploadTarget: {available, profileId, label, provider, publicLink,
 // warning, shareExpirationDays?} or {available:false, reason, message}.
 void uploadTarget(const QString& profileId,JsonReply);
 static Error parse(const QJsonObject& params,BundleRequest&);
 void create(const BundleRequest&,JsonReply);
 // Whether create saved a package at this absolute path in this resident, so
 // Show in folder accepts it under any name (#223).
 bool saved(const QString& path) const {return saved_.contains(QDir::cleanPath(path));}
 static QString defaultFolder(const Settings&);
 // Worker-thread work; no session state. Tests call it directly.
 static QJsonObject build(const Context&,const BundleRequest&,Error&);
 static int prune(const QString& folder,const QDateTime& now=QDateTime::currentDateTimeUtc());
private:
 // The working check: the last upload's outcome, then Test connection.
 void check(const QString& profileId,std::function<void(Error,QJsonObject)>);
 void upload(QJsonObject result,const BundleRequest&,JsonReply);
 OperationService& operations_;
 QString stateRoot_;
 std::function<QJsonObject()> diagnostics_,environment_;
 DestinationTest destinationTest_;
 struct Check {QString revision;QDateTime at;};
 QHash<QString,Check> checked_;
 QHash<QString,QDateTime> tested_;
 bool running_=false;
 QSet<QString> saved_;
};
}
