#pragma once
#include "core/Error.h"
#include "core/TextJob.h"
#include "editor/EditDocument.h"
#include "editor/TextLayer.h"
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QUrl>
#include <functional>
#include <memory>
class QThread;

namespace putgd {
// Text recognition in the editor (#1428): the Copy text tool and Quick redact
// read one result per recognition revision. The editor starts the putgd-ocr
// helper as its own child through TextJob, on a worker thread that outlives it.
//
// What recognition reads is the source inside the crop with every Redact mark
// applied as opaque black; arrows, text, blur and highlight are not part of it.
// The revision changes only with a Redact mark or the crop (TextLayer::
// inputSignature). A new revision cancels a running job and discards its
// result, the selection and the codes; a late result for an old revision is
// rejected; and only while Copy text is the active tool does it start again.
// Any word, candidate or code touching a Redact mark is dropped.
class EditorTextRecognition final:public QObject {
 Q_OBJECT
public:
 // How a job runs: TextJob in production, a fake in tests.
 using Runner=std::function<TextResult(const QImage& grey,const textjob::Options&,Error&,TextJob::Cancel)>;
 explicit EditorTextRecognition(editor::EditDocument&,QObject* parent=nullptr);
 ~EditorTextRecognition() override;
 void setRunner(Runner);
 void setLanguages(QStringList languages) {languages_=std::move(languages);}
 // idle (nothing for this revision), recognizing, done, failed.
 QString state() const {return state_;}
 Error error() const {return error_;}
 // The identity of the current result; 0 without one. It changes with every result.
 quint64 identity() const {return layer_.identity();}
 editor::TextLayer& layer() {return layer_;}
 const editor::TextLayer& layer() const {return layer_;}
 // Every recognized word, unfiltered, with what touches a Redact mark dropped (Quick redact).
 const QList<editor::RecognizedWord>& candidates() const {return candidates_;}
 bool truncated() const {return truncated_;}
 // The Copy all text clipboard text: the words by line with each text code
 // on a line of its own (textjob::copyText's rule), rebuilt from what is left.
 QString allText() const;
 // Recognizes the current revision unless it has a result or a job for it
 // runs; after a failure it tries again.
 void start();
 // Copy text is the active tool: a new revision starts recognition by itself.
 void setActive(bool);
 bool active() const {return active_;}
 // Call after any change to the document; a new revision starts over.
 void documentChanged();
 // Quick redact and redactText: runs `done` once the current revision has a
 // result (starting or joining its job), or with edit_stale when a Redact mark
 // or the crop changes first, or with the job's failure.
 void whenRecognized(std::function<void(Error)> done);
 // editor.inspect's text block: state, result, words, codes, selection, truncated.
 QJsonObject inspect() const;
 // Code content Open link may open (decision 10): http or https with a host,
 // no control character, at most 2,048 characters. Invalid for anything else.
 static QUrl openableLink(const QString& content);
 // Code content as its row shows it: any unusual character escaped as \u{XXXX}.
 static QString shown(const QString& content);
 // The input of the current revision: the crop, with Redact marks black, in grey.
 static QImage input(const QImage& base,const QRect& crop,const QList<QRect>& redactions);
signals:
 void changed();
private:
 void run();
 void finished(quint64 revision,Error,std::shared_ptr<TextResult>);
 void fail(const Error&);
 editor::EditDocument& doc_;
 Runner runner_;
 QStringList languages_;
 editor::TextLayer layer_;
 QList<editor::RecognizedWord> candidates_;
 QList<int> lineTops_;
 bool truncated_=false,active_=false,restart_=false;
 QString state_="idle";Error error_;
 QByteArray signature_;quint64 revision_=1,jobRevision_=0,nextIdentity_=1;
 TextJob::Cancel cancel_;
 QPointer<QThread> worker_;
 QList<std::function<void(Error)>> waiting_;
};
}
