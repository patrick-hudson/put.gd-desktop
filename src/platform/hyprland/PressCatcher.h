#pragma once
#include <QList>
#include <QObject>
#include <QPointF>
#include <QRectF>
#include <QString>
#include <memory>

namespace putgd {
// The region selector is a separate process, and until its Qt start has mapped
// a surface a press goes to the window underneath, which then keeps the whole
// drag: the first drag after the hotkey was lost (~0.2 s settled on Omarchy, up
// to ~3 s while its shell reacts to a new output). This keeps one quiet Wayland
// connection and maps transparent overlay surfaces the moment the hotkey arrives,
// and reports a drag that starts on them; the selector draws it.
//
// They stay under the selector until it ends, holding the input the selector
// cannot take yet, because of how Hyprland focuses layer surfaces (measured on
// 0.56): only one that accepts the keyboard gets pointer focus without the
// pointer moving, so these take it on demand; a surface that takes the keyboard
// when it maps, or that becomes exclusive, releases every held button and the
// real release is then swallowed, so the selector maps without the keyboard and
// switches to on-demand, which releases nothing but only takes effect on the
// next pointer motion. Until then Esc and the keyboard reach it through here.
class PressCatcher final:public QObject {
 Q_OBJECT
public:
 enum Kind {Press,Move,Release,Cancel};
 struct Area {QString output;QRectF logical;};
 explicit PressCatcher(QObject* parent=nullptr);
 ~PressCatcher() override;
 // Maps a surface on every listed output. False when the compositor offers no
 // layer shell or none of the outputs: the selector then works on its own.
 bool start(const QList<Area>&);
 // Ends it now, a drag in progress included.
 void stop();
 bool running()const;
signals:
 // kind is a Kind; logical is in layout coordinates.
 void pointer(int kind,QPointF logical);
 // A key other than Esc while no button is held, as a Qt key and modifiers.
 void key(int qtKey,int qtModifiers);
private:
 struct Impl;
 std::unique_ptr<Impl> d_;
};
}
