#pragma once
#include <QObject>
#include "app/OperationService.h"
class QSystemTrayIcon;
namespace putgd {
class SettingsWindow;class HistoryWindow;class RecentOperations;
class TrayController final:public QObject {
 Q_OBJECT
public:TrayController(OperationService&,SettingsWindow&,HistoryWindow&,RecentOperations&,QObject* parent=nullptr);bool available()const;void activate();void showSettings();void showHistory();void showActivity();
signals:void quitRequested();
private:OperationService& operations_;SettingsWindow& settings_;HistoryWindow& history_;RecentOperations& activity_;QSystemTrayIcon* tray_;
};
}
