#include "ui/TrayCalendar.h"
#include "ui/IconArt.h"
#include "ui/Theme.h"
#include "ui/Ui.h"
#include "fixtures/Waits.h"
#include <QSignalSpy>
#include <QtTest>
#include <ctime>

using namespace putgd;
using IconArt::TrayGlyph;
using IconArt::TrayIcon;

namespace {
struct CalendarEnv {
 explicit CalendarEnv(const QByteArray& now){qputenv("PUTGD_TRAY_CALENDAR_NOW",now);}
 ~CalendarEnv(){qunsetenv("PUTGD_TRAY_CALENDAR_NOW");}
};
int pixelsOf(const QImage& image,const QColor& color){
 int count=0;for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(image.pixelColor(x,y)==color)++count;return count;
}
QColor idle(){return Theme::trayTints(false).idle;}
QColor amber(){return Theme::trayTints(false).encoding;}
}
Q_DECLARE_METATYPE(TrayGlyph)

class TraySeason final:public QObject {
 Q_OBJECT
private slots:
 void datesPickTheGlyph_data(){
  QTest::addColumn<QDate>("date");QTest::addColumn<TrayGlyph>("glyph");
  QTest::newRow("Oct 31")<<QDate(2026,10,31)<<TrayGlyph::Mark;
  QTest::newRow("Nov 1")<<QDate(2026,11,1)<<TrayGlyph::Turkey;
  QTest::newRow("Nov 30")<<QDate(2026,11,30)<<TrayGlyph::Turkey;
  QTest::newRow("Dec 1")<<QDate(2026,12,1)<<TrayGlyph::Tree;
  QTest::newRow("Dec 31")<<QDate(2026,12,31)<<TrayGlyph::Tree;
  QTest::newRow("Jan 1")<<QDate(2027,1,1)<<TrayGlyph::Mark;
  QTest::newRow("another year")<<QDate(2031,11,15)<<TrayGlyph::Turkey;
 }
 void datesPickTheGlyph(){
  QFETCH(QDate,date);QFETCH(TrayGlyph,glyph);QCOMPARE(TrayCalendar::glyphFor(date),glyph);
 }
 void withoutAnOverrideItReadsTheRealLocalDate(){
  qunsetenv("PUTGD_TRAY_CALENDAR_NOW");
  TrayCalendar calendar;QVERIFY(!calendar.overridden());
  QCOMPARE(calendar.date(),QDate::currentDate());QCOMPARE(calendar.glyph(),TrayCalendar::glyphFor(QDate::currentDate()));
 }
 void badOverrideIsIgnored(){
  CalendarEnv env("the first of december");
  QTest::ignoreMessage(QtWarningMsg,QRegularExpression("Ignoring PUTGD_TRAY_CALENDAR_NOW"));
  TrayCalendar calendar;QVERIFY(!calendar.overridden());QCOMPARE(calendar.date(),QDate::currentDate());
 }
 // A running resident crosses each boundary by itself, without a restart.
 void midnightChangesTheGlyphWithoutARestart_data(){
  QTest::addColumn<QByteArray>("start");QTest::addColumn<TrayGlyph>("before");QTest::addColumn<TrayGlyph>("after");
  QTest::newRow("into November")<<QByteArray("2026-10-31T23:59:59")<<TrayGlyph::Mark<<TrayGlyph::Turkey;
  QTest::newRow("into December")<<QByteArray("2026-11-30T23:59:59")<<TrayGlyph::Turkey<<TrayGlyph::Tree;
  QTest::newRow("into January")<<QByteArray("2026-12-31T23:59:59")<<TrayGlyph::Tree<<TrayGlyph::Mark;
 }
 void midnightChangesTheGlyphWithoutARestart(){
  QFETCH(QByteArray,start);QFETCH(TrayGlyph,before);QFETCH(TrayGlyph,after);
  CalendarEnv env(start);QTest::ignoreMessage(QtWarningMsg,QRegularExpression("Tray calendar overridden"));
  TrayCalendar calendar;QVERIFY(calendar.overridden());QCOMPARE(calendar.glyph(),before);
  QSignalSpy changed(&calendar,&TrayCalendar::glyphChanged);
  QVERIFY(changed.wait(3000));QCOMPARE(calendar.glyph(),after);QCOMPARE(changed.size(),1);
  calendar.check();QCOMPARE(changed.size(),1);
 }
 void midnightInsideAMonthChangesNothing(){
  CalendarEnv env("2026-11-14T23:59:59");QTest::ignoreMessage(QtWarningMsg,QRegularExpression("Tray calendar overridden"));
  TrayCalendar calendar;const auto first=calendar.date();QSignalSpy changed(&calendar,&TrayCalendar::glyphChanged);
  QVERIFY(!changed.wait(1500));QCOMPARE(calendar.date(),first.addDays(1));
 }
 // A time zone change is caught when the date is next read, on the five-minute
 // recheck at the latest. Nov 30 23:30 UTC is already Dec 1 in Tokyo.
 void timeZoneChangeIsNoticed(){
  const auto zone=qgetenv("TZ");auto restore=qScopeGuard([&]{if(zone.isNull())qunsetenv("TZ");else qputenv("TZ",zone);tzset();});
  qputenv("TZ","UTC");tzset();
  CalendarEnv env("2026-11-30T23:30:00");QTest::ignoreMessage(QtWarningMsg,QRegularExpression("Tray calendar overridden"));
  TrayCalendar calendar;QCOMPARE(calendar.glyph(),TrayGlyph::Turkey);
  QSignalSpy changed(&calendar,&TrayCalendar::glyphChanged);
  qputenv("TZ","Asia/Tokyo");tzset();calendar.check();
  QCOMPARE(changed.size(),1);QCOMPARE(calendar.glyph(),TrayGlyph::Tree);
 }
 void timeUntilTomorrowCountsToLocalMidnight(){
  const QDateTime late(QDate(2026,11,30),QTime(23,59,59,500));QCOMPARE(TrayCalendar::msUntilTomorrow(late),qint64(500));
  QCOMPARE(TrayCalendar::msUntilTomorrow(QDateTime(QDate(2026,6,1),QTime(0,0))),qint64(86400000));
 }
 void markKeepsItsExactPixels(){
  for(int percent:{-1,-2,0,50,100})for(int scale:{1,2,3})QCOMPARE(IconArt::liftTray(TrayIcon{idle(),percent},scale),IconArt::liftTray(idle(),percent,scale));
 }
 void everyGlyphKeepsTheBowl(){
  // The bowl's solid body and its transparent well match the mark's below y=17;
  // only the contour's width differs.
  const auto mark=IconArt::liftTray(idle(),-1,8);
  for(const auto glyph:{TrayGlyph::Turkey,TrayGlyph::Tree}){
   const auto image=IconArt::liftTray(TrayIcon{idle(),-1,0,glyph},8);
   for(int y=17*8;y<192;++y)for(int x=0;x<192;++x){
    if(mark.pixelColor(x,y)==idle())QCOMPARE(image.pixelColor(x,y),idle());
    if(qAlpha(mark.pixel(x,y))==0)QCOMPARE(qAlpha(image.pixel(x,y)),0);
   }
  }
 }
 void seasonalArtReachesItsOwnHeight(){
  const auto top=[](const QImage& image){for(int y=0;y<image.height();++y)for(int x=0;x<image.width();++x)if(qAlpha(image.pixel(x,y)))return y;return -1;};
  QVERIFY(top(IconArt::liftTray(TrayIcon{idle(),-1,0,TrayGlyph::Turkey},8))<2*8);
  QVERIFY(top(IconArt::liftTray(TrayIcon{idle(),-1,0,TrayGlyph::Tree},8))<1*8);
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
  // The head and the star turn amber at 100%, above the old fill top of y=2.
  if(glyph==TrayGlyph::Turkey)QCOMPARE(IconArt::liftTray(TrayIcon{idle(),100,0,glyph},8).pixelColor(int(15.5*8),int(1.5*8)),amber());
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
 void cutoutsAndTheGapStayTransparent(){
  // Tree: the gap to the bowl on both sides and below, and the ornaments.
  for(int percent:{-1,50,100}){
   const auto tree=IconArt::liftTray(TrayIcon{idle(),percent,0,TrayGlyph::Tree},8);
   QCOMPARE(qAlpha(tree.pixel(int(6.5*8),14*8)),0);QCOMPARE(qAlpha(tree.pixel(int(17.5*8),14*8)),0);QCOMPARE(qAlpha(tree.pixel(12*8,18*8)),0);
   QCOMPARE(qAlpha(tree.pixel(int(10.3*8),int(9.8*8))),0);QCOMPARE(qAlpha(tree.pixel(int(14.2*8),int(13.2*8))),0);
   const auto turkey=IconArt::liftTray(TrayIcon{idle(),percent,0,TrayGlyph::Turkey},8);
   QCOMPARE(qAlpha(turkey.pixel(int(17.5*8),int(2.9*8))),0);
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
 void qtTrayIconIsTheSameFrame(){
  const TrayIcon icon{idle(),50,0,TrayGlyph::Tree};
  QCOMPARE(Ui::trayIcon(icon).pixmap(48,48).toImage().convertToFormat(QImage::Format_ARGB32),[&]{auto image=IconArt::liftTray(icon,2);image.setDevicePixelRatio(1);return image;}());
  // The cached Ui::icon paths don't carry a season into the ordinary mark.
  const auto plain=Ui::icon("tray",idle()).pixmap(24,24).toImage();
  QVERIFY(Ui::trayIcon({idle(),-1,0,TrayGlyph::Turkey}).pixmap(24,24).toImage()!=plain);
  QCOMPARE(Ui::trayIcon({idle()}).pixmap(24,24).toImage(),plain);
 }
};

QTEST_MAIN(TraySeason)
#include "test_tray_season.moc"
