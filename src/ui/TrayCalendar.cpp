#include "TrayCalendar.h"
#ifdef Q_OS_LINUX
#include <QDBusConnection>
#endif

namespace putgd {
namespace {
// The timer doesn't run during suspend and doesn't notice a clock change, so it
// never sleeps longer than this however far away midnight is.
constexpr qint64 longestSleepMs=5*60*1000;
// Landing just past midnight, not just before it.
constexpr qint64 pastMidnightMs=50;
}
TrayCalendar::TrayCalendar(QObject* parent):QObject(parent){
 if(const auto value=qEnvironmentVariable("PUTGD_TRAY_CALENDAR_NOW");!value.isEmpty()){
  const auto start=QDateTime::fromString(value,Qt::ISODate);
  if(start.isValid()){overridden_=true;offsetSeconds_=QDateTime::currentDateTime().secsTo(start);qWarning("Tray calendar overridden for this process: %s",qPrintable(value));}
  else qWarning("Ignoring PUTGD_TRAY_CALENDAR_NOW: %s is not a local ISO date",qPrintable(value));
 }
 date_=now().date();glyph_=glyphFor(date_);
 timer_.setSingleShot(true);timer_.setTimerType(Qt::PreciseTimer);
 connect(&timer_,&QTimer::timeout,this,&TrayCalendar::check);
#ifdef Q_OS_LINUX
 QDBusConnection::systemBus().connect("org.freedesktop.login1","/org/freedesktop/login1","org.freedesktop.login1.Manager","PrepareForSleep",this,SLOT(sleeping(bool)));
#endif
 schedule();
}
IconArt::TrayGlyph TrayCalendar::glyphFor(const QDate& date){
 return date.month()==11?IconArt::TrayGlyph::Turkey:date.month()==12?IconArt::TrayGlyph::Tree:IconArt::TrayGlyph::Mark;
}
qint64 TrayCalendar::msUntilTomorrow(const QDateTime& now){
 return qMax(qint64(1),now.msecsTo(now.date().addDays(1).startOfDay()));
}
QDateTime TrayCalendar::now()const{return QDateTime::currentDateTime().addSecs(offsetSeconds_);}
void TrayCalendar::schedule(){
 timer_.start(int(qMin(longestSleepMs,msUntilTomorrow(now())+pastMidnightMs)));
}
void TrayCalendar::check(){
 const auto date=now().date();date_=date;
 schedule();
 const auto glyph=glyphFor(date);
 if(glyph==glyph_)return;
 glyph_=glyph;emit glyphChanged();
}
void TrayCalendar::sleeping(bool entering){if(!entering)check();}
}
