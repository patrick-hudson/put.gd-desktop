#pragma once
#include "core/TextJob.h"
#include <QJsonObject>
#include <QList>

namespace putgd {
// The resident's view of text recognition (#1428). The production backend runs
// one TextJob in the temporary putgd-ocr helper; tests inject a fake
// (tests/fixtures/FakeTextBackend.h). Capability inspection starts no helper
// and loads no engine.
class TextBackend {
public:
 virtual ~TextBackend()=default;
 // {available, error?: {code, message, details}, engines: {tesseract?, zxing?},
 //  landlockAbi, languages: [{id, bytes, kind: "user"|"packaged"|"system"}]}.
 // error is text_helper_missing, engine_unavailable (no English data) or
 // helper_sandbox_unavailable (Landlock older than ABI 3), with what to install.
 virtual QJsonObject capabilities() const=0;
 // The installed languages by identifier, from the names of the data files.
 virtual QList<textjob::Language> languages() const=0;
 // Synchronous: TextService calls it on a worker thread, one job at a time.
 virtual TextResult recognize(const QImage& grey,const textjob::Options&,Error&,TextJob::Cancel)=0;
};
// The helper next to this executable, reading the fixed data directories.
class HelperTextBackend final:public TextBackend {
public:
 // Tests and the fake helper name another executable, deadline and data directories.
 explicit HelperTextBackend(QString helperPath={},int timeoutMs=textjob::maximumDeadlineMs,QStringList dataDirectories={});
 QJsonObject capabilities() const override;
 QList<textjob::Language> languages() const override;
 TextResult recognize(const QImage& grey,const textjob::Options& options,Error& error,TextJob::Cancel cancel) override {return job_.recognize(grey,options,error,std::move(cancel));}
 // The kernel's Landlock ABI, 0 without Landlock: the resident's own query.
 static int landlockAbi();
private:
 TextJob job_;
 QStringList dataDirectories_;
};
}
