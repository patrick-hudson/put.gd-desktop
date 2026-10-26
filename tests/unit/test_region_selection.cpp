#include "ui/RegionSelection.h"
#include <QtTest>
using namespace putgd;
class RegionSelectionTest final:public QObject {
 Q_OBJECT
private slots:
 void initialRegionAndSteps(){
  const QRectF screen(-800,100,800,600);QRectF selected;
  QCOMPARE(regionKey(selected,screen,Qt::Key_Right),RegionKeyAction::Change);QCOMPARE(selected,QRectF(-600,250,400,300));
  QVERIFY(adjustRegion(selected,screen,Qt::Key_Left));QCOMPARE(selected.x(),-601.0);
  QVERIFY(adjustRegion(selected,screen,Qt::Key_Down,Qt::ControlModifier));QCOMPARE(selected.y(),260.0);
  QVERIFY(adjustRegion(selected,screen,Qt::Key_Right,Qt::ShiftModifier));QCOMPARE(selected.width(),401.0);
  QVERIFY(adjustRegion(selected,screen,Qt::Key_Up,Qt::ControlModifier|Qt::ShiftModifier));QCOMPARE(selected.height(),290.0);
  const auto before=selected;QVERIFY(!adjustRegion(selected,screen,Qt::Key_A));QCOMPARE(selected,before);
  const QKeyEvent event(QEvent::KeyPress,Qt::Key_Left,Qt::ControlModifier);QVERIFY(adjustRegion(selected,screen,event));QCOMPARE(selected.x(),before.x()-10);
 }
 void boundsAndMinimumSize(){
  const QRectF screen(20,-100,80,60);QRectF selected(20,-100,80,60);
  QVERIFY(adjustRegion(selected,screen,Qt::Key_Right,Qt::ControlModifier));QCOMPARE(selected,screen);
  for(int i=0;i<10;++i){adjustRegion(selected,screen,Qt::Key_Left,Qt::ControlModifier|Qt::ShiftModifier);adjustRegion(selected,screen,Qt::Key_Up,Qt::ControlModifier|Qt::ShiftModifier);}
  QCOMPARE(selected.size(),QSizeF(1,1));
  for(int i=0;i<20;++i){adjustRegion(selected,screen,Qt::Key_Right,Qt::ControlModifier);adjustRegion(selected,screen,Qt::Key_Down,Qt::ControlModifier);}
  QCOMPARE(selected,QRectF(99,-41,1,1));
  adjustRegion(selected,screen,Qt::Key_Right,Qt::ShiftModifier);QCOMPARE(selected,QRectF(99,-41,1,1));
 }
 void anotherScreenOrSubpixelIntersectionReinitializes(){
  const QRectF screen(-800,100,800,600);QRectF selected(100,200,20,20);
  QVERIFY(adjustRegion(selected,screen,Qt::Key_Up,Qt::ShiftModifier));QCOMPARE(selected,QRectF(-600,250,400,300));
  selected=QRectF(-.5,200,10,20);QVERIFY(adjustRegion(selected,screen,Qt::Key_Right,Qt::ShiftModifier));QCOMPARE(selected,QRectF(-600,250,400,300));
  selected={};QVERIFY(!adjustRegion(selected,{},Qt::Key_Left));QVERIFY(selected.isEmpty());
  QVERIFY(adjustRegion(selected,QRectF(0,0,1,1),Qt::Key_Left));QCOMPARE(selected,QRectF(0,0,1,1));
 }
 void acceptCancelAndIgnore(){
  const QRectF screen(100,20,640,480);QRectF selected;
  QCOMPARE(regionKey(selected,screen,Qt::Key_A),RegionKeyAction::Ignore);QVERIFY(selected.isEmpty());
  QCOMPARE(regionKey(selected,screen,Qt::Key_Escape),RegionKeyAction::Cancel);QVERIFY(selected.isEmpty());
  QCOMPARE(regionKey(selected,screen,Qt::Key_Return),RegionKeyAction::Accept);QCOMPARE(selected,screen);
  selected=QRectF(120,40,50,60);const auto before=selected;
  QCOMPARE(regionKey(selected,screen,Qt::Key_Enter),RegionKeyAction::Accept);QCOMPARE(selected,before);
 }
};
QTEST_GUILESS_MAIN(RegionSelectionTest)
#include "test_region_selection.moc"
