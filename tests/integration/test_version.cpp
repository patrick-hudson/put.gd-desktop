#include "core/BuildInfo.h"
#include "fixtures/U2Fixtures.h"
#include <QFile>
#include <QProcess>
#include <QProcessEnvironment>
using namespace putgd;
using namespace putgd::test;
class VersionContract final : public QObject {
 Q_OBJECT
private slots:
 void binariesAndDiagnosticsMatchManifest() {
  QFile file(PUTGD_MANIFEST_PATH);QVERIFY(file.open(QIODevice::ReadOnly));
  const auto manifest=QJsonDocument::fromJson(file.readAll()).object();
  QCOMPARE(QJsonDocument::fromJson(buildManifestJson()).object(),manifest);
  QCOMPARE(manifest["version"].toString(),QString::fromLatin1(buildVersion()));
  auto environment=QProcessEnvironment::systemEnvironment();
  environment.remove("DISPLAY");environment.remove("WAYLAND_DISPLAY");
  environment.insert("QT_QPA_PLATFORM","this-platform-does-not-exist");
  for(const auto& program:QList<QPair<QString,QString>>{{PUTGD_APP_PATH,"putgd"},{PUTGD_CLI_PATH,"putgd-cli"},{PUTGD_UI_PATH,"putgd-ui"}}) {
   QProcess process;process.setProcessEnvironment(environment);process.start(program.first,{"--version"});
   QVERIFY(process.waitForFinished(5000));QCOMPARE(process.exitCode(),0);
   QCOMPARE(process.readAllStandardOutput().trimmed(),(program.second+" "+buildVersion()).toUtf8());
   QVERIFY(process.readAllStandardError().isEmpty());
  }
  QTemporaryDir directory;QVERIFY(directory.isValid());ProviderRegistry providers;
  SettingsService settings(directory.filePath("config.json"));Source source;
  OperationService operations(directory.filePath("data"),settings,providers,source);
  const auto diagnostics=operations.diagnostics();QCOMPARE(diagnostics["schemaVersion"].toInt(),1);
  QCOMPARE(diagnostics["build"].toObject(),manifest);QCOMPARE(source.reads,0);QCOMPARE(source.copies,0);
 }
};
QTEST_GUILESS_MAIN(VersionContract)
#include "test_version.moc"
