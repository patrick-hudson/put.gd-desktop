#pragma once
#include <QFileSystemWatcher>
#include <QObject>
#include <QString>
#include <QTimer>
#include <utility>

namespace putgd {
// Whether the panel that shows the tray icon is light (#60 item 5). The panel,
// not the app theme, decides the tray tint: GNOME Shell's top bar is dark in
// both schemes; Plasma's follows the Plasma style, whose own colours win when
// it ships them (Breeze Dark, Kubuntu) and the colour scheme otherwise;
// Cinnamon's follows its desktop theme. Unknown desktops keep the dark-panel
// tint put.gd always used.
class PanelTone final:public QObject {
 Q_OBJECT
public:
 explicit PanelTone(QObject* parent=nullptr);
 bool light()const{return light_;}
 QString source()const{return source_;}
 // plasmarc and kdeglobals are the first files in the configuration cascade
 // (~/.config, ~/.config/kdedefaults, XDG_CONFIG_DIRS) that set the value;
 // styleColors is the Plasma style's own "colors" file, when it has one.
 struct Inputs {QString desktop,plasmarc,kdeglobals,cinnamonTheme,styleColors;};
 // The Plasma style named by the configuration cascade ("default" when none is).
 static QString plasmaStyle();
 // The rules alone: light or not, and which input decided it.
 static std::pair<bool,QString> decide(const Inputs&);
signals:
 void changed();
private:
 void refresh();
 void settle(const Inputs&);
 bool light_=false;QString source_="default";
 QFileSystemWatcher watcher_;QTimer debounce_;
};
}
