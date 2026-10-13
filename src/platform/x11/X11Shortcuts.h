#pragma once
#include <QObject>
#include "core/Error.h"
#include <functional>
#include <QHash>
#include <QJsonObject>
#include <QSocketNotifier>
#include <xcb/xcb.h>
#include <xcb/xcb_keysyms.h>
namespace putgd {
class X11Shortcuts final:public QObject {
 Q_OBJECT
public:
 explicit X11Shortcuts(QObject* parent=nullptr);
 ~X11Shortcuts() override;
 bool available()const;
 void setRecording(bool,std::function<void(Error)>);
 void bind(const QHash<QString,QString>&);
 QJsonObject status()const{return status_;}
signals:
 void activated(const QString& action);
 void bindingsChanged();
private:
 struct Grab {QString action;xcb_keycode_t code;uint16_t modifiers;};
 void events();
 void clear();
 uint16_t modifier(const xcb_get_modifier_mapping_reply_t*,xcb_keysym_t,xcb_keysym_t);
 xcb_connection_t* connection_=nullptr;
 xcb_window_t root_=XCB_WINDOW_NONE;
 xcb_key_symbols_t* symbols_=nullptr;
 QSocketNotifier* notifier_=nullptr;
 QList<Grab> grabs_;
 QHash<QString,QString> requested_;
 QJsonObject status_;
 uint16_t locks_=0;
 bool recording_=false;
};
}
