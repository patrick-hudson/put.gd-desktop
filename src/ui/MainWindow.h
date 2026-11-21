#pragma once
#include <QMainWindow>
#include <QJsonObject>
#include <QList>
#include <QHash>
#include <functional>
#include "FileDialog.h"
#include "core/Error.h"
class QListWidget;class QBoxLayout;class QHBoxLayout;class QToolButton;class QTabBar;class QStackedWidget;class QLabel;class QPushButton;class QMimeData;class QScrollArea;class QSpacerItem;
namespace putgd {
class UiSession;class SettingsWindow;class HistoryWindow;
class MainWindow final:public QMainWindow {
 Q_OBJECT
public:
 explicit MainWindow(UiSession&,QWidget* parent=nullptr);
 ~MainWindow()override;
 void navigate(const QString&);
 // Settings → Error log on one entry (#1753), and Destinations on one destination.
 Q_SLOT void showErrorLog(const QJsonObject& target);
 Q_SLOT void showDestination(const QString& profileId);
 Q_SLOT void showLocalCopySettings();void showCaptureSettings(const QStringList& actions={});void showActionFailure(const QString& action,const Error&);
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
 QString sourceGaps_; // the last capture-kind gaps seen, so statusChanged rebuilds the output popup only on change
 QHash<QString,QString> explanations_; // each action button's description
 void updateOutputSummary();void fitBand();void updateSaveStatus();void updateAvailability();void updateDelay();void refreshCommands();
 UiSession& session_;QToolButton* editButton_=nullptr;SettingsWindow* settings_=nullptr;HistoryWindow* history_=nullptr;QTabBar* tabs_;QStackedWidget* pages_;QToolButton* destination_=nullptr;QLabel *saveStatus_=nullptr,*version_=nullptr;QPushButton* versionLink_;QPushButton* updateLink_=nullptr;FileDialog* chooser_=nullptr;QLabel* explanation_=nullptr;QWidget* outputGroup_=nullptr;
 // Edit an image file… whose chooser failed: its reason and Try again, under the command band.
 QWidget* chooserFailure_=nullptr;QLabel* chooserError_=nullptr;
 QWidget* outputWarning_=nullptr;QLabel* outputWarningText_=nullptr;QWidget* band_=nullptr;QList<QToolButton*> actionButtons_;bool fittingBand_=false,statusFitPending_=false;
 // The command band's two groups, laid out in one row or wrapped into two (fitBand).
 QBoxLayout* bandBox_=nullptr;QHBoxLayout *firstRow_=nullptr,*secondRow_=nullptr;QSpacerItem* firstRowEnd_=nullptr;QWidget* divider_=nullptr;bool bandWrapped_=false;
 QToolButton* delay_=nullptr; // the status band's Capture delay control
};
}
