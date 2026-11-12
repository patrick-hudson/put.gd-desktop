#pragma once
#include <functional>
#include "IconArt.h"
#include <QDBusConnection>
#include <QObject>
#include <QColor>
#include <QUrl>
#include <QHash>
#include <memory>

namespace putgd {
class NativeTrayPrivate;
// A notification button: `label` is shown; invoking it emits notificationAction(key, value).
struct NotificationAction {QString key, label, value;};
// StatusNotifier and dbusmenu transport; no windowing application is required.
// Rendering to a QImage is software-only. Native desktop acceptance is separate
// from tests of these wire methods.
class NativeTray final : public QObject {
 Q_OBJECT
public:
 explicit NativeTray(QObject* parent = nullptr);
 NativeTray(const QDBusConnection& bus, QObject* parent = nullptr);
 ~NativeTray() override;
 bool available() const;
 bool availabilityKnown() const;
 void setActivity(const QColor& color, const QString& description, int percent = -1,int recording = 0,const QString& title = {},IconArt::TrayGlyph glyph = IconArt::TrayGlyph::Mark);
 // Shows the "update" menu entry with this label; empty hides it (#293).
 void setUpdateLabel(const QString& label);
 void setCommands(const QHash<QString,QString>& shortcuts,const QString& uploadStatus,int uploads=1,bool failed=false,const QString& recording = {},const QString& attention = "Failed uploads");
 void setOutput(const QString& summary,const QStringList& details);
 // The capture delay the radio submenu shows as chosen (#1363): 0, 3, 5 or 10.
 void setCaptureDelay(int seconds);
 void notify(const QString& title, const QString& message, bool warning = false, const QUrl& link = {},
             const QList<NotificationAction>& actions = {});
 // Closes every notification this tray sent that its server has not reported
 // closed (CloseNotification), buttons and links with them; how many it asked for,
 // those whose Notify has not been answered yet included (they are closed when
 // their ids arrive). Where the server leaves a closed notification on screen
 // it is dismissed there as well (notifications::dismissCommand).
 // A scroll capture clears the screen of put.gd's own notices this way (#1376).
 // `gone` is called once, when the server has reported all of them closed and
 // every dismissal has run, or closeConfirmMs after the call; never when
 // there was nothing to close.
 int closeNotifications(std::function<void()> gone = {});
 static constexpr int closeConfirmMs = 1000;
 // expire_timeout for a notification on this server (GetServerInformation name):
 // 10 s, or 0 (until used or dismissed) for buttons the server would otherwise lose.
 static int notificationTimeoutMs(const QString& server, bool actions);
signals:
 void menuAboutToShow();
 void notificationActivated(const QUrl& link);
 void notificationAction(const QString& key, const QString& value);
 // activationToken: the host's xdg-activation token for this click, or empty (#657).
 void actionActivated(const QString& action, const QString& activationToken);
 void availabilityChanged(bool available);
 void availabilityResolved();
private:
 friend class NativeTrayPrivate;
 std::unique_ptr<NativeTrayPrivate> d_;
};
}
