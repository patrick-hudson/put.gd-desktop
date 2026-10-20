#pragma once
#include "OperationService.h"
#include <QDateTime>
#include <QRegularExpression>
namespace putgd {
// What `diagnostics.bundle` asked for (#74 §3).
struct BundleRequest {
 QString operationId;
 qint64 sinceMs=7LL*24*60*60*1000;
 bool includeNames=false;
 QString output; // absolute .zip path or existing folder; empty means the default folder
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
 static Error parse(const QJsonObject& params,BundleRequest&);
 void create(const BundleRequest&,JsonReply);
 static QString defaultFolder(const Settings&);
 // Worker-thread work; no session state. Tests call it directly.
 static QJsonObject build(const Context&,const BundleRequest&,Error&);
 static int prune(const QString& folder,const QDateTime& now=QDateTime::currentDateTimeUtc());
private:
 OperationService& operations_;
 QString stateRoot_;
 std::function<QJsonObject()> diagnostics_,environment_;
 bool running_=false;
};
}
