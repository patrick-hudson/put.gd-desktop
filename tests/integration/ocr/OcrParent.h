#pragma once
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QProcess>
#include <QString>
#include <QStringList>

// A test parent for putgd-ocr: the parent side of the helper's protocol, as
// small as the tests and measurements need. The product's own parent is the
// core job class; this one exists so the helper can be driven and measured on
// its own.
namespace putgd::test {
struct OcrRequest {
 QString helper;
 QStringList dataDirectories;     // PUTGD_OCR_DATA, in resolution order
 QStringList languages{QStringLiteral("eng")};
 bool strictLanguages=true;
 bool words=false,candidates=false;
 int timeoutMs=20000;
 // Fault injection for the helper tests, all off by default.
 bool corruptBuild=false;         // send a build manifest that does not match
 bool unsealedInput=false;        // an input memfd without its seals
 bool readOnlyOutput=false;       // the output descriptor opened read-only
 int stopAfterReady=0;            // 1: kill the helper after ready; 2: never activate
};
struct OcrRun {
 QString error;                   // the child's code, or the parent's text_job_*
 QString language;                // the language a language_unavailable names
 QJsonObject result;              // the parsed output after a zero exit
 QByteArray output;
 qint64 inputMs=0;                // grey conversion and the memfd write
 qint64 spawnMs=0;                // start to ready (process start, dynamic linking, confinement, Qt)
 qint64 recognitionMs=0;          // activate to checkpoint
 qint64 copyMs=0;                 // reading the output back and parsing it
 qint64 totalMs=0;
 long peakRssKiB=0;               // the child's VmHWM at its checkpoint (all work done)
 int exitCode=-1;
 QProcess::ExitStatus exitStatus=QProcess::NormalExit;
 int signal=0;
 bool childGone=false;            // the exact child no longer exists after return
};
// Converts the image to 8-bit grey with alpha flattened onto white, as the
// product parent does, and runs one job.
OcrRun runOcr(const QImage& image,const OcrRequest& request);
// The same with a grey buffer already in the helper's layout.
OcrRun runOcrGrey(const QByteArray& grey,int width,int height,int stride,const OcrRequest& request);
QString defaultDataDirectory();
}
