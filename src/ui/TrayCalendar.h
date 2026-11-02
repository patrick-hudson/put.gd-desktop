#pragma once
#include "IconArt.h"
#include <QDateTime>
#include <QObject>
#include <QTimer>

namespace putgd {
// Which glyph the tray shows today, by the local calendar: a turkey through
// November, a tree through December, the ordinary mark otherwise. glyphChanged
// fires when the date crosses into another month's glyph, with no restart: at
// local midnight, on resume from suspend, and within five minutes of a clock or
// time zone change.
class TrayCalendar final:public QObject {
 Q_OBJECT
public:
 explicit TrayCalendar(QObject* parent=nullptr);
 static IconArt::TrayGlyph glyphFor(const QDate&);
 // How long until `now`'s local date ends, at least 1 ms.
 static qint64 msUntilTomorrow(const QDateTime& now);
 IconArt::TrayGlyph glyph()const{return glyph_;}
 QDate date()const{return date_;}
 // PUTGD_TRAY_CALENDAR_NOW (a local ISO date or date and time) starts this
 // calendar's clock there and lets it run, for lab previews and tests. It moves
 // only the tray glyph: nothing else in the app reads this clock.
 bool overridden()const{return overridden_;}
 // Re-reads the date, and emits glyphChanged if the glyph moved.
 void check();
signals:
 void glyphChanged();
private:
 QDateTime now()const;
 void schedule();
 Q_SLOT void sleeping(bool entering);
 qint64 offsetMs_=0;
 bool overridden_=false;
 QDate date_;
 IconArt::TrayGlyph glyph_=IconArt::TrayGlyph::Mark;
 QTimer timer_;
};
}
