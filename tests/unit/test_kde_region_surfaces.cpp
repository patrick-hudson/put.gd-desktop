#include "platform/kde/KdeRegionOverlay.h"
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QTest>
#include <QWidget>
#include <QWindow>

class KdeRegionSurfacesTest : public QObject {
 Q_OBJECT
private slots:
 void eachSurfaceKeepsItsOutputOriginBeforeNativeRoleCreation() {
  const auto screens=QGuiApplication::screens();
  QCOMPARE(screens.size(),3);
  QList<putgd::CaptureFrame> frames;
  QList<QRect> remaining;
  for(auto* screen:screens) {
   putgd::CaptureFrame frame;frame.identity=screen->name();frame.logicalRect=screen->geometry();
   frames.append(frame);
   remaining.append(screen->geometry());
  }
  putgd::KdeRegionOverlay overlay(frames);
  overlay.start();
  QCoreApplication::processEvents();
  int found=0;
  for(auto* widget:QApplication::topLevelWidgets()) {
   if(widget->objectName()!="putgd-kde-region-overlay")continue;
   ++found;
   auto* screen=widget->screen();QVERIFY(screen);
   QCOMPARE(widget->geometry(),screen->geometry());
   QVERIFY(remaining.removeOne(widget->geometry()));
   QVERIFY(widget->windowHandle());
   QCOMPARE(widget->windowHandle()->screen(),screen);
  }
  QCOMPARE(found,3);
  QVERIFY(remaining.isEmpty());
 }
};
QTEST_MAIN(KdeRegionSurfacesTest)
#include "test_kde_region_surfaces.moc"
