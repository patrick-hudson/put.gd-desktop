#include "ui/IconArt.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QtTest>
using namespace putgd;
using IconArt::TrayGlyph;
using IconArt::TrayIcon;
namespace {
int pixelsOf(const QImage& image,const QColor& color){int count=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==color)++count;return count;}
QColor idle(){return Theme::trayTints(false).idle;}
QColor amber(){return Theme::trayTints(false).encoding;}
}
Q_DECLARE_METATYPE(TrayGlyph)
class TrayArt final:public QObject {
 Q_OBJECT
private slots:
 void markOverloadsProduceTheSamePixels(){
  for(int percent:{-1,-2,0,50,100})for(int scale:{1,2,3})QCOMPARE(IconArt::liftTray(TrayIcon{idle(),percent},scale),IconArt::liftTray(idle(),percent,scale));
 }
 void turkeyKeepsASmallerOpenBowl(){
  for(int percent:{-1,50,100}){
   const auto image=IconArt::liftTray(TrayIcon{idle(),percent,0,TrayGlyph::Turkey},8);
   QCOMPARE(qAlpha(image.pixel(12*8,22*8)),255);
   QCOMPARE(qAlpha(image.pixel(12*8,19*8)),0);
   QCOMPARE(qAlpha(image.pixel(3*8,21*8)),0);
   QCOMPARE(qAlpha(image.pixel(22*8,21*8)),0);
  }
 }
 void turkeyNeckAndWattleStayOpen(){
  for(bool light:{false,true})for(int percent:{-1,50,100}){
   const auto image=IconArt::liftTray({Theme::trayTints(light).idle,percent,0,TrayGlyph::Turkey},8);
   QCOMPARE(qAlpha(image.pixel(int(14.5*8),4*8)),0);
   QCOMPARE(qAlpha(image.pixel(int(18.5*8),int(9.5*8))),0);
   QCOMPARE(qAlpha(image.pixel(int(12.5*8),int(8.7*8))),255);
  }
 }
 void fullFillCoversTheWholeSilhouette_data(){
  QTest::addColumn<TrayGlyph>("glyph");QTest::newRow("mark")<<TrayGlyph::Mark;QTest::newRow("turkey")<<TrayGlyph::Turkey;QTest::newRow("tree")<<TrayGlyph::Tree;
 }
 void fullFillCoversTheWholeSilhouette(){
  QFETCH(TrayGlyph,glyph);
  for(int scale:{1,2,3,8}){
   const auto image=IconArt::liftTray(TrayIcon{idle(),100,0,glyph},scale);
   QCOMPARE(pixelsOf(image,idle()),0);
   QVERIFY(pixelsOf(image,amber())>0);
  }
  // The head and the star turn amber at 100%, including the top of each glyph.
  if(glyph==TrayGlyph::Turkey)QCOMPARE(IconArt::liftTray(TrayIcon{idle(),100,0,glyph},8).pixelColor(int(17*8),int(2.5*8)),amber());
  if(glyph==TrayGlyph::Tree)QCOMPARE(IconArt::liftTray(TrayIcon{idle(),100,0,glyph},8).pixelColor(12*8,int(2.9*8)),amber());
 }
 void fillClimbsBottomUpWithGlyphBounds_data(){fullFillCoversTheWholeSilhouette_data();}
 void fillClimbsBottomUpWithGlyphBounds(){
  QFETCH(TrayGlyph,glyph);int previous=-1;
  for(int percent:{0,25,50,75,100}){
   const auto count=pixelsOf(IconArt::liftTray(TrayIcon{idle(),percent,0,glyph},8),amber());
   if(percent==0)QCOMPARE(count,0);else QVERIFY(count>previous);previous=count;
  }
 }
 void aBadgeDrawsOnTheOrdinaryMark_data(){
  QTest::addColumn<int>("percent");QTest::addColumn<int>("recording");
  QTest::newRow("failed")<<-2<<0;QTest::newRow("recording")<<-1<<1;QTest::newRow("paused")<<-1<<2;
  QTest::newRow("failed while recording")<<-2<<1;QTest::newRow("failed while paused")<<-2<<2;
  QTest::newRow("uploading while recording")<<50<<1;QTest::newRow("uploading while paused")<<50<<2;
 }
 void aBadgeDrawsOnTheOrdinaryMark(){
  QFETCH(int,percent);QFETCH(int,recording);
  const auto mark=IconArt::liftTray(idle(),percent,2,recording);
  for(const auto glyph:{TrayGlyph::Turkey,TrayGlyph::Tree})QCOMPARE(IconArt::liftTray(TrayIcon{idle(),percent,recording,glyph},2),mark);
 }
 void smallTurkeyUsesItsOwnOpticalFrame(){
  for(int percent:{-1,0,50,100}){
   const TrayIcon state{idle(),percent,0,TrayGlyph::Turkey};
   const auto image=IconArt::liftTrayAtSize(state,16);
   QCOMPARE(image.size(),QSize(16,16));
   // Soft contours remain intentional; the eye, wing and throat must still
   // contrast with the solid bird, and the compact bowl must retain its well.
   QVERIFY(qAlpha(image.pixel(12,4))<180); // eye
   QCOMPARE(qAlpha(image.pixel(5,4)),255); // solid fan
   QVERIFY(qAlpha(image.pixel(9,5))<96); // neck gap
   QVERIFY(qAlpha(image.pixel(6,8))<96); // curved wing
   QVERIFY(qAlpha(image.pixel(12,7))<96); // throat gap
   QVERIFY(qAlpha(image.pixel(13,7))>160); // hanging wattle
   QCOMPARE(qAlpha(image.pixel(8,13)),0); // open bowl
   QVERIFY(qAlpha(image.pixel(11,13))>128); // raised end
   QVERIFY(qAlpha(image.pixel(8,14))>128); // bowl base
   QVERIFY(qAlpha(image.pixel(14,4))>128); // beak
   QVERIFY(qAlpha(image.pixel(14,6))<32); // clear below the beak
   if(percent==0){QCOMPARE(image.pixelColor(8,11),idle());QCOMPARE(pixelsOf(image,amber()),0);}
   if(percent==50){QCOMPARE(image.pixelColor(8,11),amber());QCOMPARE(image.pixelColor(11,5),idle());}
   if(percent==100){QCOMPARE(image.pixelColor(8,11),amber());QCOMPARE(image.pixelColor(11,5),amber());QCOMPARE(pixelsOf(image,idle()),0);}
   QCOMPARE(Ui::trayIcon(state).pixmap(16,16).toImage().convertToFormat(QImage::Format_ARGB32),image);
  }
  for(const auto& state:{TrayIcon{idle(),-2,0,TrayGlyph::Turkey},TrayIcon{idle(),-1,1,TrayGlyph::Turkey},TrayIcon{idle(),-1,2,TrayGlyph::Turkey}}){
   auto mark=state;mark.glyph=TrayGlyph::Mark;
   QCOMPARE(IconArt::liftTrayAtSize(state,16),IconArt::liftTrayAtSize(mark,16));
   QCOMPARE(Ui::trayIcon(state).pixmap(16,16).toImage(),Ui::trayIcon(mark).pixmap(16,16).toImage());
  }
 }
 void smallProgressKeepsOpaquePixelsInTheTwoStatusTints(){
  for(bool light:{false,true}){
   const auto& tints=Theme::trayTints(light);
   const auto idle=IconArt::liftTrayAtSize({tints.idle,-1,0,TrayGlyph::Turkey},16);
   for(int percent=0;percent<=100;++percent){
    const auto image=IconArt::liftTrayAtSize({tints.idle,percent,0,TrayGlyph::Turkey},16);
    for(int y=0;y<16;++y)for(int x=0;x<16;++x){
     const auto color=image.pixelColor(x,y);
     QCOMPARE(color.alpha(),idle.pixelColor(x,y).alpha());
     if(color.alpha()==255)QVERIFY(color==tints.idle||color==tints.encoding);
    }
   }
  }
 }
 void qtTrayIconIsTheSameFrame(){
  const TrayIcon icon{idle(),50,0,TrayGlyph::Tree};
  QCOMPARE(Ui::trayIcon(icon).pixmap(48,48).toImage().convertToFormat(QImage::Format_ARGB32),[&]{auto image=IconArt::liftTray(icon,2);image.setDevicePixelRatio(1);return image;}());
  // The cached Ui::icon paths don't carry a season into the ordinary mark.
  const auto plain=Ui::icon("tray",idle()).pixmap(24,24).toImage();
  QVERIFY(Ui::trayIcon({idle(),-1,0,TrayGlyph::Turkey}).pixmap(24,24).toImage()!=plain);
  QCOMPARE(Ui::trayIcon({idle()}).pixmap(24,24).toImage(),plain);
 }
};
QTEST_MAIN(TrayArt)
#include "test_tray_art.moc"
