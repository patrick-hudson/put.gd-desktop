#include "core/CrashHandler.h"
#include "core/CrashRecord.h"
#include "core/Log.h"
#include "core/RunMarker.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QTimer>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <memory>
#include <thread>
#include <dlfcn.h>
#include <fcntl.h>
#include <unistd.h>
using namespace putgd;
// tests/fixtures/crash_early_library.cpp, linked so its constructor runs before ours (#211).
extern "C" void putgd_test_early_thread_stack(long long* bytes,int* enabled);
extern "C" void putgd_test_overflow_early_thread();
// The built-in crash handler (#124). The test runs itself as a child that
// crashes on purpose, then checks the record, the exit and the symbols.
namespace {
volatile std::uintptr_t nowhere=0;
[[gnu::noinline]] void crashSite() {*reinterpret_cast<volatile int*>(nowhere)=7;asm volatile("");}
[[gnu::noinline]] void callerTwo(const char* kind) {
 if(std::strcmp(kind,"abort")==0)std::abort();
 if(std::strcmp(kind,"module")==0) {
  void* library=::dlopen(PUTGD_CRASH_MODULE,RTLD_NOW|RTLD_LOCAL);
  if(!library)::_exit(3);
  reinterpret_cast<void(*)()>(::dlsym(library,"putgd_test_crash_in_module"))();
 }
 crashSite();asm volatile("");
}
[[gnu::noinline]] void callerOne(const char* kind) {callerTwo(kind);asm volatile("");}
volatile int stopRecursion=0;
// Recurses until the thread's stack runs into its guard page (#156).
[[gnu::noinline]] int overflowSite(int depth) {
 volatile char frame[1024];frame[0]=char(depth);
 if(stopRecursion)return frame[0];
 return overflowSite(depth+1)+frame[0];
}
// The alternate stack this thread has: {size, enabled}.
QPair<qint64,bool> alternateStack() {stack_t stack{};if(::sigaltstack(nullptr,&stack)!=0)return {0,false};return {qint64(stack.ss_size),!(stack.ss_flags&SS_DISABLE)};}
int mappingCount() {QFile maps("/proc/self/maps");return maps.open(QIODevice::ReadOnly)?int(maps.readAll().count('\n')):-1;}
int child(int argc,char** argv) {
 const char* kind=argv[2];
 if(argc>3) {const int fd=::open(argv[3],O_WRONLY|O_CREAT|O_TRUNC|O_CLOEXEC,0600);if(fd<0)return 4;crash::setOutput(fd);}
 if(std::strcmp(kind,"wait")==0) {
  // The development trigger crashes this process from its event loop.
  QCoreApplication app(argc,argv);QTimer::singleShot(10000,&app,[]{::_exit(5);});return app.exec();
 }
 // Stack overflows on the main thread and on threads started three ways.
 if(std::strcmp(kind,"overflow-main")==0)overflowSite(0);
 if(std::strcmp(kind,"overflow-std")==0){std::thread([]{overflowSite(0);}).join();return 6;}
 if(std::strcmp(kind,"overflow-qthread")==0) {
  QCoreApplication app(argc,argv);std::unique_ptr<QThread> thread(QThread::create([]{overflowSite(0);}));
  thread->start();thread->wait();return 6;
 }
 if(std::strcmp(kind,"overflow-module")==0) {
  void* library=::dlopen(PUTGD_CRASH_MODULE,RTLD_NOW|RTLD_LOCAL);
  if(!library)::_exit(3);
  reinterpret_cast<void(*)()>(::dlsym(library,"putgd_test_overflow_in_module_thread"))();
  return 6;
 }
 // A thread a linked library started from its constructor, before main (#211).
 if(std::strcmp(kind,"overflow-early")==0)putgd_test_overflow_early_thread();
 if(std::strcmp(kind,"early-thread-stack")==0) {
  long long bytes=0;int enabled=0;putgd_test_early_thread_stack(&bytes,&enabled);
  std::printf("%lld %d\n",bytes,enabled);return 0;
 }
 // Whether a new thread has an alternate stack: "<bytes> <enabled>".
 if(std::strcmp(kind,"thread-stack")==0) {
  QPair<qint64,bool> stack;std::thread([&stack]{stack=alternateStack();}).join();
  std::printf("%lld %d\n",static_cast<long long>(stack.first),int(stack.second));return 0;
 }
 callerOne(kind);
 return 0;
}
struct Run {QProcess::ExitStatus status;int code;QByteArray stdoutText,stderrText;};
Run runChild(const QStringList& arguments,const QProcessEnvironment& environment=QProcessEnvironment::systemEnvironment()) {
 QProcess process;process.setProcessEnvironment(environment);
 process.start(QCoreApplication::applicationFilePath(),QStringList{"--crash-child"}+arguments);
 if(!process.waitForFinished(30000)){process.kill();process.waitForFinished();}
 return {process.exitStatus(),process.exitCode(),process.readAllStandardOutput(),process.readAllStandardError()};
}
QString tool(const QStringList& names) {for(const auto& name:names){const auto path=QStandardPaths::findExecutable(name);if(!path.isEmpty())return path;}return {};}
// addr2line or eu-addr2line: the function containing a module address.
QString symbolize(const QString& file,const QString& offset) {
 const auto program=tool({"eu-addr2line","addr2line"});if(program.isEmpty())return {};
 QProcess process;
 process.start(program,QFileInfo(program).fileName()=="eu-addr2line"?QStringList{"-f","-C","-e",file,offset}:QStringList{"-f","-C","-e",file,offset});
 process.waitForFinished(30000);
 return QString::fromUtf8(process.readAllStandardOutput()).split('\n').value(0);
}
QString minusOne(const QString& offset) {bool ok=false;const auto value=offset.toULongLong(&ok,16);return ok?QStringLiteral("0x%1").arg(value-1,0,16):offset;}
QJsonObject moduleOf(const QJsonObject& record,const QJsonObject& frame) {
 for(const auto& value:record["modules"].toArray())if(value.toObject()["index"]==frame["module"])return value.toObject();
 return {};
}
}
class CrashHandlerTest final:public QObject {
 Q_OBJECT
 // A stack overflow on <kind>'s thread: a complete record from that thread,
 // whose top frame is in <file>, and the process still dies by SIGSEGV.
 void verifyOverflowRecord(const char* kind,const QString& file,const char* function,bool worker) {
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto path=directory.filePath("running.crash");
  const auto run=runChild({kind,path});
  QCOMPARE(run.status,QProcess::CrashExit);QCOMPARE(run.code,SIGSEGV);
  QFile output(path);QVERIFY(output.open(QIODevice::ReadOnly));const auto text=output.readAll();
  const auto record=crash::parseRecord(text);
  QVERIFY2(!record.isEmpty(),kind);
  QVERIFY2(record["complete"].toBool(),text.right(2000).constData());
  QCOMPARE(record["signal"].toInt(),SIGSEGV);
  QCOMPARE(record["thread"].toInteger()!=record["pid"].toInteger(),worker);
  const auto frames=record["frames"].toArray();
  QVERIFY2(frames.size()>=16,text.right(2000).constData());
  const auto top=frames[0].toObject();
  const auto module=moduleOf(record,top);
  QCOMPARE(QFileInfo(module["file"].toString()).canonicalFilePath(),QFileInfo(file).canonicalFilePath());
  const auto name=symbolize(module["file"].toString(),top["offset"].toString());
  if(!name.isEmpty())QVERIFY2(name.contains(function),qPrintable(name));
 }
private slots:
 void initTestCase() {
#if defined(__SANITIZE_ADDRESS__)||defined(__SANITIZE_THREAD__)||defined(PUTGD_CRASH_SANITIZED)
  QSKIP("The crash handler is disabled under the sanitizers");
#endif
  QVERIFY(crash::installed());
 }
 // A segfault writes a complete record to the pre-opened file and the process
 // still dies by SIGSEGV, so systemd-coredump and earlier handlers see it.
 void segfaultRecordToFile() {
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto path=directory.filePath("running.crash");
  const auto run=runChild({"segv",path});
  QCOMPARE(run.status,QProcess::CrashExit);QCOMPARE(run.code,SIGSEGV);
  QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto text=file.readAll();
  const auto record=crash::parseRecord(text);
  QVERIFY2(!record.isEmpty(),text.constData());
  QCOMPARE(record["recordVersion"].toInt(),crash::recordVersion);
  QCOMPARE(record["process"].toString(),QFileInfo(QCoreApplication::applicationFilePath()).fileName());
  QCOMPARE(record["signal"].toInt(),SIGSEGV);QCOMPARE(record["signalName"].toString(),QString("SIGSEGV"));
  QCOMPARE(record["faultAddress"].toString(),QString("0x0"));
  QVERIFY(record["complete"].toBool());
  const auto frames=record["frames"].toArray();
  QVERIFY2(frames.size()>=4,text.constData());
  // Frame 0 is the faulting instruction in this executable, with its build ID.
  const auto top=frames[0].toObject();
  QCOMPARE(top["address"].toString(),record["pc"].toString());
  const auto module=moduleOf(record,top);
  QCOMPARE(QFileInfo(module["file"].toString()).canonicalFilePath(),QFileInfo(QCoreApplication::applicationFilePath()).canonicalFilePath());
  QVERIFY2(QRegularExpression("^[0-9a-f]{40}$").match(module["buildId"].toString()).hasMatch(),qPrintable(module["buildId"].toString()));
  // Every frame names its module: nothing here is outside a mapped object.
  for(const auto& value:frames)QVERIFY2(value.toObject().contains("module"),text.constData());
  // The documented maintainer step: the module offset gives the function.
  const auto name=symbolize(module["file"].toString(),top["offset"].toString());
  if(name.isEmpty())QSKIP("No addr2line on this machine");
  QVERIFY2(name.contains("crashSite"),qPrintable(name));
  // Return addresses are looked up one byte back, inside the call.
  QStringList callers;
  for(qsizetype i=1;i<frames.size()&&i<4;++i) {const auto frame=frames[i].toObject();if(moduleOf(record,frame)["file"]==module["file"])callers.append(symbolize(module["file"].toString(),minusOne(frame["offset"].toString())));}
  QVERIFY2(callers.join(' ').contains("callerTwo")&&callers.join(' ').contains("callerOne"),qPrintable(callers.join(' ')));
 }
 // Helpers have no file: abort() writes the record to stderr.
 void abortRecordToStderr() {
  const auto run=runChild({"abort"});
  QCOMPARE(run.status,QProcess::CrashExit);QCOMPARE(run.code,SIGABRT);
  const auto record=crash::parseRecord(run.stderrText);
  QVERIFY2(!record.isEmpty(),run.stderrText.constData());
  QCOMPARE(record["signal"].toInt(),SIGABRT);QVERIFY(record["complete"].toBool());
  QVERIFY(record["frames"].toArray().size()>=3);
  for(const auto& line:run.stderrText.split('\n'))if(!line.isEmpty())QVERIFY2(crash::isRecordLine(line)||!line.startsWith("putgd"),line.constData());
 }
 // A library loaded after startup is not in the early module map; its file and
 // build ID come from /proc/self/maps at crash time.
 void moduleLoadedLaterIsResolved() {
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto path=directory.filePath("running.crash");
  const auto run=runChild({"module",path});
  QCOMPARE(run.status,QProcess::CrashExit);QCOMPARE(run.code,SIGSEGV);
  QFile file(path);QVERIFY(file.open(QIODevice::ReadOnly));const auto text=file.readAll();
  const auto record=crash::parseRecord(text);
  const auto top=record["frames"].toArray().first().toObject();
  const auto module=moduleOf(record,top);
  QCOMPARE(QFileInfo(module["file"].toString()).canonicalFilePath(),QFileInfo(PUTGD_CRASH_MODULE).canonicalFilePath());
  QVERIFY2(QRegularExpression("^[0-9a-f]{40}$").match(module["buildId"].toString()).hasMatch(),text.constData());
  const auto name=symbolize(module["file"].toString(),top["offset"].toString());
  if(!name.isEmpty())QVERIFY2(name.contains("putgd_test_crash_in_module"),qPrintable(name));
 }
 // PUTGD_CRASH_HANDLER=0 leaves the default disposition alone.
 void environmentDisables() {
  auto environment=QProcessEnvironment::systemEnvironment();environment.insert("PUTGD_CRASH_HANDLER","0");
  const auto run=runChild({"segv"},environment);
  QCOMPARE(run.status,QProcess::CrashExit);QCOMPARE(run.code,SIGSEGV);
  QVERIFY(!run.stderrText.contains("putgd-crash"));
  // Nor does it give threads an alternate stack.
  const auto plain=runChild({"thread-stack"},environment);
  QCOMPARE(plain.code,0);QCOMPARE(plain.stdoutText.trimmed(),QByteArray("0 0"));
 }
 // Every thread starts with its own alternate signal stack, so the handler
 // still runs when a worker's stack overflows (#156).
 void stackOverflowOnAnyThreadIsRecorded() {
  const auto self=QCoreApplication::applicationFilePath();
  for(const auto* kind:{"overflow-main","overflow-std","overflow-qthread"}) {
   verifyOverflowRecord(kind,self,"overflowSite",std::strcmp(kind,"overflow-main")!=0);
   if(QTest::currentTestFailed())return;
  }
  // A library's own thread, the way Qt, GStreamer or the AWS SDK start them.
  verifyOverflowRecord("overflow-module",PUTGD_CRASH_MODULE,"overflowInModule",true);
 }
 // The handler installs from .preinit_array, before any library constructor,
 // so a thread a library starts from its constructor has a stack too (#211).
 void threadFromALibraryConstructorIsRecorded() {
  const auto stack=runChild({"early-thread-stack"});
  QCOMPARE(stack.code,0);QCOMPARE(stack.stdoutText.trimmed(),QByteArray("65536 1"));
  verifyOverflowRecord("overflow-early",PUTGD_CRASH_EARLY_LIBRARY,"overflowInEarlyThread",true);
 }
 // The stack is 64 KiB of untouched pages per thread, removed when it ends.
 void threadStacksAreSmallAndReleased() {
  QPair<qint64,bool> stack;std::thread([&stack]{stack=alternateStack();}).join();
  QCOMPARE(stack.first,qint64(64*1024));QVERIFY(stack.second);
  std::unique_ptr<QThread> thread(QThread::create([&stack]{stack=alternateStack();}));thread->start();QVERIFY(thread->wait(10000));
  QCOMPARE(stack.first,qint64(64*1024));QVERIFY(stack.second);
  const int before=mappingCount();QVERIFY(before>0);
  for(int i=0;i<200;++i)std::thread([]{}).join();
  QVERIFY2(mappingCount()<=before+4,qPrintable(QStringLiteral("%1 -> %2").arg(before).arg(mappingCount())));
 }
 // The development trigger crashes a named process from its main thread.
 void developmentTrigger() {
#ifndef PUTGD_CRASH_TRIGGER
  QSKIP("Release builds have no crash trigger");
#else
  auto environment=QProcessEnvironment::systemEnvironment();
  environment.insert("PUTGD_TEST_CRASH",QFileInfo(QCoreApplication::applicationFilePath()).fileName()+":segv:300");
  const auto run=runChild({"wait"},environment);
  QCOMPARE(run.status,QProcess::CrashExit);QCOMPARE(run.code,SIGSEGV);
  const auto record=crash::parseRecord(run.stderrText);
  QCOMPARE(record["signal"].toInt(),SIGSEGV);
  QCOMPARE(record["thread"].toInteger(),record["pid"].toInteger());
  // With "worker", the trigger overflows a thread of its own.
  environment.insert("PUTGD_TEST_CRASH",QFileInfo(QCoreApplication::applicationFilePath()).fileName()+":overflow:300:worker");
  const auto worker=runChild({"wait"},environment);
  QCOMPARE(worker.status,QProcess::CrashExit);QCOMPARE(worker.code,SIGSEGV);
  const auto overflow=crash::parseRecord(worker.stderrText);
  QVERIFY2(overflow["complete"].toBool(),worker.stderrText.right(2000).constData());
  QCOMPARE(overflow["signal"].toInt(),SIGSEGV);
  QVERIFY(overflow["thread"].toInteger()!=overflow["pid"].toInteger());
  // Another process name is left alone.
  environment.insert("PUTGD_TEST_CRASH","putgd-not-this-one:segv");
  const auto quiet=runChild({"wait"},environment);
  QCOMPARE(quiet.status,QProcess::NormalExit);QCOMPARE(quiet.code,5);
#endif
 }
 void parserKeepsOnlyRecordLines() {
  const QByteArray text="Qt warning\nputgd-crash\tbegin\t1\tputgd-ui\t41\t42\nputgd-crash\tversion\t0.1.1-dev.5+gabc\n"
   "putgd-crash\tsignal\t11\tSIGSEGV\t1\t0x10\nputgd-crash\ttime\t1760000000\nputgd-crash\tpc\t0x55d0\n"
   "putgd-crash\tframe\t0\t0x55d0\t0\t0x1d0\nputgd-crash\tframe\t1\t0xdeadzzz\t-\t-\n"
   "putgd-crash\tmodule\t0\t0x5400\t0123456789abcdef0123456789abcdef01234567\t/usr/bin/putgd-ui\n";
  auto record=crash::parseRecord(text);
  QCOMPARE(record["process"].toString(),QString("putgd-ui"));QCOMPARE(record["pid"].toInteger(),41);
  QCOMPARE(record["time"].toString(),QString("2025-10-09T08:53:20Z"));
  QCOMPARE(record["faultAddress"].toString(),QString("0x10"));
  QCOMPARE(record["frames"].toArray().size(),2);
  QCOMPARE(record["frames"][1].toObject()["address"].toString(),QString()); // not hex
  QVERIFY(!record["frames"][1].toObject().contains("module"));
  QCOMPARE(record["modules"][0].toObject()["file"].toString(),QString("/usr/bin/putgd-ui"));
  QVERIFY(!record["complete"].toBool());
  QVERIFY(crash::parseRecord(text+"putgd-crash\tend\t2\n")["complete"].toBool());
  QVERIFY(crash::parseRecord("putgd-crash\tframe\t0\t0x1\t-\t-\n").isEmpty());
 }
 // Helper stderr: the record becomes one log record, even after the
 // generation's ordinary output bound is used up.
 void helperRecordIsLogged() {
  QTemporaryDir directory;QVERIFY(directory.isValid());
  logging::open(directory.filePath("logs"),{},"putgd");
  HelperOutput output("putgd-ui",logUi());output.started(4242);
  output.append(QByteArray(HelperOutput::generationBytes+10,'x')+"\n");
  output.append("putgd-crash\tbegin\t1\tputgd-ui\t4242\t4242\nputgd-crash\tsignal\t6\tSIGABRT\t-6\t0x0\n");
  output.append("putgd-crash\tframe\t0\t0x10\t0\t0x10\nputgd-crash\tmodule\t0\t0x0\t-\t/usr/bin/putgd-ui\nputgd-crash\tend\t1\n");
  output.finished(SIGABRT,QProcess::CrashExit);
  logging::flush();
  QByteArray bytes;const QDir logs(directory.filePath("logs"));
  for(const auto& name:logs.entryList({"putgd-*.jsonl"},QDir::Files,QDir::Name)){QFile file(logs.filePath(name));if(file.open(QIODevice::ReadOnly))bytes+=file.readAll();}
  logging::close();
  QJsonObject found;
  for(const auto& line:bytes.split('\n')){const auto object=QJsonDocument::fromJson(line).object();if(object["message"]=="Helper crash record")found=object;}
  QVERIFY2(!found.isEmpty(),bytes.right(2000).constData());
  QCOMPARE(found["process"].toString(),QString("putgd-ui"));QCOMPARE(found["level"].toString(),QString("error"));
  const auto crash=found["fields"].toObject()["crash"].toObject();
  QCOMPARE(crash["signal"].toInt(),SIGABRT);
  QCOMPARE(crash["frames"].toArray().size(),1);
  QVERIFY(!bytes.contains("putgd-crash\\t"));
 }
 // The resident's record in running.crash makes the next start's unclean-exit
 // record a crash, with the frames.
 void residentRecordOnNextStart() {
  QTemporaryDir directory;QVERIFY(directory.isValid());
  const auto root=directory.filePath("state");QVERIFY(QDir().mkpath(root));
  // A pid no putgd resident has: an exited child's.
  const qint64 pid=999999; // /proc/999999/comm is never "putgd" here
  QFile marker(root+"/running.json");QVERIFY(marker.open(QIODevice::WriteOnly));
  marker.write(QJsonDocument(QJsonObject{{"pid",pid},{"started","2026-10-19T10:00:00.000Z"},{"version","0.1.1"},{"bootId","x"}}).toJson());marker.close();
  QFile note(root+"/running.crash");QVERIFY(note.open(QIODevice::WriteOnly));
  note.write(QStringLiteral("putgd-crash\tbegin\t1\tputgd\t%1\t%1\nputgd-crash\tsignal\t11\tSIGSEGV\t1\t0x0\nputgd-crash\tframe\t0\t0x10\t0\t0x10\n"
   "putgd-crash\tmodule\t0\t0x0\t0123456789abcdef0123456789abcdef01234567\t/usr/bin/putgd\nputgd-crash\tend\t1\n").arg(pid).toUtf8());note.close();
  RunMarker::begin(root,"0.1.2");
  const auto report=RunMarker::report();
  QVERIFY(report["detected"].toBool());
  const auto last=report["last"].toObject();
  QCOMPARE(last["cause"].toString(),QString("crash"));QCOMPARE(last["signal"].toInt(),SIGSEGV);
  QCOMPARE(last["crash"]["modules"][0].toObject()["buildId"].toString(),QString("0123456789abcdef0123456789abcdef01234567"));
  // This run's file is new and empty, and open for the handler.
  QVERIFY(RunMarker::crashOutput()>=0);
  QCOMPARE(QFileInfo(root+"/running.crash").size(),0);
  QCOMPARE(QFileInfo(root+"/running.crash").permissions()&(QFile::ReadGroup|QFile::ReadOther),QFileDevice::Permissions{});
  RunMarker::end();
  QVERIFY(!QFileInfo::exists(root+"/running.crash"));
 }
};
int main(int argc,char** argv) {
 if(argc>=3&&std::strcmp(argv[1],"--crash-child")==0)return child(argc,argv);
 QCoreApplication app(argc,argv);
 CrashHandlerTest test;
 return QTest::qExec(&test,argc,argv);
}
#include "test_crash_handler.moc"
