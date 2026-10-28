#include "ui/RegionSelection.h"
#include <QtTest>
#include <QScopeGuard>
using namespace putgd;
class RegionSelectionTest final:public QObject {
 Q_OBJECT
private slots:
 void instructionCardUsesThemeAndScaledInsets(){
  const auto original=qApp->font();auto restore=qScopeGuard([&]{qApp->setFont(original);});
  for(int pixels:{14,21})for(auto scheme:{Theme::Scheme::Dark,Theme::Scheme::Light}){
   auto font=original;font.setPixelSize(pixels);qApp->setFont(font);Theme::setScheme(scheme);
   const QRect bounds(0,0,800,600);const QString text="Capture region\nDrag to choose";const auto box=regionInstructionBox(bounds,QFontMetrics(font),text,{}, {},font);
   QCOMPARE(box.top(),Graphite::space(Graphite::Space::l));
   QImage image(bounds.size(),QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);QPainter painter(&image);painter.setFont(font);
   QPalette unrelated;unrelated.setColor(QPalette::Window,Qt::magenta);unrelated.setColor(QPalette::WindowText,Qt::green);paintRegionInstructions(painter,bounds,unrelated,text);painter.end();
   QCOMPARE(image.pixelColor(box.center().x(),box.top()+2),Theme::current().surfaceRaised);
   QVERIFY(Theme::contrast(Theme::current().text,Theme::current().surfaceRaised)>=4.5);
   QVERIFY(Theme::contrast(Theme::current().textSecondary,Theme::current().surfaceRaised)>=4.5);
  }
 }
 void badgeAvoidsSelectionAndDragPointer(){
  const QRect bounds(0,0,800,600);const QFontMetrics metrics(QFont{});const auto text=regionInstructions({},true);
  const auto top=regionInstructionBox(bounds,metrics,text);QVERIFY(top.top()<100);
  const QRectF selected(200,0,400,220);
  const auto bottom=regionInstructionBox(bounds,metrics,text,selected);QVERIFY(!QRectF(bottom).intersects(selected));QVERIFY(bottom.top()>300);
  QCOMPARE(regionInstructionBox(bounds,metrics,text,{},QPointF(top.center())),bottom);
  QCOMPARE(regionInstructionBox(bounds,metrics,text,{},QPointF(bottom.center())),top);
 }
 void scrimLeavesSelectionClear(){
  QImage image(100,80,QImage::Format_ARGB32_Premultiplied);image.fill(Qt::transparent);
  QPainter painter(&image);paintRegionScrim(painter,image.rect(),QRectF(20,15,60,50));painter.end();
  QCOMPARE(image.pixelColor(5,5),QColor(0,0,0,70));
  QCOMPARE(image.pixelColor(25,20).alpha(),0);
  QCOMPARE(image.pixelColor(85,70),QColor(0,0,0,70));
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
 void screenshotOutcomeSurvivesKeyboardAndMouseSelection(){
  const QRectF bounds(0,0,800,600);const QList<CaptureFrame> frames{{"Display 1",bounds,bounds.toRect(),1,0,{}}};
  for(const QString outcome:{QString("Uploads immediately to Team Drive · Shared link"),QString("Saves locally"),QString("Opens the image editor")}){
   for(bool dragging:{false,true})for(const QRectF selection:{QRectF(),QRectF(20,30,400,200)}){
    const auto text=regionInstructions(selection,false,"Display 1",bounds,frames,dragging,outcome);
    QVERIFY(text.endsWith("\n"+outcome));QCOMPARE(text.count(outcome),1);
    QVERIFY(regionDescription(selection,false,"Display 1",bounds,frames,dragging,outcome).contains(outcome));
    const auto box=regionInstructionBox(bounds.toRect(),QFontMetrics(QFont{}),text,selection);
    QVERIFY(bounds.contains(box));
   }
   const auto recording=regionInstructions({},true,"Display 1",bounds,frames,false,outcome);
   QVERIFY(!recording.contains(outcome));QVERIFY(recording.endsWith("Start recording comes next"));
  }
 }
 void destinationAndSharingWrapInsideTheBadge(){
  auto font=nativeRegionFont();font.setPixelSize(21);const QFontMetrics metrics(font);const QRect bounds(0,0,640,480);
  const auto shortText=regionInstructions({},false,{},bounds,{},false,"Uploads immediately to Team Drive · Shared link");
  const auto longText=regionInstructions({},false,{},bounds,{},false,"Uploads immediately to Team Drive for the quarterly release readiness review and accessibility notes · Private upload");
  const auto shortBox=regionInstructionBox(bounds,metrics,shortText,{}, {},font),longBox=regionInstructionBox(bounds,metrics,longText,{}, {},font);
  const int margin=Graphite::space(Graphite::Space::l);QVERIFY(bounds.adjusted(margin,margin,-margin,-margin).contains(longBox));QVERIFY(longBox.height()>shortBox.height());

 }
 void hintsReflectActionAndState(){
  for(bool recording:{false,true}){
   QRectF selected;const QRectF screen(0,0,800,600);
   const QList<CaptureFrame> frames{{"DP-2",screen,QRect(0,0,800,600),1,0,{}}};
   const auto initial=regionInstructions(selected,recording,"DP-2",screen,frames);
   QVERIFY(initial.contains(recording?"Enter to choose":"Enter to capture"));
   QVERIFY(initial.contains("Drag to select · Arrows move"));
   QVERIFY(initial.startsWith("Entire display · 800 × 600 px · "));QVERIFY(!initial.contains("logical pixels"));
   QCOMPARE(regionKey(selected,screen,Qt::Key_Right),RegionKeyAction::Change);
   const auto changed=regionInstructions(selected,recording,"DP-2",screen,frames);
   QVERIFY(changed.contains(recording?"Enter to choose":"Enter to capture"));
   const auto dragging=regionInstructions(selected,recording,"DP-2",screen,frames,true);
   QVERIFY(dragging.contains(recording?"Release to choose recording area":"Release to capture"));
   QVERIFY(!dragging.contains("Enter to")&&!dragging.contains("Drag to select"));
   QCOMPARE(regionDescription(selected,recording,"DP-2",screen,frames,true).contains("Release to"),true);
   QVERIFY(!changed.contains("entire display"));QVERIFY(changed.contains("400 × 300"));
  }
 }
 void hintHasThreeLevelsAndNoSingleDisplayNoise(){
  const QRectF bounds(0,0,800,600);const QList<CaptureFrame> frames{{"Display 1",bounds,bounds.toRect(),1,0,{}}};
  const auto text=regionInstructions(QRectF(20,20,400,400),false,"Display 1",bounds,frames,false,"Opens the image editor");
  const auto lines=text.split('\n');QCOMPARE(lines.size(),3);
  QCOMPARE(lines[0],QString("400 × 400 px · Enter to capture · Esc to cancel"));
  QVERIFY(lines[1].contains("Arrows move"));QVERIFY(!text.contains("Display 1"));QVERIFY(!text.contains("Tab next display"));
  QCOMPARE(lines[2],QString("Opens the image editor"));
 }
 void outputPixelSizesUseTheCaptureGrid() {
  const QList<CaptureFrame> frames{{"Left",QRectF(-800,100,800,600),QRect(0,0,1000,750),1.25,0,{}},
   {"Right",QRectF(0,-200,800,600),QRect(0,0,1600,1200),2,0,{}}};
  QCOMPARE(regionOutputSize(QRectF(-799,101,101,51),frames),QSize(127,64));
  QCOMPARE(regionOutputSize(QRectF(-100,100,200,100),frames),QSize(400,200));
  QCOMPARE(regionOutputSize(QRectF(-800,100,800,600),frames),QSize(1000,750));
  const auto hint=regionInstructions({},false,"Left",frames[0].logicalRect,frames);
  QVERIFY(hint.contains("Entire display · 1000 × 750 px"));
  QVERIFY(regionInstructions(QRectF(-799,101,101,51),true,"Left",frames[0].logicalRect,frames).contains("127 × 64 px"));
  QVERIFY(regionInstructions(QRectF(0,0,50,30),false,"Unknown",QRectF(0,0,800,600),{}).contains("50 × 30 logical px"));
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
