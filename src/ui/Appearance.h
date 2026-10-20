#pragma once
#include "Theme.h"
#include <QFileSystemWatcher>
#include <QJsonObject>
#include <QObject>
#include <QTimer>
#include <utility>

namespace putgd {
// The interface's colour scheme (#60 phase 1): preferences.appearance.theme is
// "system" (the default), "dark" or "light", and "system" follows the desktop
// live. The desktop's preference comes from, in order: Qt 6.5+ style hints;
// the XDG portal's org.freedesktop.appearance color-scheme (Qt 6.4.2 has no
// style hint); the portal's GNOME interface settings, which GNOME and
// Cinnamon's XApp backend both serve (color-scheme, then the GTK theme name);
// and Plasma's kdeglobals window colour. When nothing says, it stays dark.
class Appearance final:public QObject {
 Q_OBJECT
public:
 explicit Appearance(QObject* parent=nullptr);
 // "system", "dark" or "light"; anything else reads as "system".
 void setPreference(const QString&);
 QString preference()const{return preference_;}
 Theme::Scheme scheme()const;
 // Setting, desktop preference and its source, the resolved scheme and its tokens.
 QJsonObject state()const;
 struct Inputs {
  int qt=0;      // 1 light, 2 dark (Qt::ColorScheme), 0 unknown
  int portal=0;  // org.freedesktop.appearance color-scheme: 1 dark, 2 light, 0 none
  QString gnomeScheme,gtkTheme,kdeglobals;bool kde=false;
 };
 // The desktop's preference ("dark", "light" or "" when nothing says) and
 // which input decided it.
 static std::pair<QString,QString> decide(const Inputs&);
signals:
 void changed();
private:
 void readPortal();
 void readKde();
 void evaluate();
 QString preference_="system",desktop_,source_;
 Inputs inputs_;
 QFileSystemWatcher watcher_;QTimer debounce_;
 class PortalSettings* portal_=nullptr;
};
}
