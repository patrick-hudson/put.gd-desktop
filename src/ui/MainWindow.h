#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include "FileDialog.h"
class QListWidget;class QToolButton;class QTabBar;class QStackedWidget;class QLabel;class QPushButton;class QMimeData;class QScrollArea;class QSpacerItem;
namespace putgd {
class UiSession;class SettingsWindow;class HistoryWindow;
class MainWindow final:public QMainWindow {
 Q_OBJECT
public:
 explicit MainWindow(UiSession&,QWidget* parent=nullptr);
 ~MainWindow()override;
 void navigate(const QString&);
 Q_SLOT void showLocalCopySettings();
 // Asks for a PNG or JPEG to edit through the desktop's chooser (#259), then
 // emits editRequested; false while that chooser is already open.
 bool chooseImageToEdit();FileDialog& editImageDialog(){return *chooser_;}
 // How that chooser ended: a chosen image opens in the editor, a cancel does
 // nothing, a failure shows under the command band with Try again.
 void imageChooserFinished(const FileDialog::Result&);QJsonObject saveViewState()const;bool restoreViewState(const QJsonObject&,bool restorePage=true);
 // settings() initializes on demand; passive lifecycle checks use existingSettings().
 SettingsWindow* settings();SettingsWindow* existingSettings()const{return settings_;}
 HistoryWindow* history()const{return history_;}
 // The absolute path of a single dropped local PNG or JPEG file, else empty.
 static QString droppedImage(const QMimeData*);
signals:void settingsCreated(SettingsWindow*);void viewStateChanged();void actionRequested(const QString&);
 // Edit & upload from a source other than a region: "clipboard" or "file" with a path.
 void editRequested(const QString& source,const QString& path);void quitRequested();void aboutRequested();
protected:
 bool eventFilter(QObject*,QEvent*)override;
private:
 void fitBand();void updateSaveStatus();
 UiSession& session_;QToolButton* editButton_=nullptr;SettingsWindow* settings_=nullptr;HistoryWindow* history_;QTabBar* tabs_;QStackedWidget* pages_;QLabel *destination_=nullptr,*saveStatus_=nullptr,*version_=nullptr;QPushButton* versionLink_;QPushButton* updateLink_=nullptr;FileDialog* chooser_=nullptr;QLabel* explanation_=nullptr;QWidget* outputGroup_=nullptr;
 // Edit an image file… whose chooser failed: its reason and Try again, under the command band.
 QWidget* chooserFailure_=nullptr;QLabel* chooserError_=nullptr;
 QWidget* band_=nullptr;QList<QToolButton*> actionButtons_;int bandStage_=0;bool fittingBand_=false;
};
}
