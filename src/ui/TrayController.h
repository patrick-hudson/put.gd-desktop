#pragma once
#include "app/OperationService.h"
#include <QObject>
#include <QElapsedTimer>
#include <QColor>
#include <QTimer>
#include <functional>
#include <memory>

class QMenu;
class QSystemTrayIcon;

namespace putgd {
#ifdef Q_OS_LINUX
class NativeTray;
#endif
class TrayController final : public QObject {
 Q_OBJECT
public:
 TrayController(OperationService&, std::function<void(const QString&)> openView,
                std::function<void()> quit, QObject* parent = nullptr);
 ~TrayController() override;
 bool available() const;
 bool availabilityKnown() const;
 void activate();
 void showSettings();
 void showHistory();
 void showActivity();
 void notifyFailure(const Error&);
 void invokeShortcut(const QString&);

signals:
 void availabilityResolved();

private:
 void invokeAction(const QString&, bool fromShortcut = false);
 QString lastShortcutError_;
 QElapsedTimer shortcutFeedbackClock_;
 void notifyMessage(const QString&, const QString&, bool warning);
 void refreshActivity();
 void flashError();
 QTimer errorFlashTimer_,errorRecoveryTimer_;
 bool errorRed_=false;
 QColor iconColor_=QColor("#9bbcff");
 QString activityText_;
 void showAbout();
 void showView(const QString&);
 OperationService& operations_;
 std::function<void(const QString&)> openView_;
 std::function<void()> quit_;
 QSystemTrayIcon* tray_ = nullptr;
#ifdef Q_OS_LINUX
 std::unique_ptr<NativeTray> native_;
#endif
 std::unique_ptr<QMenu> menu_;
};
}
