#pragma once
#include "Error.h"
#include <QDateTime>
#include <QJsonObject>
#include <QLoggingCategory>
#include <QProcess>
namespace putgd {
Q_DECLARE_LOGGING_CATEGORY(logApp)
Q_DECLARE_LOGGING_CATEGORY(logOperation)
Q_DECLARE_LOGGING_CATEGORY(logS3)
Q_DECLARE_LOGGING_CATEGORY(logProton)
Q_DECLARE_LOGGING_CATEGORY(logCapture)
Q_DECLARE_LOGGING_CATEGORY(logUi)
Q_DECLARE_LOGGING_CATEGORY(logIpc)
Q_DECLARE_LOGGING_CATEGORY(logSettings)
Q_DECLARE_LOGGING_CATEGORY(logHistory)
Q_DECLARE_LOGGING_CATEGORY(logRecording)
// Persistent JSON Lines log (#74, docs/diagnostics.md). Only the resident opens
// the files; helpers write stderr, which the resident collects (HelperOutput).
// Records are queued for one writer thread: a full queue or a failed write
// drops the record and reports loggingDegraded, never failing or delaying the
// caller. Every record is redacted when written.
namespace logging {
constexpr int defaultDays=14;
constexpr qint64 defaultBytes=50LL*1024*1024;
constexpr qint64 rotationBytes=5LL*1024*1024;
constexpr int maximumQueuedRecords=4096;
struct Retention {int days=defaultDays;qint64 bytes=defaultBytes;};
// $XDG_STATE_HOME/putgd, else ~/.local/state/putgd. QStandardPaths::StateLocation
// needs Qt 6.7; the minimum is 6.4.2, so the path is resolved here.
QString stateRoot();
// Starts the writer and installs the message handler. An unusable folder
// leaves logging degraded; it never stops the resident.
void open(const QString& directory,Retention={},const QString& process={});
// Writes everything queued, then stops the writer and restores the handler.
void close();
bool isOpen();
void setRetention(Retention);
// Debug records (put.gd categories and qDebug) are kept only while on.
// QT_LOGGING_RULES still overrides individual categories.
void setDebug(bool,const QDateTime& expiresAt={});
bool debug();
void write(const QLoggingCategory&,QtMsgType,const QString& message,const QJsonObject& fields={});
// A record for collected helper output names that process instead of this one.
void write(const QLoggingCategory&,QtMsgType,const QString& message,const QJsonObject& fields,const QString& process,qint64 pid);
inline void debug(const QLoggingCategory& c,const QString& m,const QJsonObject& f={}) {write(c,QtDebugMsg,m,f);}
inline void info(const QLoggingCategory& c,const QString& m,const QJsonObject& f={}) {write(c,QtInfoMsg,m,f);}
inline void warning(const QLoggingCategory& c,const QString& m,const QJsonObject& f={}) {write(c,QtWarningMsg,m,f);}
inline void error(const QLoggingCategory& c,const QString& m,const QJsonObject& f={}) {write(c,QtCriticalMsg,m,f);}
// Error code, message and details; S3 details carry the HTTP status, service
// code and request IDs.
QJsonObject errorFields(const Error&);
// {available, directory, files, bytes, oldestRecord, degraded, degradedReason,
//  droppedRecords, debugLogging, retention}; reads cached figures only.
QJsonObject status();
// Applies the retention limits on the writer thread.
void cleanup();
// Removes every log file and starts a new one. Blocks until the writer has
// done it (at most a few seconds): call it off the session thread.
QJsonObject clear(Error&);
// Waits until every record queued before the call is written. Tests only.
void flush();
// The files, oldest first, and the one being written.
QStringList files();
QString currentFile();
// Hard redaction applied to every record: authorization headers, session
// tokens, presigned query strings and URL fragments, secret-like values and
// access key IDs. Sensitive field names lose their values entirely.
QString redact(const QString&);
QJsonValue redact(const QJsonValue&);
}
// Collects one helper generation's stderr and its exit into the resident log,
// bounded per generation. AWS SDK lines from the S3 worker keep their level.
// A crash record (putgd-crash lines, #124) becomes one "Helper crash record"
// with its own bound, so earlier output never crowds it out.
class HelperOutput {
public:
 static constexpr qint64 generationBytes=64*1024;
 static constexpr qsizetype lineBytes=2048;
 static constexpr qsizetype crashBytes=64*1024;
 HelperOutput(QString process,const QLoggingCategory& category,bool quiet=false);
 void started(qint64 pid,const QJsonObject& fields={});
 void append(const QByteArray&);
 // `requested` marks an exit put.gd asked for (stop, terminate after failure).
 void finished(int code,QProcess::ExitStatus,bool requested=false);
 void failedToStart(const QString& reason);
 qint64 pid() const {return pid_;}
private:
 void line(QByteArray);
 void crashRecord();
 QString process_;const QLoggingCategory& category_;bool quiet_;
 qint64 pid_=0,logged_=0,suppressed_=0;quint64 generation_=0;QByteArray partial_,crash_;
};
}
