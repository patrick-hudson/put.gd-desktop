#include "TrayController.h"
#include "core/BuildInfo.h"
#include "VisualStyle.h"
#include "SettingsWindow.h"
#include "HistoryWindow.h"
#include "RecentOperations.h"
#include <QSystemTrayIcon>
#include <QMenu>
#include <QApplication>
#include <QStyle>
#include <QPointer>
#include <QMessageBox>
#include <QJsonDocument>
namespace putgd {
namespace{void showWindow(QWidget& window){window.show();window.raise();window.activateWindow();}}
TrayController::TrayController(OperationService& operations,std::function<SettingsWindow&()> createSettings,std::function<HistoryWindow&()> createHistory,std::function<RecentOperations&()> createActivity,QObject* parent):QObject(parent),operations_(operations),createSettings_(std::move(createSettings)),createHistory_(std::move(createHistory)),createActivity_(std::move(createActivity)){tray_=new QSystemTrayIcon(VisualStyle::icon("brand"),this);tray_->setToolTip("put.gd — Share and local capture");menu_=std::make_unique<QMenu>();auto* menu=menu_.get();for(const auto& action:Settings::actionNames()){auto* item=menu->addAction(VisualStyle::icon(action),"Capture "+action);connect(item,&QAction::triggered,this,[this,action]{operations_.invoke(action,[g=QPointer<TrayController>(this)](Error e,QJsonObject){if(g&&e){g->showActivity();if(g->available())g->tray_->showMessage("put.gd",e.message,QSystemTrayIcon::Warning);}});});}menu->addSeparator();connect(menu->addAction(VisualStyle::icon("preferences"),"Settings and setup"),&QAction::triggered,this,&TrayController::showSettings);connect(menu->addAction(VisualStyle::icon("history"),"Upload history"),&QAction::triggered,this,&TrayController::showHistory);connect(menu->addAction(VisualStyle::icon("activity"),"Recent activity"),&QAction::triggered,this,&TrayController::showActivity);menu->addSeparator();connect(menu->addAction("About put.gd"),&QAction::triggered,this,&TrayController::showAbout);connect(menu->addAction("Quit"),&QAction::triggered,this,&TrayController::quitRequested);tray_->setContextMenu(menu);connect(tray_,&QSystemTrayIcon::activated,this,[this](auto reason){if(reason==QSystemTrayIcon::Trigger||reason==QSystemTrayIcon::DoubleClick)showActivity();});if(available())tray_->show();connect(&operations_,&OperationService::changed,this,[this](const QString& id){if(!available()||!operations_.settingsSnapshot().notifications)return;operations_.inspect(id,[g=QPointer<TrayController>(this)](Error e,QJsonObject op){if(!g||e)return;auto state=op["state"].toString();if((state=="completed"||state=="partial")&&op["copyStatus"]=="pending")return;if(QStringList{"completed","partial","failed","interrupted"}.contains(state))g->tray_->showMessage("put.gd — "+state,"Clipboard: "+op["copyStatus"].toString()+". Open recent activity for per-item results.",state=="completed"?QSystemTrayIcon::Information:QSystemTrayIcon::Warning);});});}
TrayController::~TrayController(){delete about_.data();tray_->hide();tray_->setContextMenu(nullptr);}
bool TrayController::available()const{return QSystemTrayIcon::isSystemTrayAvailable();}
void TrayController::activate(){showSettings();}
void TrayController::showSettings(){
 if(!settings_){settings_=&createSettings_();connect(settings_,&SettingsWindow::actionRequested,this,[this](const QString& action){operations_.invoke(action,[g=QPointer<TrayController>(this)](Error,QJsonObject){if(g)g->showActivity();});});}
 settings_->refreshDiagnostics();showWindow(*settings_);
}
void TrayController::showHistory(){if(!history_)history_=&createHistory_();history_->refresh();showWindow(*history_);}
void TrayController::showActivity(){if(!activity_)activity_=&createActivity_();activity_->refresh();showWindow(*activity_);}
void TrayController::showAbout(){
 if(!about_){
  const auto info=QJsonDocument::fromJson(buildManifestJson()).object();
  about_=new QMessageBox(QMessageBox::Information,"About put.gd",QString("put.gd %1").arg(buildVersion()),QMessageBox::Ok);
  about_->setAttribute(Qt::WA_DeleteOnClose);
  about_->setWindowModality(Qt::NonModal);
  about_->setInformativeText("Screen capture and file sharing");
  about_->setDetailedText(QString::fromUtf8(QJsonDocument(info).toJson(QJsonDocument::Indented)));
 }
 showWindow(*about_);
}
}
