#pragma once
#include "app/OperationService.h"
#include <QObject>
#include <functional>
#include <memory>

class QMenu;
class QSystemTrayIcon;

namespace putgd {
class TrayController final : public QObject {
 Q_OBJECT
public:
 TrayController(OperationService&, std::function<void(const QString&)> openView,
                std::function<void()> quit, QObject* parent = nullptr);
 ~TrayController() override;
 bool available() const;
 void activate();
 void showSettings();
 void showHistory();
 void showActivity();
 void notifyFailure(const Error&);

private:
 void showAbout();
 void showView(const QString&);
 OperationService& operations_;
 std::function<void(const QString&)> openView_;
 std::function<void()> quit_;
 QSystemTrayIcon* tray_;
 std::unique_ptr<QMenu> menu_;
};
}
