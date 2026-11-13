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
//  - the pixels go in a sealed memfd (header, then 8-bit grey) the parent fills
//    with write() and closes once the child has started; the result comes back
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
 // What recognition reads: alpha flattened onto white, then 8-bit grey. The
 // caller applies the crop and any Redact mark first.
 static QImage grey(const QImage&);
 // A job is running in this process.
 static bool busy();
 // `grey` is Format_Grayscale8 of at most 24 megapixels. On failure the result
 // is empty and error holds one code of textjob::parentErrorCodes(), a child
 // code or `cancelled`.
 TextResult recognize(const QImage& grey,const Options&,Error&,Cancel={}) const;
private:
 QString helperPath_;
 int timeoutMs_;
 QStringList dataDirectories_;
};
}
