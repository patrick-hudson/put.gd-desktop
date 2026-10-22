#pragma once
#include <QMainWindow>
#include <QJsonObject>
class QListWidget;class QStackedWidget;class QLabel;class QPushButton;class QMimeData;class QScrollArea;class QSpacerItem;
namespace putgd {
class UiSession;class SettingsWindow;class HistoryWindow;class FileDialog;
class MainWindow final:public QMainWindow {
 Q_OBJECT
public:
 explicit MainWindow(UiSession&,QWidget* parent=nullptr);
 ~MainWindow()override;
 void navigate(const QString&);
 // Asks for a PNG or JPEG to edit through the desktop's chooser (#259), then
 // emits editRequested; false while that chooser is already open.
 bool chooseImageToEdit();FileDialog& editImageDialog(){return *chooser_;}QJsonObject saveViewState()const;bool restoreViewState(const QJsonObject&);
 SettingsWindow* settings()const{return settings_;}HistoryWindow* history()const{return history_;}
 // The absolute path of a single dropped local PNG or JPEG file, else empty.
 static QString droppedImage(const QMimeData*);
signals:void viewStateChanged();void actionRequested(const QString&);
 // Edit & upload from a source other than a region: "clipboard" or "file" with a path.
 void editRequested(const QString& source,const QString& path);void quitRequested();
protected:
 bool eventFilter(QObject*,QEvent*)override;
private:
 void fitRail();void setRailCompact(bool);
 UiSession& session_;QPushButton* editButton_=nullptr;SettingsWindow* settings_;HistoryWindow* history_;QListWidget* navigation_;QStackedWidget* pages_;QLabel *destination_,*saveStatus_,*version_;QPushButton* versionLink_;QPushButton* updateLink_=nullptr;FileDialog* chooser_=nullptr;
 QScrollArea* railScroll_=nullptr;QList<QPushButton*> railButtons_,railLinks_;QList<QSpacerItem*> railGaps_;bool railCompact_=false,fittingRail_=false;int railSaving_=0;
};
}
