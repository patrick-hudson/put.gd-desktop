#pragma once
#include "Error.h"
#include "TextJobProtocol.h"
#include <QImage>
#include <QStringList>
#include <atomic>
#include <memory>

namespace putgd {
// One text recognition job in the temporary putgd-ocr helper (#1428), for the
// resident and for the editor. The contract is in TextJobProtocol.h; this is
// its parent side:
//  - the deadline is fixed before the input is prepared, 20 s at most;
//  - the pixels go in a sealed memfd (header, 8-bit grey, then 8-bit RGB) the
//    parent fills strip by strip and closes once the child has started; the
//    image itself is let go before the helper runs; the result comes back
//    in a second memfd, read only after the authenticated checkpoint and a zero
//    exit. Nothing is staged in a directory;
//  - the child gets the two memfds at fixed descriptor numbers, every other
//    descriptor closed at exec, a cleared environment and a parent-death signal;
//  - one job at a time per process: a second call while one runs is refused at
//    once with text_busy. Nothing queues.
// Synchronous: call it from a worker thread, never an event loop the user waits
// on. The helper is started and reaped on the calling thread, which therefore
// outlives it (PR_SET_PDEATHSIG follows the starting thread).
class TextJob final {
public:
 using Cancel=std::shared_ptr<std::atomic_bool>;
 using Options=textjob::Options;
 // Tests and the fake helper name another executable and a shorter deadline.
 explicit TextJob(QString helperPath={},int timeoutMs=textjob::maximumDeadlineMs);
 static QString defaultHelper();
 QString helper() const {return helperPath_;}
 // The directories the child may read language data from, in lookup order;
 // by default the fixed table for this user and installation.
 void setDataDirectories(QStringList directories) {dataDirectories_=std::move(directories);}
 QStringList dataDirectories() const;
 // A job is running in this process.
 static bool busy();
 // `image` is any format of at most 24 megapixels; the caller applies the crop
 // and any Redact mark first. Recognition reads its alpha flattened onto white,
 // as 8-bit grey and as 8-bit RGB. On failure the result is empty and error
 // holds one code of textjob::parentErrorCodes(), a child code or `cancelled`.
 TextResult recognize(QImage image,const Options&,Error&,Cancel={}) const;
private:
 QString helperPath_;
 int timeoutMs_;
 QStringList dataDirectories_;
};
}
