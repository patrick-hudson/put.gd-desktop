#pragma once
#include <QColor>
#include <QJsonObject>
#include <QObject>
#include <QString>

namespace putgd::Theme {
// #60: one set of named colour tokens. Dark and light are two values of the
// same set; stylesheets are generated from it (%token% placeholders), custom
// painting and IconArt glyphs read it when they draw, so a scheme change
// reaches every open window without a restart. Phase 2 (a custom accent and
// per-token overrides) supplies user values for these same tokens.
enum class Scheme { Dark, Light };
struct Palette {
 QColor well,bg,surface,surfaceRaised,selected,border,borderStrong;
 QColor text,textSecondary,textMuted,textDisabled;
 QColor accent,accentHover,onAccent,link;
 QColor success,warning,danger,dangerHover,successSurface,warningSurface,dangerSurface;
 // IconArt glyphs: the main stroke and the darker or lighter secondary fill.
 QColor icon,iconSecondary;
 // The boundary of an essential control (text field, combo box, spin box,
 // check box, radio button): 3:1 against bg, surface and surfaceRaised, unlike
 // the quiet decorative border.
 QColor controlOutline;
 // IconArt glyphs on the selected fill (a selected row, segment or menu
 // item): both tones 3:1 against selected and against each other, so a
 // selected glyph keeps its silhouette and its inner detail.
 QColor iconSelected,iconSecondarySelected;
 QColor lead,leadOutline;
};
const Palette& palette(Scheme);
const Palette& current();
Scheme scheme();
QString name(Scheme);
// Token name to #rrggbb, for view state, tests and the CLI/MCP.
QJsonObject tokens(Scheme);
// Replaces each %token% in a stylesheet template with the scheme's colour.
QString stylesheet(QString qssTemplate,Scheme);
// Switches the active scheme and emits Notifier::changed; false when it is
// already active.
bool setScheme(Scheme);
// Drawn over user media (a video poster's play glyph and length): the same in
// every scheme, because the picture under it is not themed.
inline QColor mediaScrim(){return QColor(0,0,0,150);}
inline QColor onMedia(){return QColor(255,255,255);}
// The brand glyph's own colours (window and app icons): identity, not theme.
inline QColor brandAccent(){return QColor("#E8B66D");}
inline QColor brandTile(){return QColor("#222222");}
inline QColor trayOutline(){return QColor("#182126");}
// Tray glyph tints (#60 item 5). The panel's colour, not the app theme,
// decides which set is readable: a light app on a dark panel, or the
// reverse, keeps a visible icon. Each meets 3:1 on its kind of panel.
struct TrayTints {QColor idle,upload,encoding,error;};
const TrayTints& trayTints(bool lightPanel);
// WCAG 2.x contrast ratio of two opaque colours (1 to 21).
double contrast(const QColor&,const QColor&);

class Notifier final:public QObject {
 Q_OBJECT
public:
 static Notifier& instance();
signals:
 void changed();
private:
 Notifier()=default;
};
}
