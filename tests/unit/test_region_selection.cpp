#include "ui/RegionSelection.h"
#include <QtTest>
using namespace putgd;
class RegionSelectionTest final:public QObject {
 Q_OBJECT
private slots:
 void badgeAvoidsSelectionAndDragPointer(){
  const QRect bounds(0,0,800,600);const QFontMetrics metrics(QFont{});const auto text=regionInstructions({},true);
  const auto top=regionInstructionBox(bounds,metrics,text);QVERIFY(top.top()<100);
  const QRectF selected(200,0,400,220);
  const auto bottom=regionInstructionBox(bounds,metrics,text,selected);QVERIFY(!QRectF(bottom).intersects(selected));QVERIFY(bottom.top()>300);
  QCOMPARE(regionInstructionBox(bounds,metrics,text,{},QPointF(top.center())),bottom);
  QCOMPARE(regionInstructionBox(bounds,metrics,text,{},QPointF(bottom.center())),top);
 }
 void switchesDisplaysAndClamps(){
  const QList<QRectF> screens{QRectF(-800,100,800,600),QRectF(0,-200,320,240)};int current=0;QRectF selected;
  QVERIFY(switchRegionDisplay(selected,screens,current,Qt::Key_Tab));QCOMPARE(current,1);QVERIFY(selected.isEmpty());
  QVERIFY(switchRegionDisplay(selected,screens,current,Qt::Key_Tab,Qt::ShiftModifier));QCOMPARE(current,0);
  selected=QRectF(-600,250,400,300);QVERIFY(switchRegionDisplay(selected,screens,current,Qt::Key_Tab));QCOMPARE(selected,screens[1]);
  QVERIFY(switchRegionDisplay(selected,screens,current,Qt::Key_Backtab));QCOMPARE(selected,QRectF(-800,100,320,240));
  QVERIFY(!switchRegionDisplay(selected,screens,current,Qt::Key_Right));
  QVERIFY(!switchRegionDisplay(selected,{},current,Qt::Key_Tab));
 }
 void hintsReflectActionAndState(){
  for(bool recording:{false,true}){
   QRectF selected;const QRectF screen(0,0,800,600);
   const auto initial=regionInstructions(selected,recording,"DP-2");
   QVERIFY(initial.contains(recording?"Enter chooses this entire display":"Enter captures this entire display"));
   QVERIFY(initial.contains("Drag to select · "));
   QVERIFY(initial.startsWith("DP-2\n"));QVERIFY(!initial.contains("logical pixels"));
   QCOMPARE(regionKey(selected,screen,Qt::Key_Right),RegionKeyAction::Change);
   const auto changed=regionInstructions(selected,recording,"DP-2");
   QVERIFY(changed.contains(recording?"Enter chooses this region":"Enter to capture"));
   QVERIFY(!changed.contains("entire display"));QVERIFY(changed.contains("400 × 300"));
  }
 }
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
QTEST_MAIN(RegionSelectionTest)
#include "test_region_selection.moc"
