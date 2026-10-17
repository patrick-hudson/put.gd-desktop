#pragma once
#include <QMainWindow>
#include <QJsonObject>
class QListWidget;class QStackedWidget;class QLabel;
namespace putgd {
class UiSession;class SettingsWindow;class HistoryWindow;
class MainWindow final:public QMainWindow {
 Q_OBJECT
public:
 explicit MainWindow(UiSession&,QWidget* parent=nullptr);
 ~MainWindow()override;
 void navigate(const QString&);
 // Asks for a PNG or JPEG to edit, then emits editRequested.
 void chooseImageToEdit();QJsonObject saveViewState()const;bool restoreViewState(const QJsonObject&);
 SettingsWindow* settings()const{return settings_;}HistoryWindow* history()const{return history_;}
signals:void viewStateChanged();void actionRequested(const QString&);
 // Edit & upload from a source other than a region: "clipboard" or "file" with a path.
 void editRequested(const QString& source,const QString& path);void quitRequested();
private:
 UiSession& session_;SettingsWindow* settings_;HistoryWindow* history_;QListWidget* navigation_;QStackedWidget* pages_;QLabel *destination_,*saveStatus_;
};
}
