// One smoke test of the real putgd-ocr (#1428), driven by the contract's
// TextJob as the resident and the editor drive it: confined, it reads large
// clear text and one QR code; and its handling of models that carry the
// legacy engine's data. Recognition quality is scored on the corpus,
// never here: Tesseract differs by distribution. TextJob's refusals (wrong
// build, a result over the cap) are tested against the contract's fake helper.
#include "core/TextJob.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QGuiApplication>
#include <QPainter>
#include <QTemporaryDir>
#include <QTest>
#include <algorithm>
#include <cstring>
#include <vector>

namespace {
QStringList systemData(){
 QStringList data;
 for(const auto& d:{QStringLiteral("/usr/share/tesseract-ocr/5/tessdata"),QStringLiteral("/usr/share/tesseract/tessdata"),QStringLiteral("/usr/share/tessdata")})
  if(QFileInfo::exists(d+QStringLiteral("/eng.traineddata")))data.append(d);
 return data;
}
QImage helloWorld(){
 QImage image(1000,420,QImage::Format_RGB32);image.fill(Qt::white);
 QPainter painter(&image);QFont font(QStringLiteral("DejaVu Sans"));font.setPixelSize(40);painter.setFont(font);painter.setPen(Qt::black);
 painter.drawText(40,80,QStringLiteral("HELLO WORLD"));
 return image;
}
// A traineddata container with the given entries (index, bytes), in index order.
QByteArray container(const QList<QPair<int,QByteArray>>& entries){
 const uint32_t count=24;std::vector<int64_t> table(count,-1);
 qint64 at=4+8*count;QByteArray body;
 for(const auto& [index,bytes]:entries){table[size_t(index)]=at+body.size();body+=bytes;}
 QByteArray out(4+8*count,'\0');std::memcpy(out.data(),&count,4);std::memcpy(out.data()+4,table.data(),8*count);
 return out+body;
}
// The entries of a container, as container() takes them.
QList<QPair<int,QByteArray>> entries(const QByteArray& data){
 uint32_t count=0;std::memcpy(&count,data.constData(),4);
 std::vector<int64_t> table(count);std::memcpy(table.data(),data.constData()+4,8*count);
 QList<QPair<int,QByteArray>> out;
 for(uint32_t i=0;i<count;++i){
  if(table[i]<0)continue;
  qint64 end=data.size();for(uint32_t j=i+1;j<count;++j)if(table[j]>=0){end=table[j];break;}
  out.append({int(i),data.mid(table[i],end-table[i])});
 }
 return out;
}
}

class OcrHelper final:public QObject {
 Q_OBJECT
private slots:
 void readsClearTextAndACode(){
  const auto helper=qEnvironmentVariable("PUTGD_OCR_HELPER");
  QVERIFY(QFileInfo(helper).isExecutable());
  putgd::TextJob job(helper);
  const auto data=systemData();
  if(data.isEmpty())QSKIP("English language data is not installed");
  job.setDataDirectories(data);
  auto image=helloWorld();
  {QPainter painter(&image);painter.drawImage(QRect(40,140,300,300),QImage(QStringLiteral(PUTGD_SOURCE_DIR "/tests/fixtures/ocr/qr-url.png")));}
  putgd::TextJob::Options options;options.languages={QStringLiteral("eng")};options.languagesRequired=true;
  putgd::Error error;
  const auto result=job.recognize(putgd::TextJob::grey(image),options,error);
  QVERIFY2(!error,qPrintable(error.code));
  QVERIFY2(result.text.contains(QLatin1String("WORLD")),qPrintable(result.text));
  // ZXing's format names differ between 2.x and 3.x; a mismatch would drop every code.
  QCOMPARE(result.codes.size(),1);
  QCOMPARE(result.codes.first().format,QStringLiteral("QRCode"));
  QCOMPARE(result.codes.first().text,QStringLiteral("https://example.test/ocr?id=1428"));
 }
 // A model that also carries the legacy engine (Arch's English) is handed to
 // Tesseract as its LSTM entries alone, with the offset table rewritten: a
 // wrong table would load the wrong entries and read nothing, or garbage. A
 // model with no LSTM entries is refused rather than run on the legacy engine.
 void legacyEntriesAreDroppedAndLegacyOnlyRefused_data(){
  QTest::addColumn<bool>("lstm");
  QTest::newRow("legacy-and-lstm")<<true;
  QTest::newRow("legacy-only")<<false;
 }
 void legacyEntriesAreDroppedAndLegacyOnlyRefused(){
  QFETCH(bool,lstm);
  const auto data=systemData();
  if(data.isEmpty())QSKIP("English language data is not installed");
  QFile english(data.first()+QStringLiteral("/eng.traineddata"));QVERIFY(english.open(QIODevice::ReadOnly));
  // The legacy entries, between and around the LSTM ones: unicharset,
  // templates, prototype tables, legacy dictionaries and the shape table.
  QList<QPair<int,QByteArray>> model;
  for(const int legacy:{1,3,4,5,6,7,8,9,13,14,15})model.append({legacy,QByteArray(65536+legacy,char(legacy))});
  if(lstm)for(const auto& entry:entries(english.readAll()))if(entry.first>=17)model.append(entry);
  std::sort(model.begin(),model.end(),[](const auto& a,const auto& b){return a.first<b.first;});
  QTemporaryDir directory;QVERIFY(directory.isValid());
  QFile file(directory.filePath(QStringLiteral("eng.traineddata")));
  QVERIFY(file.open(QIODevice::WriteOnly));file.write(container(model));file.close();
  putgd::TextJob job(qEnvironmentVariable("PUTGD_OCR_HELPER"));job.setDataDirectories({directory.path()});
  putgd::TextJob::Options options;options.languages={QStringLiteral("eng")};options.languagesRequired=true;
  putgd::Error error;
  const auto result=job.recognize(putgd::TextJob::grey(helloWorld()),options,error);
  if(lstm){
   QVERIFY2(!error,qPrintable(error.code));
   QVERIFY2(result.text.contains(QLatin1String("WORLD")),qPrintable(result.text));
  }else{
   QCOMPARE(error.code,QStringLiteral("language_unavailable"));
  }
 }
};
QTEST_MAIN(OcrHelper)
#include "test_ocr_helper.moc"
