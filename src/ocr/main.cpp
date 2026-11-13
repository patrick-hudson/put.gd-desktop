// putgd-ocr: the temporary text-recognition helper (#1428). One job, then exit.
// It is the only binary that links Tesseract, Leptonica and ZXing-cpp, and it
// needs QtCore alone: no QtGui, QtSql, QtNetwork or QtDBus. The control
// protocol and the two memfds are the contract's (TextJobChild); this file
// confines the process and hands runChild the engine.
#include "Confinement.h"
#include "Recognize.h"
#include "TextJobChild.h"
#include "core/BuildInfo.h"
#include <cstdio>
#include <cstring>
#include <QString>
#include <QStringList>
#include <cstdlib>
#include <sys/syscall.h>
#include <unistd.h>

#if defined(__SANITIZE_ADDRESS__)
#define PUTGD_OCR_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define PUTGD_OCR_ASAN 1
#endif
#endif
#ifdef PUTGD_OCR_ASAN
// LeakSanitizer stops the world with a clone that is not a thread, which the
// seccomp filter kills; sanitizer builds check the helper's memory otherwise.
extern "C" const char* __asan_default_options(){return "detect_leaks=0";}
#endif

namespace {
using namespace putgd;
using namespace putgd::ocr;
// The memory budget (decision 3): peak RSS under 128 MiB with the shipped
// English data, growing by other languages' data, 192 MiB at most.
// RLIMIT_DATA is about one and a half times the budget.
constexpr qint64 mib=1024*1024;
constexpr qint64 baseBudget=128*mib,largestBudget=192*mib;
// RLIMIT_CPU: just above the parent's 20 s deadline for each recognition thread,
// since CPU time adds up across threads.
constexpr int cpuSecondsPerThread=21;
// Recognition stops this long after the job starts and returns what it read,
// truncated, leaving the parent's 20 s deadline time for the rest: decoding,
// the memfd, the result and the copy.
constexpr int budgetMs=16000;
// The parent's data path, read before confinement: Landlock may read only
// these language directories. Relative or dotted entries are not used.
QStringList dataDirectories(){
 QStringList out;
 for(const auto& d:QString::fromLocal8Bit(qgetenv(textjob::dataPathVariable)).split(QLatin1Char(':'),Qt::SkipEmptyParts))
  if(d.startsWith(QLatin1Char('/'))&&!d.contains(QLatin1String("/../"))&&!d.endsWith(QLatin1String("/.."))&&!out.contains(d))out.append(d);
 return out;
}
}

int main(int argc,char** argv){
 if(argc==2&&std::strcmp(argv[1],"--version-json")==0){std::printf("%s\n",putgd::buildManifestJson());return 0;}
 // Only the control pipes, stderr and the two memfds stay open.
 ::syscall(SYS_close_range,unsigned(textjob::outputDescriptor+1),~0u,0u);
 const auto directories=dataDirectories();
 Confinement confinement;confinement.readable=readableDirectories(directories);
 confinement.dataBytes=largestBudget*3/2;confinement.cpuSeconds=cpuSecondsPerThread*workerThreads();
 confinement.fileBytes=textjob::maximumOutputBytes;confinement.descriptors=32;
 QString failed;int abi=0;
 const bool confined=confine(confinement,failed,abi);
 const textjob::Engine engine=[&](const textjob::GreyImage& image,const textjob::Options& options,const QStringList&,TextResult& result)->textjob::ChildFailure {
  JobOptions job;job.languages=options.languages;job.strictLanguages=options.languagesRequired;
  job.words=options.words;job.candidates=options.candidates;job.directories=directories;job.budgetMs=budgetMs;job.memoryBudget=baseBudget;job.memoryCeiling=largestBudget;
  if(job.languages==QStringList{QStringLiteral("eng")}&&!lowerDataLimit(baseBudget*3/2))return {QStringLiteral("helper_sandbox_unavailable"),{}};
  const auto recognized=recognizeImage({image.data,image.width,image.height,image.stride},job);
  if(!recognized.error.isEmpty())return {recognized.error,recognized.failedLanguage};
  result=toTextResult(recognized,job);
  return {};
 };
 return textjob::runChild(argc,argv,engine,confined?QString():QStringLiteral("helper_sandbox_unavailable"));
}
