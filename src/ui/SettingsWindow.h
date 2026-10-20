#pragma once
#include "UiSession.h"
#include <functional>
#include "core/Settings.h"
#include "RetainedWindow.h"
#include <QHash>
#include <QJsonArray>
#include <QVector>
#include <QMap>
#include <QPointer>
#include <QSet>
#include <QPixmap>
class QAction;class QToolButton;class QTabWidget;class QListWidget;class QSpinBox;class QDoubleSpinBox;class QTimer;class QComboBox;class QLineEdit;class QCheckBox;class QLabel;class QPushButton;class QPlainTextEdit;class QDialog;
namespace putgd {
class ShortcutEditor;
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
 void showCaptureSettings();
 QWidget* destinationsPage()const{return destinationPage_;}
 void clearCredentialInputs();
 void refreshDiagnostics();
 void activateDestinations(bool forceRefresh=false);
 // Moves the selected (or the given) destination one place; saves right away.
 Error moveSelectedDestination(int delta,const QString& profileId,QJsonObject& result);
 // General → Diagnostics (#122): debug logging, Clear logs, the log and unclean-exit
 // status and Create diagnostics package. diagnosticsCommand drives them for agents
 // (ui.diagnostics); see docs/diagnostics.md.
 void diagnosticsCommand(const QJsonObject& params,std::function<void(Error,QJsonObject)> reply);
 QJsonObject diagnosticsState()const;
 void openDiagnosticsPackage(const QString& operationId={});
 // The two lines a destination row shows, as painted (elided, never wrapped).
 QStringList destinationRowLines(int row)const;
 // The image a drag of that row carries, and its hot spot (#133).
 QPixmap destinationDragImage(int row,QPoint* hotSpot=nullptr)const;
signals:
 void viewStateChanged();
 void actionRequested(const QString&);
 void quitRequested();
protected:
 void closeEvent(QCloseEvent*) override;
 bool eventFilter(QObject*,QEvent*)override;
private:
 template<class T>T findControl(const QString& name)const{auto value=findChild<T>(name);return value?value:destinationPage_->findChild<T>(name);}
 static qint64 displayUnit(const QString&);
 void queueCaptureEdit(QWidget*);void updateCaptureInheritance();
 QString destinationDefaultSharing(const QString& profileId)const;QJsonObject captureRoutingState()const;
 QComboBox *captureProfile_=nullptr,*captureSharing_=nullptr;
 QToolButton* captureExceptions_=nullptr;QHash<QString,QCheckBox*> useCaptureDefault_;QHash<QString,QWidget*> captureEditors_;
 bool captureDefaultsEnabled_=false,captureRoutingEdited_=false;
 QLabel* captureOutputNotice_=nullptr;QHash<QString,QLabel*> captureStatus_;
 bool editChangedSinceSubmission(const QString&)const;
 void stageEdit(const QString&,const QJsonValue&,QWidget*);
 void queueEdit(QWidget*,QString path={});void persistNextEdit();void refreshDestinationList(const QJsonObject& document={});void refreshCredentialHint();void inlineError(QWidget*,const QString&);void autosaveFailure(const Error&);
 QMap<QString,QJsonValue> pendingEdits_;QHash<QString,quint64> pendingEditSerials_,submittedEditSerials_;quint64 editSerial_=0;QHash<QString,QPointer<QWidget>> editControls_;
 QHash<QWidget*,QLabel*> errors_;QHash<QString,QJsonObject> testedDestinations_;QSet<QString> requestedFolders_;
 bool autosaveBlocked_=false;QVector<QPushButton*> retryAutosave_;
 QTimer* autosave_=nullptr;QPointer<QWidget> destinationPage_;QTabWidget* tabs_=nullptr;QListWidget* destinationList_=nullptr;
 QWidget *linksPanel_=nullptr,*connectionPanel_=nullptr;QPushButton* defaultButton_=nullptr;
 QLabel* credentialHint_=nullptr;
 QLabel *configPath_=nullptr,*endpointHint_=nullptr,*linkExample_=nullptr,*destinationStatus_=nullptr;
 void refreshEffectiveLimits();void updateLimitHints();QHash<QString,QLabel*> limitHints_;QJsonObject effectiveLimits_;
 void refreshLoggingStatus();void clearLogs(std::function<void(Error,QJsonObject)> done);void choosePackageFolder(const QString& folder);
 QCheckBox* debugLogging_=nullptr;QLabel *debugLoggingStatus_=nullptr,*logSummary_=nullptr,*loggingWarning_=nullptr,*uncleanExit_=nullptr;QPushButton* clearLogs_=nullptr;
 QPointer<QDialog> packageDialog_;
 void updateProtonAccountDisplay();bool applyProtonAccountResult(const QString&,const QJsonObject&);
 void beginSetup(const QString&,const QJsonObject& input={});void pollSetup();void updateSetupButton();void configurationChanged();
 void setupFeedback(const QString&,bool success=false);
 void populate();QJsonObject profileEditorState()const;void applyProfileEditor(const QJsonObject&);QJsonObject editedProfile()const;
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
 QJsonObject draft_,savingDocument_;QString observed_;bool loading_=false,dirty_=false,conflict_=false,initialProfile_=false;int profileIndex_=-1;
 QLabel *status_,*metadata_,*capabilities_;QPushButton *quit_;
 ShortcutEditor* shortcuts_;QHash<QString,QComboBox*> actionProfiles_;QHash<QString,QLineEdit*> folders_,s3_,numbers_,templates_;
 QComboBox *providerType_=nullptr,*protonFolder_=nullptr;QSpinBox* protonExpiration_=nullptr;QDoubleSpinBox* protonTimeout_=nullptr;
 QCheckBox* protonShare_=nullptr;QLineEdit* protonPassword_=nullptr;
 QWidget* protonPanel_=nullptr;QLabel *protonAccount_=nullptr,*protonFolderStatus_=nullptr;
 QHash<QString,QComboBox*> publicationModes_;
 QString protonAccountUid_,protonAccountEmail_;QSet<QString> folderRequests_;bool protonBrowserOpened_=false;
 std::function<void()> updateProviderVisibility_;
 QLineEdit *root_,*profileId_,*profileLabel_,*reference_,*access_,*secret_,*token_;QComboBox *profile_,*credentialMode_,*acl_,*naming_,*collision_,*addressing_,*regionChoices_,*uploadClipboard_;QPlainTextEdit* providerJson_;QCheckBox *keep_,*notifications_,*login_,*unlockAtStartup_;
};
}
