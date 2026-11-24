#pragma once
// The fake putgd-ocr helper for tests (#1428), until and beside the real one.
// test_text_job_fixture is a real child process that speaks the real text-job
// protocol (src/ocr/TextJobChild) around a canned engine; a test points TextJob
// or HelperTextBackend at it (PUTGD_TEXT_JOB_FIXTURE, set by CMake) and at this
// private data directory, where the helper finds its plan.
//
//   FakeTextHelper helper;
//   helper.plan({{"mode","result"},{"result",QJsonObject{{"text","hello"},{"lineTops",QJsonArray{4}}}}});
//   TextJob job(PUTGD_TEXT_JOB_FIXTURE,2000);job.setDataDirectories(helper.dataDirectories());
//
// Plan modes:
//   describe (default)  text that reports the image, options, inherited
//                       descriptors, environment names and whether the input is sealed
//   result              the plan's "result", in the helper output shape, as written
//   child-error         "code" and optionally "language"
//   stall               never answers, until the parent's deadline or a cancel
//   wrong-build         a ready frame from another build
//   oversize            a result one byte above the output cap
// "delayMs" delays describe and result. The helper writes its PID to fixture.pid.
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>

namespace putgd::test {
struct FakeTextHelper {
 QTemporaryDir directory{QStringLiteral("/tmp/tj-XXXXXX")};
 bool plan(const QJsonObject& plan) const {
  QFile file(directory.filePath("fixture.json"));
  return file.open(QIODevice::WriteOnly|QIODevice::Truncate)&&file.write(QJsonDocument(plan).toJson(QJsonDocument::Compact))>0;
 }
 // An installed language, as textjob::installedLanguages sees one: a data file by name.
 bool addLanguage(const QString& identifier) const {
  QFile file(directory.filePath(identifier+".traineddata"));
  return file.open(QIODevice::WriteOnly)&&file.write("fixture")>0;
 }
 QStringList dataDirectories() const {return {directory.path()};}
 // The helper's process ID once it has started, else 0.
 qint64 pid() const {QFile file(directory.filePath("fixture.pid"));return file.open(QIODevice::ReadOnly)?file.readAll().toLongLong():0;}
};
}
