#pragma once
#include <QObject>
#include <functional>
#include <memory>
#include "app/OperationService.h"
class QSystemTrayIcon;class QMenu;
namespace putgd {
class SettingsWindow;class HistoryWindow;class RecentOperations;
class TrayController final:public QObject {
 Q_OBJECT
public:TrayController(OperationService&,std::function<SettingsWindow&()>,std::function<HistoryWindow&()>,std::function<RecentOperations&()>,QObject* parent=nullptr);~TrayController() override;bool available()const;void activate();void showSettings();void showHistory();void showActivity();
signals:void quitRequested();
private:OperationService& operations_;std::function<SettingsWindow&()> createSettings_;std::function<HistoryWindow&()> createHistory_;std::function<RecentOperations&()> createActivity_;SettingsWindow* settings_=nullptr;HistoryWindow* history_=nullptr;RecentOperations* activity_=nullptr;QSystemTrayIcon* tray_;std::unique_ptr<QMenu> menu_;
};
}
