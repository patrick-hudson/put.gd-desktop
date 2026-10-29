// Production widget input/painting without a Wayland compositor or layer role.
// This does not establish native KWin focus, dismissal or capture correctness.
#include "platform/kde/KdeRegionOverlay.h"
#include "ui/RegionSelection.h"
#include <QtTest>
#include <QApplication>
#include <QDir>
#include <QScreen>
namespace putgd {
class KdeRegionOverlayTest final:public QObject {
 Q_OBJECT
 QWidget* add(KdeRegionOverlay& overlay,const QRectF& logical){
  CaptureFrame frame{QString("Display %1").arg(overlay.frames_.size()+1),logical,logical.toAlignedRect(),1,0,{}};
  overlay.frames_.append(frame);overlay.addSurface(frame,QGuiApplication::primaryScreen());
  for(auto* w:QApplication::topLevelWidgets())if(w->isVisible()&&w->objectName()=="putgd-kde-region-overlay"&&w->size()==logical.size().toSize())return w;
  return nullptr;
 }
private slots:
 void resolvedOutcomeIsVisibleAndAccessible(){
  for(const QString outcome:{QString("Uploads right away to Team Drive · Shared link"),QString("Saves locally"),QString("Opens the image editor")}){
   KdeRegionOverlay overlay({},nullptr,false,outcome);auto* surface=add(overlay,QRectF(0,0,800,600));QVERIFY(surface);
   QVERIFY(surface->accessibleDescription().endsWith(outcome));
   QTest::keyClick(surface,Qt::Key_Right);QVERIFY(surface->accessibleDescription().endsWith(outcome));
   QTest::mousePress(surface,Qt::LeftButton,{},QPoint(30,40));QTest::mouseMove(surface,QPoint(200,160));
   QVERIFY(surface->accessibleDescription().contains("Release to capture"));QVERIFY(surface->accessibleDescription().endsWith(outcome));
   QVERIFY(!surface->grab().isNull());overlay.cancel();
  }
 }
 void keyboardStepsAndAccessibleGeometry(){
  KdeRegionOverlay overlay({});const QRectF screen(-800,100,800,600);auto* surface=add(overlay,screen);QVERIFY(surface);
  QSignalSpy accepted(&overlay,&KdeRegionOverlay::selected);QSignalSpy cancelled(&overlay,&KdeRegionOverlay::cancelled);
  auto* accessible=QAccessible::queryAccessibleInterface(surface);QVERIFY(accessible);QCOMPARE(accessible->text(QAccessible::Name),QString("Capture region"));
  QVERIFY(accessible->text(QAccessible::Description).contains("Drag to select"));
  QVERIFY(surface->accessibleDescription().contains("Enter to capture"));
  QTest::keyClick(surface,Qt::Key_Right);QCOMPARE(overlay.selection_,QRectF(-600,250,400,300));
  QVERIFY(surface->accessibleDescription().contains("Enter to capture"));QVERIFY(surface->accessibleDescription().contains("400 × 300"));
  QTest::keyClick(surface,Qt::Key_Right);QTest::keyClick(surface,Qt::Key_Down,Qt::ControlModifier);
  QTest::keyClick(surface,Qt::Key_Right,Qt::ShiftModifier);QTest::keyClick(surface,Qt::Key_Up,Qt::ControlModifier|Qt::ShiftModifier);
  const QRectF expected(-599,260,401,290);QCOMPARE(overlay.selection_,expected);
  QCOMPARE(accessible->text(QAccessible::Description),regionDescription(expected,false,"Display 1",overlay.frames_.first().logicalRect,overlay.frames_));
  QTest::keyClick(surface,Qt::Key_Enter);QCOMPARE(accepted.size(),1);QCOMPARE(qvariant_cast<QRectF>(accepted.first().first()),expected);QCOMPARE(cancelled.size(),0);QVERIFY(!surface->isVisible());
  QTest::keyClick(surface,Qt::Key_Return);QCOMPARE(accepted.size(),1);
 }
 void recordingHintsReflectSelection(){
  KdeRegionOverlay overlay({},nullptr,true);auto* surface=add(overlay,QRectF(0,0,640,480));QVERIFY(surface);
  QCOMPARE(surface->accessibleName(),QString("Recording region"));QVERIFY(surface->accessibleDescription().contains("Enter to choose"));
  QVERIFY(surface->accessibleDescription().contains("Enter to choose"));
  QTest::mousePress(surface,Qt::LeftButton,{},QPoint(40,40));QTest::mouseMove(surface,QPoint(200,200));QVERIFY(surface->accessibleDescription().contains("Release to choose recording area"));
  QTest::keyClick(surface,Qt::Key_Right);QVERIFY(surface->accessibleDescription().contains("Enter to choose"));QVERIFY(surface->accessibleDescription().contains("160 × 160"));
  QTest::keyClick(surface,Qt::Key_Escape);
 }
 void enterUsesReceivingScreenAndEscapeCancels(){
  for(bool cancel:{false,true}){
   KdeRegionOverlay overlay({});auto* left=add(overlay,QRectF(-800,0,800,600));auto* right=add(overlay,QRectF(0,100,640,480));QVERIFY(left);QVERIFY(right);
   QSignalSpy accepted(&overlay,&KdeRegionOverlay::selected);QSignalSpy cancelled(&overlay,&KdeRegionOverlay::cancelled);
   QTest::keyClick(right,cancel?Qt::Key_Escape:Qt::Key_Return);QCOMPARE(accepted.size(),cancel?0:1);QCOMPARE(cancelled.size(),cancel?1:0);
   if(!cancel)QCOMPARE(qvariant_cast<QRectF>(accepted.first().first()),QRectF(0,100,640,480));
   QVERIFY(!left->isVisible());QVERIFY(!right->isVisible());
  }
 }
 void keyboardSwitchesScreenWithoutInvalidGeometry(){
  KdeRegionOverlay overlay({});auto* left=add(overlay,QRectF(-800,0,800,600));auto* right=add(overlay,QRectF(0,100,640,480));QVERIFY(left);QVERIFY(right);
  QSignalSpy accepted(&overlay,&KdeRegionOverlay::selected);QTest::keyClick(left,Qt::Key_Right);QCOMPARE(overlay.selection_,QRectF(-600,150,400,300));
  QTest::keyClick(left,Qt::Key_Tab);QCOMPARE(overlay.selection_,QRectF(200,250,400,300));
  QVERIFY(left->accessibleDescription().contains("Display 2"));
  QTest::keyClick(right,Qt::Key_Backtab);QCOMPARE(overlay.selection_,QRectF(-600,150,400,300));
  QTest::keyClick(right,Qt::Key_Tab);QTest::keyClick(right,Qt::Key_Left,Qt::ShiftModifier);QCOMPARE(overlay.selection_,QRectF(200,250,399,300));
  QCOMPARE(left->accessibleDescription(),right->accessibleDescription());QTest::keyClick(right,Qt::Key_Return);QCOMPARE(accepted.size(),1);
 }
 void keyboardBoundsAndMouseTakeover(){
  KdeRegionOverlay overlay({});auto* surface=add(overlay,QRectF(100,-200,640,480));QVERIFY(surface);QSignalSpy accepted(&overlay,&KdeRegionOverlay::selected);
  QTest::keyClick(surface,Qt::Key_Left);for(int i=0;i<100;++i)QTest::keyClick(surface,Qt::Key_Left,Qt::ControlModifier);
  QCOMPARE(overlay.selection_.left(),100.0);for(int i=0;i<100;++i)QTest::keyClick(surface,Qt::Key_Up,Qt::ShiftModifier|Qt::ControlModifier);
  QCOMPARE(overlay.selection_.height(),1.0);
  QTest::mousePress(surface,Qt::LeftButton,{},QPoint(80,100));QTest::mouseMove(surface,QPoint(300,320));QVERIFY(surface->accessibleDescription().contains("Release to capture"));QTest::mouseRelease(surface,Qt::LeftButton,{},QPoint(300,320));
  QCOMPARE(accepted.size(),1);QCOMPARE(qvariant_cast<QRectF>(accepted.first().first()),QRectF(180,-100,220,220));
 }
 void keyboardStopsDragAndEscapeStillWorks(){
  KdeRegionOverlay overlay({});auto* surface=add(overlay,QRectF(0,0,640,480));QVERIFY(surface);QSignalSpy accepted(&overlay,&KdeRegionOverlay::selected);QSignalSpy cancelled(&overlay,&KdeRegionOverlay::cancelled);
  QTest::mousePress(surface,Qt::LeftButton,{},QPoint(100,100));QTest::mouseMove(surface,QPoint(300,300));QTest::keyClick(surface,Qt::Key_Right);
  const auto selected=overlay.selection_;QTest::mouseRelease(surface,Qt::LeftButton,{},QPoint(400,400));QCOMPARE(accepted.size(),0);QCOMPARE(overlay.selection_,selected);
  QTest::keyClick(surface,Qt::Key_Escape);QCOMPARE(cancelled.size(),1);QVERIFY(!surface->isVisible());
 }
 void mouseCancellationAndAgentSelection(){
  for(int mode=0;mode<3;++mode){KdeRegionOverlay overlay({});auto* surface=add(overlay,QRectF(0,0,640,480));QVERIFY(surface);QSignalSpy accepted(&overlay,&KdeRegionOverlay::selected);QSignalSpy cancelled(&overlay,&KdeRegionOverlay::cancelled);
   if(mode==0)QTest::mouseClick(surface,Qt::RightButton,{},QPoint(80,100));
   else if(mode==1)QTest::mouseClick(surface,Qt::LeftButton,{},QPoint(80,100));
   else overlay.select(QRectF(20,30,100,120));
   QCOMPARE(accepted.size(),mode==2?1:0);QCOMPARE(cancelled.size(),mode==2?0:1);QVERIFY(!surface->isVisible());
  }
 }
 void hintAndSizeShowWithoutScrimAndClearBeforeDismissal(){
  // #629: no scrim; a one-line hint pill and a size label next to the selection.
  for(const auto& variant:{QString("dark"),QString("light"),QString("text150")}){
   KdeRegionOverlay overlay({});auto* surface=add(overlay,QRectF(0,0,800,600));QVERIFY(surface);
   const bool light=variant=="light";Theme::setScheme(light?Theme::Scheme::Light:Theme::Scheme::Dark);
   QFont font=surface->font();font.setPixelSize(variant=="text150"?21:14);surface->setFont(font);QTest::keyClick(surface,Qt::Key_Right);
   QCOMPARE(overlay.selection_,QRectF(200,150,400,300));QVERIFY(!overlay.clearing_);QVERIFY(!overlay.finished_);
   surface->repaint();const auto image=surface->grab().toImage();QCOMPARE(image.pixelColor(300,151),QColor(Qt::white));
   QCOMPARE(image.pixelColor(5,500).alpha(),0);QCOMPARE(image.pixelColor(790,10).alpha(),0);QCOMPARE(image.pixelColor(400,300).alpha(),0);
   const QFontMetrics metrics(font);const auto bg=Theme::current().surfaceRaised;
   const auto hint=regionHintBox(surface->rect(),metrics,regionHint(),overlay.selection_);QVERIFY(!hint.isEmpty());QVERIFY(hint.height()<surface->height()/8);
   QCOMPARE(image.pixelColor(hint.left()+hint.height(),hint.top()+2),bg);
   const auto size=regionSizeBox(surface->rect(),metrics,regionSizeLabel(overlay.selection_,overlay.frames_),overlay.selection_);
   QVERIFY(!size.isEmpty());QVERIFY(!QRectF(size).intersects(overlay.selection_));QCOMPARE(image.pixelColor(size.left()+size.height(),size.top()+2),bg);
   int ink=0;for(int y=hint.top()+3;y<hint.bottom()-3;++y)for(int x=hint.left()+8;x<hint.right()-8;++x){const auto c=image.pixelColor(x,y);if(c.alpha()==255&&std::abs(c.lightness()-bg.lightness())>100)++ink;}
   QVERIFY(ink>50);
   const auto dir=qEnvironmentVariable("PUTGD_REGION_RENDER_DIR");if(!dir.isEmpty()){QVERIFY(QDir().mkpath(dir+"/"+variant));QImage preview(image.size(),QImage::Format_RGB32);preview.fill(Qt::darkCyan);QPainter painter(&preview);painter.drawImage(0,0,image);painter.end();QVERIFY(preview.save(dir+"/"+variant+"/kde-region.png"));}
   // After the fade only the size label remains.
   overlay.hint_.expire();surface->repaint();const auto faded=surface->grab().toImage();QCOMPARE(faded.pixelColor(hint.left()+hint.height(),hint.top()+2).alpha(),0);QCOMPARE(faded.pixelColor(size.left()+size.height(),size.top()+2),bg);
   overlay.clearing_=true;const auto cleared=surface->grab().toImage();for(int y=0;y<cleared.height();++y)for(int x=0;x<cleared.width();++x)QCOMPARE(cleared.pixelColor(x,y).alpha(),0);
  }
 }
 void screenshotFirstDrawsTheFrozenFrame(){
  // #629: the frozen pixels are the backdrop; the selection only adds an outline.
  QImage pixels(1600,1200,QImage::Format_RGB32);pixels.fill(QColor(Qt::magenta));
  KdeRegionOverlay overlay({},nullptr,false,{},true);
  CaptureFrame frame{"Display 1",QRectF(0,0,800,600),QRect(0,0,1600,1200),2,0,pixels};overlay.frames_.append(frame);overlay.addSurface(frame,QGuiApplication::primaryScreen());
  QWidget* surface=nullptr;for(auto* w:QApplication::topLevelWidgets())if(w->isVisible()&&w->objectName()=="putgd-kde-region-overlay")surface=w;QVERIFY(surface);
  overlay.hint_.expire();QTest::keyClick(surface,Qt::Key_Right);surface->repaint();const auto image=surface->grab().toImage();
  QCOMPARE(image.pixelColor(5,500),QColor(Qt::magenta));QCOMPARE(image.pixelColor(400,300),QColor(Qt::magenta));QCOMPARE(image.pixelColor(300,151),QColor(Qt::white));
  // The size label reports the frozen frame's output pixels.
  QCOMPARE(regionSizeLabel(overlay.selection_,overlay.frames_),QString("800 × 600"));
  overlay.cancel();
 }
};
}
QTEST_MAIN(putgd::KdeRegionOverlayTest)
#include "test_kde_region_overlay.moc"
