#include "TrayController.h"
#include "VisualStyle.h"
#include "SettingsWindow.h"
#include "HistoryWindow.h"
#include "RecentOperations.h"
#include <QSystemTrayIcon>
#include <QMenu>
#include <QApplication>
#include <QStyle>
#include <QPointer>
namespace putgd {
namespace{void showWindow(QWidget& window){window.show();window.raise();window.activateWindow();}}
TrayController::TrayController(OperationService& operations,SettingsWindow& settings,HistoryWindow& history,RecentOperations& activity,QObject* parent):QObject(parent),operations_(operations),settings_(settings),history_(history),activity_(activity){connect(&settings_,&SettingsWindow::actionRequested,this,[this](const QString& action){operations_.invoke(action,[g=QPointer<TrayController>(this)](Error,QJsonObject){if(g)g->showActivity();});});tray_=new QSystemTrayIcon(VisualStyle::icon("brand"),this);tray_->setToolTip("put.gd — Share and local capture");auto* menu=new QMenu(&settings_);for(const auto& action:Settings::actionNames()){auto* item=menu->addAction(VisualStyle::icon(action),"Capture "+action);connect(item,&QAction::triggered,this,[this,action]{operations_.invoke(action,[g=QPointer<TrayController>(this)](Error e,QJsonObject){if(g&&e){g->showActivity();if(g->available())g->tray_->showMessage("put.gd",e.message,QSystemTrayIcon::Warning);}});});}menu->addSeparator();connect(menu->addAction(VisualStyle::icon("preferences"),"Settings and setup"),&QAction::triggered,this,&TrayController::showSettings);connect(menu->addAction(VisualStyle::icon("history"),"Upload history"),&QAction::triggered,this,&TrayController::showHistory);connect(menu->addAction(VisualStyle::icon("activity"),"Recent activity"),&QAction::triggered,this,&TrayController::showActivity);menu->addSeparator();connect(menu->addAction("Quit"),&QAction::triggered,this,&TrayController::quitRequested);tray_->setContextMenu(menu);connect(tray_,&QSystemTrayIcon::activated,this,[this](auto reason){if(reason==QSystemTrayIcon::Trigger||reason==QSystemTrayIcon::DoubleClick)showActivity();});if(available())tray_->show();connect(&operations_,&OperationService::changed,this,[this](const QString& id){if(!available()||!operations_.settingsSnapshot().notifications)return;operations_.inspect(id,[g=QPointer<TrayController>(this)](Error e,QJsonObject op){if(!g||e)return;auto state=op["state"].toString();if((state=="completed"||state=="partial")&&op["copyStatus"]=="pending")return;if(QStringList{"completed","partial","failed","interrupted"}.contains(state))g->tray_->showMessage("put.gd — "+state,"Clipboard: "+op["copyStatus"].toString()+". Open recent activity for per-item results.",state=="completed"?QSystemTrayIcon::Information:QSystemTrayIcon::Warning);});});}
bool TrayController::available()const{return QSystemTrayIcon::isSystemTrayAvailable();}
void TrayController::activate(){showSettings();}
void TrayController::showSettings(){settings_.refreshDiagnostics();showWindow(settings_);}
void TrayController::showHistory(){history_.refresh();showWindow(history_);}
void TrayController::showActivity(){activity_.refresh();showWindow(activity_);}
}
