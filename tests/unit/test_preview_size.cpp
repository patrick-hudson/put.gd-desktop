#include "ui/PreviewSize.h"
#include <QtTest>
using namespace putgd::Ui;

// The History expanded preview's size on the screens that left it blank or off-screen: an empty work area (-396,-161
// reached setMinimumSize), a strip (1920x48) and a small screen with a wide image.
class PreviewSizeContract : public QObject {
 Q_OBJECT
 static constexpr int margin = 32, overhead = 113, maxWidth = 900, maxHeight = 640;
 static PreviewPlan plan(const QSize& image, const QSize& work, const QSize& screen) {
  return planExpandedPreview(image, work, screen, margin, overhead, maxWidth, maxHeight);
 }
private slots:
 void aNormalWorkAreaIsUsed() {
  const QSize work(1920, 1040), screen(1920, 1080);
  QCOMPARE(previewArea(work, screen, margin, overhead), work);
  const auto result = plan(QSize(2560, 1440), work, screen);
  QCOMPARE(result.picture, QSize(900, 506));
  QCOMPARE(result.dialog, QSize(932, 619));
 }
 void anEmptyWorkAreaFallsBackToTheScreen() {
  const QSize screen(1920, 1080);
  QCOMPARE(previewArea(QSize(0, 0), screen, margin, overhead), screen);
  const auto result = plan(QSize(2560, 1440), QSize(0, 0), screen);
  QCOMPARE(result.picture, QSize(900, 506));
  QCOMPARE(result.dialog, QSize(932, 619));
 }
 void aWorkAreaStripFallsBackToTheScreenAndKeepsAPortraitPictureWide() {
  const QSize strip(1920, 48), screen(1920, 1080);
  QCOMPARE(previewArea(strip, screen, margin, overhead), screen);
  const auto result = plan(QSize(1200, 3000), strip, screen);
  QCOMPARE(result.picture, QSize(256, 640));
  QVERIFY(result.picture.width() >= kPreviewMinimumPicture.width());
 }
 void aSmallScreenKeepsTheDialogOnIt() {
  const QSize work(800, 560), screen(800, 600);
  const auto result = plan(QSize(4000, 1000), work, screen);
  QCOMPARE(result.picture, QSize(720, 180));
  QVERIFY(result.dialog.width() <= work.width() - kPreviewScreenSlack);
  QVERIFY(result.dialog.height() <= work.height() - kPreviewScreenSlack);
  // The same screen reporting only a strip of work area is sized from its geometry, not from a fixed 1000x700.
  const auto stripped = plan(QSize(4000, 1000), QSize(800, 48), screen);
  QCOMPARE(stripped.picture, QSize(720, 180));
  QVERIFY(stripped.dialog.width() <= screen.width() - kPreviewScreenSlack);
 }
 void anEmptyWorkAreaOnASmallScreenIsSizedFromThatScreenNotTheDefault() {
  const QSize screen(800, 600);
  QCOMPARE(previewArea(QSize(0, 0), screen, margin, overhead), screen);
  const auto result = plan(QSize(4000, 1000), QSize(0, 0), screen);
  QCOMPARE(result.picture, QSize(720, 180));
  QVERIFY(result.dialog.width() <= screen.width() - kPreviewScreenSlack);  // the fixed 1000x700 asked for 932 here
 }
 void aScreenThatIsUnknownGetsTheFixedDefault() {
  QCOMPARE(previewArea(QSize(), QSize(), margin, overhead), kPreviewFallbackArea);
  const auto result = plan(QSize(2560, 1440), QSize(), QSize());
  QCOMPARE(result.picture, QSize(900, 506));
 }
 void aVeryThinImageStillHasAPicture() {
  const auto result = plan(QSize(10000, 10), QSize(1920, 1040), QSize(1920, 1080));
  QVERIFY(result.picture.width() >= 1 && result.picture.height() >= 1);
 }
};
QTEST_APPLESS_MAIN(PreviewSizeContract)
#include "test_preview_size.moc"
