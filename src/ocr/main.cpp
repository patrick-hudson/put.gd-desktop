// putgd-ocr: the temporary text-recognition helper (#1428). One job, then exit.
// It is the only binary that links Tesseract, Leptonica and ZXing-cpp, and it
// needs QtCore alone: no QtGui, QtSql, QtNetwork or QtDBus. The control
// protocol and the two memfds are the contract's (TextJobChild); this file
// confines the process and hands runChild the engine.
#include "Confinement.h"
#include "Cores.h"
#include "Recognize.h"
#include "TextJobChild.h"
#include "core/BuildInfo.h"
#include "core/CpuShare.h"
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <QString>
#include <QStringList>
#include <cstdlib>
#include <sys/resource.h>
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
// The memory budget (decision 3): peak RSS under 320 MiB with the shipped
// English data, growing by other languages' data, 384 MiB at most. It gates
// the extra engines: 128 MiB left a 4K screen on one engine (#1808); four
// engines on a dense 24 MP page peak at 258 MiB. The helper exits after the
// job, so the memory is held only while recognition runs.
// RLIMIT_DATA is about one and a half times the budget.
// In Unhinged mode (#1937) memoryBudgets() scales both to the engines, within
// half the memory available.
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
 const auto directories=safeLanguageDirectories(dataDirectories());
 Confinement confinement;confinement.readable=readableDirectories(directories);
 // The engines, counted before confinement: Landlock hides the cgroup's quota.
 // On a machine of mixed cores, one engine for each of the faster CPUs (#1929).
 // Unhinged mode (#1937): every CPU, no preference, and the memory budget the
 // free memory allows, read here too (/proc/meminfo).
 const bool unhinged=qgetenv(textjob::unhingedVariable)=="1";
 const auto cpus=unhinged?QList<int>{}:preferredCpus();
 const int threads=unhinged?workerThreads(0,std::numeric_limits<int>::max()):cpus.isEmpty()?workerThreads():std::min(workerThreads(),int(cpus.size()));
 // The inherited hard RLIMIT_DATA bounds the scaled budget too: a limit the
 // helper can't raise to would fail its confinement.
 rlimit inherited{};const qint64 dataLimit=::getrlimit(RLIMIT_DATA,&inherited)==0&&inherited.rlim_max!=RLIM_INFINITY?qint64(inherited.rlim_max):-1;
 const auto budgets=memoryBudgets(unhinged,threads,unhinged?cpu::memoryAvailable():-1,dataLimit);
 confinement.dataBytes=budgets.ceiling*3/2;confinement.cpuSeconds=cpuSecondsPerThread*threads;
 confinement.fileBytes=textjob::maximumOutputBytes;confinement.descriptors=32;
 QString failed;int abi=0;
 const bool confined=confine(confinement,failed,abi);
 const textjob::Engine engine=[&](const textjob::InputImage& image,const textjob::Options& options,const QStringList&,TextResult& result)->textjob::ChildFailure {
  JobOptions job;job.languages=options.languages;job.strictLanguages=options.languagesRequired;
  // Emoji from the colour plane (#1748), named by the templates built in.
  job.colour={image.colour,image.width,image.height,image.colourStride,image.releaseColour};job.emojiTemplates=&EmojiTemplates::builtIn();
  job.words=options.words;job.candidates=options.candidates;job.directories=directories;job.budgetMs=budgetMs;job.threads=threads;job.memoryBudget=budgets.base;job.memoryCeiling=budgets.ceiling;job.cpus=cpus;
  // More engines, more second reads in the cap: four engines or fewer keep it.
  if(unhinged)job.secondReadScale=std::max(1.0,threads/4.0);
  if(job.languages==QStringList{QStringLiteral("eng")}&&!lowerDataLimit(budgets.base*3/2))return {QStringLiteral("helper_sandbox_unavailable"),{}};
  const auto recognized=recognizeImage({image.data,image.width,image.height,image.stride},job);
  if(!recognized.error.isEmpty())return {recognized.error,recognized.failedLanguage};
  result=toTextResult(recognized,job);
  return {};
 };
 const int status=textjob::runChild(argc,argv,engine,confined?QString():QStringLiteral("helper_sandbox_unavailable"));
 // One job, then out, without exit's destructors and without waiting for an
 // engine thread the job left behind at its time budget (still in an Init or
 // a recognition that overran): the result and checkpoint are already sent
 // and accepted, and nothing such a thread does can reach them.
 std::fflush(nullptr);
 ::_exit(status);
}
