#pragma once
#include "UiSession.h"
#include <functional>
#include "core/Settings.h"
#include "RetainedWindow.h"
#include "FileDialog.h"
#include <QElapsedTimer>
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
class ShortcutEditor;class CustomUploaderPanel;class DavDestinationPanel;class SftpPanel;class ErrorLogPage;namespace Ui{class Banner;}
class SettingsWindow final:public RetainedWindow {
 Q_OBJECT
public:
 // Single source of truth for destination-type labels, groups and menu order (#2088).
 struct DestinationType {
  QString id,label,defaultName,addActionName,group;
 };
 static const QList<DestinationType>& destinationTypes();
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
 // #2118: the shown destination's Encrypted links card, its Test upload and link focused when offered.
 void showEncryptedLinks();
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
 // (ui.diagnostics); see docs/design/diagnostics.md.
 void diagnosticsCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 QJsonObject diagnosticsState()const;
 // #259: Capture → Local copies' Browse… buttons (dialog outputFolder,
 // field root|images|videos|text|other) and the package dialog's Choose folder…
 // (diagnosticsFolder): action open|inspect|cancel, answered with the chooser's state.
 void fileDialogCommand(const QString& dialog,const QString& action,const QString& field,std::function<void(Error,QJsonObject)> reply);
 // #1361: drives the custom uploader form the way a user does: action inspect,
 // import (path: the .sxcu file is read into the form; its secrets wait there
 // until stored), store (the Save to keychain button) or test.
 void uploaderCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 // ui.r2Destination (#1678): the Cloudflare R2 form, edited, pasted into and moved to as a user does.
 void r2Command(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 // ui.davDestination (#1872): the Nextcloud and WebDAV forms, edited and signed in as a user does.
 void davCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 // ui.encryptedLinks (#2079): the selected destination's Encrypted links card, as a user drives it. A mode
 // change answers once the file has it, or with settings_not_saved once it can't be (#2103).
 void encryptedLinksCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 // ui.sftpDestination (#1871): the SFTP form, edited, pasted into, tested and trusted as a user does.
 void sftpCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 // How those choosers ended, as their buttons handle it: a chosen folder is
 // used, a cancel changes nothing, a failure shows its reason beside the
 // originating row with Try again (tests drive failures through here).
 void folderChooserFinished(const QString& dialog,const QString& field,const FileDialog::Result&);
 void openDiagnosticsPackage(const QString& operationId={});
 // Settings → Error log (#1753), on one entry: {id}, or the newest about {historyId|operationId, kind}.
 void showErrorLog(const QJsonObject& target={});
 // ui.errorLog: the page's controls, driven the way a user does.
 void errorLogCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 ErrorLogPage* errorLog()const{return errorLogPage_;}
 // Selects that destination in the list as Destinations shows it (the draft's order); false when it isn't there.
 bool selectDestination(const QString& profileId);
signals:
 void viewStateChanged();
 void actionRequested(const QString&);
 void quitRequested();
 // Capture's "Add a destination…" when none exists: the main window shows Destinations.
 void destinationsRequested();
 // The Error log's Open destination settings and Show in History (#1753).
 void destinationSettingsRequested(const QString& profileId);
 void historyItemRequested(const QString& historyId);
 // #237: the theme picked in Appearance, shown before the resident accepts it;
 // empty when the accepted setting applies again (a refused or discarded save).
 void appearancePreview(const QString& theme);
protected:
 void closeEvent(QCloseEvent*) override;
 bool eventFilter(QObject*,QEvent*)override;
 void showEvent(QShowEvent*)override;
private:
 template<class T>T findControl(const QString& name)const{auto value=findChild<T>(name);return value?value:destinationPage_->findChild<T>(name);}
 static qint64 displayUnit(const QString&);
 bool chooseOutputFolder(QLineEdit* field);void outputFolderChosen(QLineEdit* field,const FileDialog::Result&);
 FileDialog* folderChooser_=nullptr;QHash<QLineEdit*,QWidget*> chooserFailures_;
 void queueCaptureEdit(QWidget*);void updateCaptureInheritance();
 QString destinationDefaultSharing(const QString& profileId)const;QJsonObject captureRoutingState()const;QString outputSummary(QComboBox* profile,QComboBox* sharing)const;
 QComboBox *captureProfile_=nullptr,*captureSharing_=nullptr;
 // History items uploaded to each destination, counted when it is selected (#1759).
 QHash<QString,int> uploadedCounts_;void refreshUploadedCount();QHash<QString,quint64> countRequests_;quint64 countSerial_=0;
 QHash<QString,bool> credentialAvailability_;QStringList outputRecoveryActions_;QGridLayout* captureExceptionRows_=nullptr;
 QToolButton* captureExceptions_=nullptr;QHash<QString,QCheckBox*> useCaptureDefault_;QHash<QString,QWidget*> captureEditors_;
 bool captureDefaultsEnabled_=false,captureRoutingEdited_=false,recordingRoutingEdited_=false;
 QLabel* captureOutputNotice_=nullptr;QHash<QString,QLabel*> captureStatus_,captureSummary_;QHash<QString,QCheckBox*> captureOverride_;
 // The no-output notice under Destination, with Keep a local copy and Choose/Add a destination.
 QWidget* captureOutputBlock_=nullptr;QPushButton* chooseOutput_=nullptr;
 // #1704: one queue of save operations, one request in flight. Each operation is applied to the
 // latest accepted document when it is sent, so an edit never carries another edit's stale copy
 // and a destination that can't be saved never blocks another one.
 struct SaveOperation {
  enum class Kind{Fields,Remove,Order,Global};
  Kind kind=Kind::Global;
  QString key,id; // "destination:<id>", "remove:<id>", "order" or the global key; the destination's ID
  // Fields: paths inside the destination ("label", "settings/region"). Remove and Global: document
  // paths ("/preferences/notifications"). An undefined value removes the key.
  QMap<QString,QJsonValue> values;
  QJsonObject profile; // Fields: the destination as the draft has it, inserted when it isn't accepted yet
  QStringList stripped; // Fields: groups refused here, saved with their accepted or default values
  QStringList order;    // Order: destination IDs in their intended order
  QHash<QString,QPointer<QWidget>> controls; // the control that made each value, for its inline message
  // Fields: the destination wasn't accepted when this was queued, so it is added; an edit of one
  // that is gone is dropped, and an add whose ID was taken meanwhile gets a fresh one. Remove: it
  // removes an add that was in flight, and goes only if that add was saved.
  bool adding=false;
  QStringList probing;  // Fields: after a refusal that names no field, the groups still to send one at a time
  QStringList promoted; // Fields: groups moved to the front of probing once, so two refused groups never swap forever
  bool reconciling=false; // Remove: its add's answer was lost; decided only once the file has been read again
 };
 QList<SaveOperation> saveQueue_;SaveOperation sending_;
 // #2103: ui.encryptedLinks mode changes waiting for their save, each answered by settleEncryptWaits.
 // confirmed: a snapshot read after the call, or a failure to read one (confirmError), so a window
 // that hasn't heard of a change made elsewhere never answers from its stale copy.
 struct EncryptWait {quint64 serial=0;QString profileId,mode;std::function<void(Error,QJsonObject)> reply;bool confirmed=false;Error confirmError;};
 QList<EncryptWait> encryptWaits_;quint64 encryptWaitSerial_=0;void settleEncryptWaits(quint64 expired=0);
 QString savedEncryptMode(const QString& profileId)const;QJsonObject encryptState()const;
 // Unsaved destination groups and settings, "<id>|<group>" or "|<path>", with the reason.
 QMap<QString,QString> notSaved_;
 QPointer<QWidget> typingControl_;
 void queueEdit(QWidget*,QString path={});void queueDestinationEdit(QWidget*,const QString& path);void queueWholeDestination(const QString& id,QWidget*);void queueGlobal(const QString& key,const QStringList& paths,QWidget*);void queueOrder();void queueRemoval(const QString& id);QStringList routingPaths()const;
 void enqueue(SaveOperation);void requeue(SaveOperation);SaveOperation merged(SaveOperation older,const SaveOperation& newer)const;void checkQueue();bool acceptedHas(const QString& id)const;bool waitsForDestination(const SaveOperation&,const QJsonObject& accepted)const;bool meetsPreconditions(SaveOperation&);void renameAddedDestination(const QString& from,const QString& to);void queueRouting(QWidget*,const QString& onlyFor={});static QString routeOf(const QString& path);void strip(SaveOperation&,const QString& group)const;void retrySaving();QStringList removedElsewhere_;QPointer<QWidget> pasting_;bool restoredDraft_=false;QJsonObject restoredBase_;void drainSaves();void operationSettled(const SaveOperation&);void saveAnswered(SaveOperation,const Result&,const QJsonObject& sent,const QJsonObject& restoredSent);void saveRefused(SaveOperation,const Error&,const QJsonObject& sent);void showUnsavedDestinations();void settleDirty();void retryAfterConflict();void queueDraftDifferences(const QJsonObject& base,bool withOrder=true);bool replayRestoredDraft();void reconciled();void dropRoutesTo(const QString& id,const QString& reason={});static bool setsInheritance(const SaveOperation& op);bool refusedAdd(const QString& id)const;bool removalInFlight(const QString& id)const;bool answered_=false;
 QJsonObject acceptedDocument()const;QJsonObject applyOperation(QJsonObject,const SaveOperation&)const;QMap<QString,QJsonValue> sendableValues(const SaveOperation&,const QJsonObject& accepted,QJsonObject* insertion)const;QJsonValue fallbackValue(const SaveOperation&,const QJsonObject& accepted,const QString& path)const;
 QString localProblem(const QJsonObject& profile,const QStringList& group)const;bool savePending()const{return saving_||!saveQueue_.isEmpty();}bool queued(const QString& key)const;void flushTyping();void refreshDestinationList(const QJsonObject& document={});void refreshCredentialHint();void inlineError(QWidget*,const QString&);bool updateRegionFeedback(bool clearOnly=false);void showFileError();bool formOwnsFeedback(QWidget*)const;void showS3SettingsProblems();void autosaveFailure(const Error&);
 // The control a settings key is edited in, for a field error.
 QWidget* settingsField(const QString& field)const;
 // Move to Cloudflare R2 (#1678): converts the selected S3 destination in place, one operation.
 Error moveToCloudflareR2();
 // Destinations whose service was switched in this window, by ID, and the service they were switched
 // to: the mark ends only when the file has that service.
 QHash<QString,QString> serviceSwitched_;
 // Cloudflare R2's field messages, the line under Bucket and its status line (#1699), from the shown
 // destination's current values and the queue's record of what isn't saved.
 void updateR2Feedback();
 QHash<QWidget*,QLabel*> errors_;QSet<QString> requestedFolders_;
 bool autosaveBlocked_=false;QVector<QPushButton*> retryAutosave_;
 // Reload from file…, beside the conflict on Settings and on Destinations, and
 // in Advanced → Config file: one confirmation naming the unsaved edits it
 // discards, then the same reload (resetDraft(true)).
 QVector<QPushButton*> reloadConflict_;QVector<Ui::Banner*> conflictBanners_;void reloadFromFile();QStringList draftEdits()const;void showConflictActions();void placeConflictBanners();static QString conflictMessage();
 // The accepted document the draft started from, to tell its edits apart
 // from changes another writer made.
 QJsonObject baseDocument_;
 // Sidebars as wide as their labels need, between the baseline and the cap.
 ErrorLogPage* errorLogPage_=nullptr;
 void fitRails();QWidget *settingsRail_=nullptr,*destinationRail_=nullptr;QListWidget* settingsNavigation_=nullptr;QPushButton* addDestination_=nullptr;
 QTimer* typingPause_=nullptr;QPointer<QWidget> destinationPage_;QTabWidget* tabs_=nullptr;QListWidget* destinationList_=nullptr;
 QWidget *linksPanel_=nullptr,*connectionPanel_=nullptr;QPushButton* defaultButton_=nullptr;
 QPushButton* credentialStoreButton_=nullptr;
 QLabel *credentialStoreReason_=nullptr,*passwordStoreReason_=nullptr,*credentialStorageNotice_=nullptr;
 QLabel* credentialHint_=nullptr;QFormLayout* credentialForm_=nullptr;
 QLabel *configPath_=nullptr,*endpointHint_=nullptr,*linkExample_=nullptr,*destinationStatus_=nullptr,*fileError_=nullptr,*destinationFileError_=nullptr;
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
 // The resident's record of a destination's last check (#1692), for the connection in
 // `profile`: its state (passed, failed or untested) and the row text that says it. An
 // unsaved connection edit is untested at once; a label edit is not.
 QString checkState(const QJsonObject& profile)const;
 QString checkText(const QJsonObject& profile,const QString& passed,const QString& testFailed,const QString& uploadFailed,const QString& untested)const;
 void updateProtonAccountDisplay();void sizeProtonAccountButtons();bool applyProtonAccountResult(const QString&,const QJsonObject&,bool write);
 QString setupBlockedReason()const;bool admitSetup();
 void beginSetup(const QString&,const QJsonObject& input={});void pollSetup();void updateSetupButton();void updateStoreButtons();void configurationChanged();
 // A control's own (provider) enabled state; setup in progress disables it on top.
 void allowControl(QWidget*,bool allowed);bool setupBusy_=false;
 void setupFeedback(const QString&,bool success=false);
 void populate();QJsonObject profileEditorState()const;void applyProfileEditor(const QJsonObject&);QJsonObject editedProfile()const;
 void endAppearancePreview();
 void refresh(bool preserveDrafts=false);void refreshMetadata();void dirty();void addS3Profile(const QString& defaultLabel="S3 destination",const QString& name={},const QString& provider={});void resetDraft(bool reloadFile);QJsonObject candidate() const;bool selectedDestinationChanged()const;void loadProfile();void storeProfile();void rebuildProfiles(int selected,const QString& defaultProfile={},bool keepEditor=false);void status(const Error&);
 void switchProvider();void importDevelopmentProfile();
 QStringList profileOrder()const;void applyProfileOrder(const QStringList&);Error moveDestination(int from,int to);void restoreReorderBaseline();void showFileOrder();
 bool reorderPending_=false; // a move not saved yet, which a conflict or refusal puts back
 QAction *moveUp_=nullptr,*moveDown_=nullptr;
 QJsonObject retainedProfileEditor()const;bool hasRetainedDrafts()const;
 void rememberCredentialInputs();void restoreCredentialInputs();void clearCurrentCredentialInputs();
 void toggleCredentialSecret(bool shown);void resetCredentialSecretDisplay();
 QPushButton* showSecret_=nullptr;bool savedSecretShown_=false;quint64 secretRevealSerial_=0;
 // values: a custom uploader's typed secret values by credential-store key (#1361).
 struct CredentialInputs {QString access,secret,token,password;QMap<QString,QString> values={};bool empty()const{return access.isEmpty()&&secret.isEmpty()&&token.isEmpty()&&password.isEmpty()&&values.isEmpty();}};
 // Settings → Destinations → Custom uploader (HTTP): its form, and ui.customUploader
 // for agents (inspect, import a .sxcu file into the form, open or close its dialog).
 CustomUploaderPanel* uploader_=nullptr;
 // Nextcloud and WebDAV (#1872): the panel, the running sign-in job, and its steps (SettingsDav.cpp).
 DavDestinationPanel* dav_=nullptr;QString davSignInSetup_,davRemoving_,davAdoptedSetup_;bool davCancelPending_=false;
 // Remove destination for Nextcloud: sign out (revoke) first, say whether the server confirmed it,
 // then remove.
 void revokeBeforeRemoving(const QString& profileId);
 void davSignInEvent(const QString& setupId,const QUrl& url);void openDavBrowser();void cancelDavSignIn();
 // A finished setup job of a Nextcloud or WebDAV destination; true when it was handled whole.
 bool davSetupFinished(const QJsonObject& job,const QJsonObject& data,const Error& error,bool currentTest);
 // #1871: the SFTP form; inspectProfile_ is the destination whose ssh -G line is being read.
 SftpPanel* sftpPanel_=nullptr;class EncryptedLinksPanel* encryptPanel_=nullptr;void loadEncryptPanel(bool edited);QString revealEncryptFor_;void revealEncryptedLinks();QString sftpInspected_;void inspectSftp();QString sftpInspectKey()const;
 // #1871: a Clear about to start; its setup job record carries the mark from then on.
 bool sftpClearPending_=false;
 QVector<QHash<QString,CredentialInputs>> credentialDrafts_;
 QHash<QString,CredentialInputs> provisionalCredentials_;
 QJsonObject provisionalProfile_;
 UiSession& settings_;
 QJsonArray setupJobs_;QTimer* setupTimer_=nullptr;bool setupPollActive_=false,testAccepting_=false,saving_=false,resetting_=false;
 QJsonArray profileEditors_;int setupAccepting_=0;
 QString baseRevision_;
 QString metadataText_; // Canonical metadata; display-only wrap characters never enter diagnostics copies.
 QJsonObject draft_;QString observed_;bool loading_=false,appearancePreviewed_=false,dirty_=false,conflict_=false,initialProfile_=false;int profileIndex_=-1;
 // Cloudflare R2's Account ID and Jurisdiction (#1678), and the line saying Account ID, Jurisdiction
 // and Bucket aren't saved.
 QLineEdit* r2AccountId_=nullptr;QComboBox* r2Jurisdiction_=nullptr;QLabel* r2UnitStatus_=nullptr;
 // By destination ID, the account and jurisdiction ("account|jurisdiction") its Bucket came with:
 // first loaded, then pasted or typed beside. An account-only paste for another account empties Bucket.
 QHash<QString,QString> r2BucketFrom_;
 // Another S3-compatible service's region and endpoint, kept while Amazon S3 is picked (#1699).
 QPair<QString,QString> s3OtherService_;
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
 QSpinBox* scrollHeight_; QCheckBox* scrollFromTop_; QCheckBox* scrollAutoScroll_;
 // #1729: Random names style; the rows show only while Random names is selected.
 QComboBox* randomStyle_=nullptr;QSpinBox* randomLength_=nullptr;QCheckBox *randomUppercase_=nullptr,*randomDigits_=nullptr;QFormLayout* namingForm_=nullptr;void showRandomNameRows();
 QLineEdit *root_,*profileId_,*profileLabel_,*reference_,*access_,*secret_,*token_;QComboBox *profile_,*credentialMode_,*acl_,*naming_,*collision_,*addressing_,*regionChoices_,*regionSelection_,*captureDelay_,*recordingFrameRate_,*recordingMicrophone_,*uploadClipboard_,*appearance_;QPlainTextEdit* providerJson_;QCheckBox *keep_,*notifications_,*login_,*unlockAtStartup_,*fileManagerMenu_,*recordingClicks_,*captureDelayShortcuts_,*unhinged_;
 // #1364: Capture → Screen recording's microphone choice and click highlights
 // ask the resident what the sound server and the desktop offer right now.
 // force: ask now (the page was just shown, or the device choice changed); otherwise at most every
 // recordingInputsEveryMs and only while the page is showing, because each ask starts the recording helper.
 void refreshRecordingInputs(bool force=false);
 // Settings → Capture → Text recognition (#1428): the installed languages from
 // the resident's capabilities, the saved choice, and why recognition is off.
 void showTextLanguages();
 QList<QComboBox*> textLanguages_;QLabel* textRecognitionStatus_=nullptr;QJsonObject textCapabilities_;
 // #1618: Quick redact's saved words (one per line) and regular expressions (one per line).
 QPlainTextEdit *quickRedactTerms_=nullptr,*quickRedactPatterns_=nullptr;
 // #1804: Check captures for sensitive text before uploading, and the Quick redact categories it looks for.
 // #1893: Capture style. styleDraft_ is what the controls show; the section's rows edit it.
 CaptureStyle styleDraft_;QCheckBox *styleEnabled_=nullptr,*styleCorners_=nullptr,*styleShadow_=nullptr;QComboBox *styleLook_=nullptr,*styleBackdrop_=nullptr,*stylePadding_=nullptr;
 QToolButton *styleColor_=nullptr,*styleColorEnd_=nullptr;QPushButton* stylePicture_=nullptr;QLabel *stylePictureName_=nullptr,*stylePreview_=nullptr,*styleSize_=nullptr;
 QList<QWidget*> styleRows_,styleCustomRows_;QWidget *styleColorsRow_=nullptr,*stylePictureRow_=nullptr;bool styleCustom_=false;FileDialog* pictureChooser_=nullptr;
 void syncCaptureStyle();void editCaptureStyle(const std::function<void(CaptureStyle&)>& change);void chooseStylePicture();void pickStyleColor(bool end);
 void renderStylePreview();
public:
 // The preview's synthetic sample at 1200 × 750, and the capture style around it; public for the UI catalog.
 static QImage captureStyleSample(const CaptureStyle& style);
private:
 QCheckBox* sensitiveCheck_=nullptr;QHash<QString,QComboBox*> sensitiveKinds_;QToolButton* sensitiveApplyAll_=nullptr;void showSensitiveCheck(); // #1820: Off, Warn or Redact automatically per kind
public:
 // What `recording.microphones` and the capability's `clickHighlights` answered; public for the UI catalog.
 void showRecordingMicrophones(const QJsonObject& microphones);void showClickHighlights(const QJsonObject& capability);
private:
 QLabel *recordingMicrophoneStatus_=nullptr,*recordingClicksStatus_=nullptr;
 QElapsedTimer recordingInputsAsked_;bool clickHighlightsAsked_=false;QJsonArray recordingDevices_;bool recordingDevicesShown_=false;
 static constexpr int recordingInputsEveryMs=10000;QComboBox* fileManagerFolders_;
};
}
