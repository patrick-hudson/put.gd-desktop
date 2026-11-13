// putgd-ocr's confinement, from inside a test build of the helper (#1428): each
// forbidden thing fails. Every case is its own confined process and runs after
// a banded recognition with the distribution's Tesseract and OpenMP, so the
// filter is shown to hold on every thread the engine started. Runs in the CI
// containers too, where Landlock and seccomp have to be shown to work.
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <cerrno>
#include <csignal>
#include <unistd.h>

namespace {
QString dataDirectory(){
 for(const auto& d:{QStringLiteral("/usr/share/tesseract-ocr/5/tessdata"),QStringLiteral("/usr/share/tesseract/tessdata"),QStringLiteral("/usr/share/tessdata")})
  if(QFileInfo::exists(d+QStringLiteral("/eng.traineddata")))return d;
 return {};
}
}
class OcrConfinement final:public QObject {
 Q_OBJECT
 QTemporaryDir home_{QDir::homePath()+QStringLiteral("/.putgd-ocr-probe-XXXXXX")};
private slots:
 void forbidden_data(){
  QTest::addColumn<QString>("action");QTest::addColumn<QString>("argument");
  QTest::addColumn<int>("errorOrSignal");  // a positive errno, or minus the signal that killed it
  QTest::addColumn<bool>("fromHome");      // run a copy of the probe installed under the home-like directory
  QVERIFY(home_.isValid());
  {
   // The Landlock ABI of this kernel, for the record kept per target.
   QProcess abi;abi.start(qEnvironmentVariable("PUTGD_OCR_PROBE"),{QStringLiteral("none")});
   if(abi.waitForFinished(30000))qInfo("%s",abi.readAllStandardOutput().split('\n').value(0).constData());
  }
  const auto secret=home_.filePath(QStringLiteral("secret.txt"));
  QFile file(secret);QVERIFY(file.open(QIODevice::WriteOnly));file.write("synthetic");file.close();
  const QString parent=QString::number(::getpid());
  QTest::newRow("socket")<<"socket-inet"<<""<<EPERM<<false;
  QTest::newRow("unix-socket")<<"socket-unix"<<""<<EPERM<<false;
  QTest::newRow("exec")<<"exec"<<""<<-SIGSYS<<false;
  QTest::newRow("child-process")<<"fork"<<""<<-SIGSYS<<false;
  QTest::newRow("write-home")<<"write"<<home_.filePath(QStringLiteral("written"))<<EACCES<<false;
  QTest::newRow("read-outside")<<"read"<<secret<<EACCES<<false;
  QTest::newRow("other-process-memory")<<"vm-read"<<parent<<-SIGSYS<<false;
  // Below Landlock ABI 6 only the seccomp filter stops this.
  QTest::newRow("signal-parent")<<"signal"<<parent<<EPERM<<false;
  // Metadata a write ban does not cover, and another process's limits.
  QTest::newRow("chmod-home")<<"chmod"<<secret<<EPERM<<false;
  QTest::newRow("parent-limit")<<"prlimit"<<parent<<EPERM<<false;
  // Installed at <home-like>/install/bin with install/lib a symlink to
  // <home-like>: the prefix and its lib are granted only as themselves, so a
  // file in the home-like directory must stay unreadable.
  QTest::newRow("read-beside-prefix")<<"read"<<secret<<EACCES<<true;
 }
 void forbidden(){
  QFETCH(QString,action);QFETCH(QString,argument);QFETCH(int,errorOrSignal);QFETCH(bool,fromHome);
  QString probe=qEnvironmentVariable("PUTGD_OCR_PROBE");
  if(fromHome){
   QDir().mkpath(home_.filePath(QStringLiteral("install/bin")));
   QFile::remove(home_.filePath(QStringLiteral("install/lib")));
   QVERIFY(QFile::link(home_.path(),home_.filePath(QStringLiteral("install/lib"))));
   const auto copy=home_.filePath(QStringLiteral("install/bin/putgd-ocr-probe"));
   QFile::remove(copy);QVERIFY(QFile::copy(probe,copy));
   QVERIFY(QFile::setPermissions(copy,QFileDevice::ReadOwner|QFileDevice::ExeOwner));
   probe=copy;
  }
  QStringList arguments;if(!dataDirectory().isEmpty())arguments<<QStringLiteral("--bands");
  arguments<<action;if(!argument.isEmpty())arguments<<argument;
  QProcess process;QProcessEnvironment environment;environment.insert("PUTGD_TEXT_DATA",dataDirectory());environment.insert("OMP_THREAD_LIMIT","1");
  process.setProcessEnvironment(environment);
  process.start(probe,arguments);
  QVERIFY(process.waitForFinished(60000));
  const auto output=process.readAllStandardOutput();
  if(output.startsWith("sanitized"))QSKIP("confinement is tested on unsanitized builds only");
  QVERIFY2(!output.contains("confine failed")&&!output.contains("recognized error=input")&&!output.contains("recognized error=lang"),output.constData());
  if(errorOrSignal<0){
   QVERIFY2(process.exitStatus()==QProcess::CrashExit,output.constData());QCOMPARE(process.exitCode(),-errorOrSignal);
  }else{
   QVERIFY2(process.exitStatus()==QProcess::NormalExit&&process.exitCode()==0,output.constData());
   QVERIFY2(output.contains(QByteArray("result=-1 errno=")+QByteArray::number(errorOrSignal)),output.constData());
  }
  QVERIFY(!QFileInfo::exists(home_.filePath(QStringLiteral("written"))));
 }
};
QTEST_GUILESS_MAIN(OcrConfinement)
#include "test_ocr_confinement.moc"
