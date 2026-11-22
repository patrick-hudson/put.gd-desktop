#pragma once
#include "Error.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <QStringList>

class QFile;

namespace putgd {
// Thread-safe handle to one live editor. The session thread queues agent
// commands; the editor worker relays them over the helper's private pipes.
class EditorSession final {
public:
 using Reply=std::function<void(Error,QJsonObject)>;
 // Replies run on the editor worker thread; callers marshal them.
 void request(const QString& command,const QJsonObject& params,Reply);
 // #1804: a decision for the prompt now shown. `prompt` above 0 must be that prompt's number
 // (prompts are counted per session, one per checked file); checked under the same lock that queues it.
 void decide(const QString& choice,int prompt,Reply);
 void notePrompt(){std::lock_guard lock(mutex_);++prompts_;phase_="prompt";}
 // The prompt now waiting, by number, or 0: phase and number read together (#1829).
 int waitingPrompt() const {std::lock_guard lock(mutex_);return phase_=="prompt"?prompts_:0;}
 void setAppearance(const QString&);
 QString takeAppearance();
 void cancel() {cancelled_.store(true);}
 bool cancelled() const {return cancelled_.load();}
 // starting → editing → finalizing → closed; a recoverable preparation
 // failure returns finalizing → editing.
 QString phase() const;
 struct Pending {QString command;QJsonObject params;Reply reply;};
 QList<Pending> take();
 void setPhase(const QString&);
 // Copy image: the resident puts the helper's checked PNG on the clipboard
 // and calls `done` once (from any thread); the worker relays the outcome.
 using CopyHandler=std::function<void(QByteArray png,std::function<void(Error)> done)>;
 void setCopyHandler(CopyHandler);
 CopyHandler copyHandler() const;
 void finishCopy(Error);
 std::optional<Error> takeCopyResult();
 // Copy text (#1428): the editor's recognized text or a code's content, which
 // the resident copies as an explicit copy; `done` once, from any thread.
 using TextCopyHandler=std::function<void(QString text,std::function<void(Error)> done)>;
 void setTextCopyHandler(TextCopyHandler);
 TextCopyHandler textCopyHandler() const;
 void finishTextCopy(const QString& copyId,Error);
 QList<std::pair<QString,Error>> takeTextCopyResults();
 // #1804: a check reached its prompt ("prompt", with kinds and counts only) or
 // became an edit ("editing"). Called on the editor worker thread.
 using PhaseHandler=std::function<void(const QString& phase,const QJsonObject& findings)>;
 void setPhaseHandler(PhaseHandler);
 PhaseHandler phaseHandler() const;
private:
 PhaseHandler phaseHandler_;int prompts_=0;
 mutable std::mutex mutex_;
 QList<Pending> queue_;
 CopyHandler copyHandler_;std::optional<Error> copyResult_;
 TextCopyHandler textCopyHandler_;QList<std::pair<QString,Error>> textCopyResults_;
 QString phase_="starting",pendingAppearance_;
 std::atomic_bool cancelled_{false};
};

// recentColors is set whenever the helper reached its checkpoint.
// `check` (#1804) is how a check ended without an edit: clean, upload, cancel or expired.
struct EditorResult {QByteArray png;int width=0,height=0;Error error;std::optional<QStringList> recentColors;QString check;QJsonObject findings;};

// Synchronous supervision of `putgd-ui --edit-image`. Run on a background
// worker. Startup and finalization have deadlines; editing itself does not.
class EditorJob final {
public:
 static constexpr qint64 admissionPixels=24000000;
 static constexpr int maximumQueuedCommands=16;
 // The largest edited PNG the editor hands to the clipboard.
 static constexpr qint64 maximumCopyBytes=128LL*1024*1024;
 // The longest text the editor hands to the clipboard, and how many such copies may wait.
 static constexpr qsizetype maximumTextCopyCharacters=4*1024*1024;
 static constexpr int maximumTextCopies=4;
 EditorJob(QString stagingRoot,QString helperPath={},int startupTimeoutMs=20000,int finalizeTimeoutMs=120000,int commandTimeoutMs=8000);
 // Custom colors the editor offers as quick picks; it reports them back.
 EditorJob& withRecentColors(QStringList colors){recentColors_=std::move(colors);return *this;}
 // An existing image can be edited and saved without a submission output.
 EditorJob& withSubmissionError(Error error){submissionError_=std::move(error);return *this;}
 // "dark" or "light": the helper's scheme, the interface's resolved one.
 EditorJob& withAppearance(QString scheme){appearance_=std::move(scheme);return *this;}
 // The saved text recognition languages (#1428) for Copy text and Quick redact.
 EditorJob& withTextLanguages(QStringList languages){textLanguages_=std::move(languages);return *this;}
 // #1618: Quick redact's saved words and patterns; each key reaches the editor only when its list is not empty.
 EditorJob& withRedactList(QStringList terms,QStringList patterns){redactTerms_=std::move(terms);redactPatterns_=std::move(patterns);return *this;}
 // #1804: check the image for these Quick redact categories before it uploads.
 // `prompt` shows the prompt to a person; without it the check waits for an
 // agent's decide. `redact` offers Redact and upload. `file` names the file
 // of a selection ("File 2 of 3: notes.png").
 struct Check {QStringList categories;bool prompt=true,redact=true;QString file;};
 EditorJob& withCheck(Check check){check_=std::move(check);return *this;}
 // The longest a check may take to say what it found, from the helper's start.
 static constexpr int checkTimeoutMs=40000;
 // An agent's decision: the helper cancels after 5 minutes; this backstop follows.
 static constexpr int decisionBackstopMs=5*60*1000+30000;
 // Writes `image` to private staging, then releases it before the helper
 // starts. Returns only bytes that pass identity and sanitized-PNG checks.
 EditorResult run(QImage& image,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>&) const;
 // Stages encoded PNG bytes (a scrolling capture, #1804) without decoding them here.
 EditorResult runEncoded(const QByteArray& png,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>&) const;
 // Stages a PNG/JPEG file's bytes without decoding them in this process.
 EditorResult runFile(const QString& path,qint64 maximumInputBytes,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>&) const;
 // Removes abandoned staging left by an earlier resident. Call before editing.
 static void discardStaging(const QString& stagingRoot);
private:
 EditorResult supervise(const std::function<bool(QFile&)>& stage,const QString& destination,qint64 outputBytes,const std::shared_ptr<EditorSession>&) const;
 QString root_,helperPath_,appearance_=QStringLiteral("dark");QStringList recentColors_,textLanguages_,redactTerms_,redactPatterns_;
 Error submissionError_;
 std::optional<Check> check_;
 int startupTimeoutMs_,finalizeTimeoutMs_,commandTimeoutMs_;
};
}
