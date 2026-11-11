#include "ui/IconArt.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include <QtTest>
using namespace putgd;
using IconArt::TrayGlyph;
using IconArt::TrayIcon;
namespace {
int pixelsOf(const QImage& image,const QColor& color){int count=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==color)++count;return count;}
}
class TrayArt final:public QObject {
 Q_OBJECT
private slots:
 void qtSelectsTheDirectSmallFrameAndFallsBackForBadges(){
  const auto& tints=Theme::trayTints(false);
  for(int percent:{-1,0,50,100}){
   const TrayIcon state{tints.idle,percent,0,TrayGlyph::Turkey};
   const auto image=IconArt::liftTrayAtSize(state,16);
   QCOMPARE(image.size(),QSize(16,16));
   if(percent<=0){QVERIFY(pixelsOf(image,tints.idle)>0);QCOMPARE(pixelsOf(image,tints.encoding),0);}
   if(percent==50){QVERIFY(pixelsOf(image,tints.idle)>0);QVERIFY(pixelsOf(image,tints.encoding)>0);}
   if(percent==100){QCOMPARE(pixelsOf(image,tints.idle),0);QVERIFY(pixelsOf(image,tints.encoding)>0);}
   QCOMPARE(Ui::trayIcon(state).pixmap(16,16).toImage().convertToFormat(QImage::Format_ARGB32),image);
  }
  for(const auto& state:{TrayIcon{tints.idle,-2,0,TrayGlyph::Turkey},TrayIcon{tints.idle,-1,1,TrayGlyph::Turkey},TrayIcon{tints.idle,-1,2,TrayGlyph::Turkey}}){
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
};
QTEST_MAIN(TrayArt)
#include "test_tray_art.moc"
