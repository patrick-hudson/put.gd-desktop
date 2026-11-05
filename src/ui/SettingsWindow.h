#pragma once
#include "UiSession.h"
#include <functional>
#include "core/Settings.h"
#include "RetainedWindow.h"
#include "FileDialog.h"
#include <QHash>
#include <QJsonArray>
#include <QVector>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QPixmap>
#include <QUrl>
class QAction;class QToolButton;class QTabWidget;class QListWidget;class QSpinBox;class QDoubleSpinBox;class QTimer;class QComboBox;class QLineEdit;class QCheckBox;class QLabel;class QPushButton;class QPlainTextEdit;class QDialog;class QFormLayout;class QGridLayout;class QProgressBar;
namespace putgd {
class ShortcutEditor;namespace Ui{class Banner;}
class SettingsWindow final:public RetainedWindow {
 Q_OBJECT
public:
 SettingsWindow(UiSession&,QWidget* parent=nullptr);
 ~SettingsWindow()override;
 QJsonObject saveViewState()const;
 bool restoreViewState(const QJsonObject&);
 bool flushAutosave();
 Error actionSaveError()const;bool hasCredentialInputs()const;
 QString autosaveText()const;
 // A conflict or a refused save: edits wait in this window, not in the file.
 bool changesUnsaved()const{return conflict_||autosaveBlocked_;}
 void showCaptureSettings(const QStringList& affectedActions={});void showLocalCopySettings();
 // General → Updates (#293), scrolled into view.
 void showUpdates();
 QWidget* destinationsPage()const{return destinationPage_;}
 void clearCredentialInputs();
 void retryProtonBrowser();
 void protonBrowserFinished(const QString& setupId,const QJsonObject& result);
 void refreshDiagnostics();
 void activateDestinations(bool forceRefresh=false);
 // A finished, non-error message stays with the page it is about: leaving
 // that page clears it; errors and work still in progress stay.
 void clearCompletedFeedback();
 // Moves the selected (or the given) destination one place; saves right away.
 Error moveSelectedDestination(int delta,const QString& profileId,QJsonObject& result);
 // Advanced → Diagnostics (#122; on General until #238): debug logging, Clear
 // logs, the log and unclean-exit status and Create diagnostics package. diagnosticsCommand drives them for agents
 // (ui.diagnostics); see docs/diagnostics.md.
 void diagnosticsCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 QJsonObject diagnosticsState()const;
 // #259: Capture → Local copies' Browse… buttons (dialog outputFolder,
 // field root|images|videos|text|other) and the package dialog's Choose folder…
 // (diagnosticsFolder): action open|inspect|cancel, answered with the chooser's state.
 void fileDialogCommand(const QString& dialog,const QString& action,const QString& field,std::function<void(Error,QJsonObject)> reply);
 // How those choosers ended, as their buttons handle it: a chosen folder is
 // used, a cancel changes nothing, a failure shows its reason beside the
 // originating row with Try again (tests drive failures through here).
 void folderChooserFinished(const QString& dialog,const QString& field,const FileDialog::Result&);
 void openDiagnosticsPackage(const QString& operationId={});
 // The painted destination name (up to two lines) and its separate status.
 QStringList destinationRowLines(int row)const;
 // The image a drag of that row carries, and its hot spot (#133).
 QPixmap destinationDragImage(int row,QPoint* hotSpot=nullptr)const;
signals:
 void viewStateChanged();
 void actionRequested(const QString&);
 void quitRequested();
 // Capture's "Add a destination…" when none exists: the main window shows Destinations.
 void destinationsRequested();
 // #237: the theme picked in Appearance, shown before the resident accepts it;
 // empty when the accepted setting applies again (a refused or discarded save).
 void appearancePreview(const QString& theme);
protected:
 void closeEvent(QCloseEvent*) override;
 bool eventFilter(QObject*,QEvent*)override;
private:
 template<class T>T findControl(const QString& name)const{auto value=findChild<T>(name);return value?value:destinationPage_->findChild<T>(name);}
 static qint64 displayUnit(const QString&);
 bool chooseOutputFolder(QLineEdit* field);void outputFolderChosen(QLineEdit* field,const FileDialog::Result&);
 FileDialog* folderChooser_=nullptr;QHash<QLineEdit*,QWidget*> chooserFailures_;
 void queueCaptureEdit(QWidget*);void updateCaptureInheritance();
 QString destinationDefaultSharing(const QString& profileId)const;QJsonObject captureRoutingState()const;QString outputSummary(QComboBox* profile,QComboBox* sharing)const;
 QComboBox *captureProfile_=nullptr,*captureSharing_=nullptr;
 QHash<QString,bool> credentialAvailability_;QStringList outputRecoveryActions_;QGridLayout* captureExceptionRows_=nullptr;
 QToolButton* captureExceptions_=nullptr;QHash<QString,QCheckBox*> useCaptureDefault_;QHash<QString,QWidget*> captureEditors_;
 bool captureDefaultsEnabled_=false,captureRoutingEdited_=false,recordingRoutingEdited_=false;
 QLabel* captureOutputNotice_=nullptr;QHash<QString,QLabel*> captureStatus_,captureSummary_;QHash<QString,QCheckBox*> captureOverride_;
 // The no-output notice under Destination, with Keep a local copy and Choose/Add a destination.
 QWidget* captureOutputBlock_=nullptr;QPushButton* chooseOutput_=nullptr;
 bool editChangedSinceSubmission(const QString&)const;
 void stageEdit(const QString&,const QJsonValue&,QWidget*);
 void queueEdit(QWidget*,QString path={});void persistNextEdit();void refreshDestinationList(const QJsonObject& document={});void refreshCredentialHint();void inlineError(QWidget*,const QString&);void autosaveFailure(const Error&);
 QMap<QString,QJsonValue> pendingEdits_;QHash<QString,quint64> pendingEditSerials_,submittedEditSerials_;quint64 editSerial_=0;QHash<QString,QPointer<QWidget>> editControls_;
 QHash<QWidget*,QLabel*> errors_;QHash<QString,QJsonObject> testedDestinations_;QSet<QString> requestedFolders_;
 bool autosaveBlocked_=false;QVector<QPushButton*> retryAutosave_;
 // Reload from file…, beside the conflict on Settings and on Destinations, and
 // in Advanced → Config file: one confirmation naming the unsaved edits it
 // discards, then the same reload (resetDraft(true)).
 QVector<QPushButton*> reloadConflict_;QVector<Ui::Banner*> conflictBanners_;void reloadFromFile();QStringList draftEdits()const;void showConflictActions();void placeConflictBanners();static QString conflictMessage();
 // The accepted document the draft started from, to tell its edits apart
 // from changes another writer made.
 QJsonObject baseDocument_;
 // Sidebars as wide as their labels need, between the baseline and the cap.
 void fitRails();QWidget *settingsRail_=nullptr,*destinationRail_=nullptr;QListWidget* settingsNavigation_=nullptr;QPushButton* addDestination_=nullptr;
 QTimer* autosave_=nullptr;QPointer<QWidget> destinationPage_;QTabWidget* tabs_=nullptr;QListWidget* destinationList_=nullptr;
 QWidget *linksPanel_=nullptr,*connectionPanel_=nullptr;QPushButton* defaultButton_=nullptr;
 QPushButton* credentialStoreButton_=nullptr;
 QLabel *credentialStoreReason_=nullptr,*passwordStoreReason_=nullptr,*credentialStorageNotice_=nullptr;
 QLabel* credentialHint_=nullptr;QFormLayout* credentialForm_=nullptr;
 QLabel *configPath_=nullptr,*endpointHint_=nullptr,*linkExample_=nullptr,*destinationStatus_=nullptr;
 QToolButton* resourceLimitsToggle_=nullptr;QWidget* resourceLimits_=nullptr;QLabel* uploadReserveHint_=nullptr;void updateResourceLimits();void refreshEffectiveLimits();void updateLimitHints();QHash<QString,QLabel*> limitHints_;QJsonObject effectiveLimits_;
 void refreshUpdateStatus();void checkForUpdates(std::function<void(Error,QJsonObject)> done);
 // The Channel choice (#344): Stable or Nightly, showing this build's own
 // channel while the stored setting is "build" (the default); the build's
 // channel, empty when it never checks, and the choice as shown at load.
 QString buildUpdateChannel() const;void showUpdateChannel();QString updateChannelSetting_,updateChannelShown_;
 // #348: set by showUpdates until the user scrolls or leaves the page.
 void revealUpdates();bool revealUpdates_=false;void revealAndFocus(QWidget*);QWidget *generalPage_=nullptr,*updatesCard_=nullptr;
 QCheckBox* checkUpdates_=nullptr;QComboBox* updateChannel_=nullptr;QLabel *updateStatus_=nullptr,*updatesIntro_=nullptr;QPushButton *checkNow_=nullptr,*downloadUpdate_=nullptr,*releaseNotes_=nullptr,*openUpdates_=nullptr;QWidget *updateActionsRow_=nullptr,*updateCommandRow_=nullptr;QLineEdit* updateCommand_=nullptr;QFormLayout* updatesForm_=nullptr;QList<QWidget*> updateRows_;
 // AppImage install (#293 phase 3): Download and install, its progress and Restart.
 QLabel *updateChannelSource_=nullptr,*otherChannelText_=nullptr,*otherChannelCommands_=nullptr;QWidget* otherChannelRow_=nullptr;
 QPushButton *installUpdate_=nullptr,*restartUpdate_=nullptr;QProgressBar* installProgress_=nullptr;QLabel* installStatus_=nullptr;QWidget* installRow_=nullptr;void refreshInstallStatus(const QJsonObject& update,bool available,bool managed);
 void refreshLoggingStatus();void clearLogs(std::function<void(Error,QJsonObject)> done);void choosePackageFolder(const QString& folder);
 QCheckBox* debugLogging_=nullptr;QLabel *debugLoggingStatus_=nullptr,*logSummary_=nullptr,*loggingWarning_=nullptr,*uncleanExit_=nullptr;QPushButton* clearLogs_=nullptr;
 QPointer<QDialog> packageDialog_;
 void updateProtonAccountDisplay();void sizeProtonAccountButtons();bool applyProtonAccountResult(const QString&,const QJsonObject&);
 QString setupBlockedReason()const;bool admitSetup();
 void beginSetup(const QString&,const QJsonObject& input={});void pollSetup();void updateSetupButton();void updateStoreButtons();void configurationChanged();
 // A control's own (provider) enabled state; setup in progress disables it on top.
 void allowControl(QWidget*,bool allowed);bool setupBusy_=false;
 void setupFeedback(const QString&,bool success=false);
 void populate();QJsonObject profileEditorState()const;void applyProfileEditor(const QJsonObject&);QJsonObject editedProfile()const;
 void endAppearancePreview();
 void refresh(bool preserveDrafts=false);void refreshMetadata();void dirty();void save();void addS3Profile(const QString& defaultLabel="S3 destination");void resetDraft(bool reloadFile);QJsonObject candidate() const;bool selectedDestinationChanged()const;void loadProfile();void storeProfile();void rebuildProfiles(int selected,const QString& defaultProfile={});void status(const Error&);
 void switchProvider();void importDevelopmentProfile();
 QStringList profileOrder()const;void applyProfileOrder(const QStringList&);Error moveDestination(int from,int to);void restoreReorderBaseline();
 QStringList reorderBaseline_;QAction *moveUp_=nullptr,*moveDown_=nullptr;
 QJsonObject retainedProfileEditor()const;bool hasRetainedDrafts()const;
 void rememberCredentialInputs();void restoreCredentialInputs();void clearCurrentCredentialInputs();
 void toggleCredentialSecret(bool shown);void resetCredentialSecretDisplay();
 QPushButton* showSecret_=nullptr;bool savedSecretShown_=false;quint64 secretRevealSerial_=0;
 struct CredentialInputs {QString access,secret,token,password;};
 QVector<QHash<QString,CredentialInputs>> credentialDrafts_;
 QHash<QString,CredentialInputs> provisionalCredentials_;
 QJsonObject provisionalProfile_;
 UiSession& settings_;
 QJsonArray setupJobs_;QTimer* setupTimer_=nullptr;bool setupPollActive_=false,testAccepting_=false,saving_=false,resetting_=false;
 QJsonArray profileEditors_;int setupAccepting_=0;
 QString baseRevision_;
 QString metadataText_; // Canonical metadata; display-only wrap characters never enter diagnostics copies.
 QJsonObject draft_,savingDocument_;QString observed_;bool loading_=false,appearancePreviewed_=false,dirty_=false,conflict_=false,initialProfile_=false;int profileIndex_=-1;
 QLabel *status_=nullptr,*metadata_=nullptr;QWidget* capabilities_=nullptr;
 ShortcutEditor* shortcuts_;QHash<QString,QComboBox*> actionProfiles_;QHash<QString,QLineEdit*> folders_,s3_,numbers_,templates_;
 QComboBox *providerType_=nullptr,*protonFolder_=nullptr;QSpinBox* protonExpiration_=nullptr;QDoubleSpinBox* protonTimeout_=nullptr;
 QComboBox* b2LinkMode_=nullptr;QSpinBox* b2LinkDays_=nullptr;
 QCheckBox* protonShare_=nullptr;QLineEdit* protonPassword_=nullptr;
 QWidget* protonPanel_=nullptr;QLabel *protonAccount_=nullptr,*protonFolderStatus_=nullptr;
 QHash<QString,QComboBox*> publicationModes_;
 QUrl pendingProtonUrl_;QString pendingProtonSetup_;QPushButton* protonBrowserRetry_=nullptr;bool protonBrowserOpening_=false;
 QString protonAccountUid_,protonAccountEmail_;QSet<QString> folderRequests_;bool protonBrowserOpened_=false;
 std::function<void()> updateProviderVisibility_;
 QLineEdit *root_,*profileId_,*profileLabel_,*reference_,*access_,*secret_,*token_;QComboBox *profile_,*credentialMode_,*acl_,*naming_,*collision_,*addressing_,*regionChoices_,*regionSelection_,*recordingFrameRate_,*uploadClipboard_,*appearance_;QPlainTextEdit* providerJson_;QCheckBox *keep_,*notifications_,*login_,*unlockAtStartup_,*fileManagerMenu_;QComboBox* fileManagerFolders_;
};
}
