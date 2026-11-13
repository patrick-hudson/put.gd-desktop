#pragma once
// An in-process text recognition backend for tests (#1428): TextService and
// OperationService::setTextBackend take it in place of the putgd-ocr helper.
// It answers with `result` or `error`, records what it was asked, and can hold
// a job until release() so a test can act while recognition "runs".
#include "app/TextBackend.h"
#include <QJsonArray>
#include <QThread>
#include <atomic>
#include <mutex>

namespace putgd::test {
class FakeTextBackend final:public TextBackend {
public:
 // Set before the job starts; read on the worker thread.
 TextResult result;
 Error error;
 QJsonObject report{{"available",true},{"engines",QJsonObject{}},{"landlockAbi",6},{"languages",QJsonArray{QJsonObject{{"id","eng"},{"bytes",1},{"kind","system"}}}}};
 QList<textjob::Language> installed{{"eng","system","/fixture/eng.traineddata",1}};
 std::atomic_int calls{0};
 std::atomic_bool hold{false},running{false};
 void release() {hold.store(false);}
 QImage image() const {std::lock_guard lock(mutex_);return image_;}
 textjob::Options options() const {std::lock_guard lock(mutex_);return options_;}
 QJsonObject capabilities() const override {return report;}
 QList<textjob::Language> languages() const override {return installed;}
 TextResult recognize(const QImage& grey,const textjob::Options& options,Error& failure,TextJob::Cancel cancel) override {
  {std::lock_guard lock(mutex_);image_=grey;options_=options;}
  ++calls;running.store(true);
  while(hold.load()&&!(cancel&&cancel->load()))QThread::msleep(2);
  running.store(false);
  if(cancel&&cancel->load()){failure=textjob::failure("cancelled");return {};}
  failure=error;return failure?TextResult{}:result;
 }
private:
 mutable std::mutex mutex_;
 QImage image_;
 textjob::Options options_;
};
}
