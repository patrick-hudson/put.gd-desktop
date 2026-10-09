#pragma once
#include "UiSession.h"
#include "core/Settings.h"
#include "RetainedWindow.h"
#include <QHash>
#include <QJsonArray>
class QTimer;class QComboBox;class QLineEdit;class QCheckBox;class QLabel;class QPushButton;class QPlainTextEdit;
namespace putgd {
class ShortcutEditor;
class SettingsWindow final:public RetainedWindow {
 Q_OBJECT
public:
 SettingsWindow(UiSession&,QWidget* parent=nullptr);
 QJsonObject saveViewState()const;
 bool restoreViewState(const QJsonObject&);
 void clearCredentialInputs();
 void refreshDiagnostics();
signals:
 void viewStateChanged();
 void actionRequested(const QString&);
 void quitRequested();
protected:
 void closeEvent(QCloseEvent*) override;
private:
 void beginSetup(const QString&,const QJsonObject& input={});void pollSetup();void configurationChanged();
 void populate();QJsonObject profileEditorState()const;void applyProfileEditor(const QJsonObject&);QJsonObject editedProfile()const;
 void refresh();void refreshMetadata();void dirty();void save();void addS3Profile();void cancel();void resetDraft(bool reloadFile);QJsonObject candidate() const;void loadProfile();void storeProfile();void rebuildProfiles(int selected,const QString& defaultProfile={});void status(const Error&);
 UiSession& settings_;
 QJsonArray setupJobs_;QTimer* setupTimer_=nullptr;bool setupPollActive_=false,saving_=false,resetting_=false;
 QJsonArray profileEditors_;int setupAccepting_=0;
 QString baseRevision_;quint64 editGeneration_=0;
 QJsonObject draft_;QString observed_;bool loading_=false,dirty_=false,conflict_=false,initialProfile_=false;int profileIndex_=-1;
 QLabel *status_,*metadata_,*capabilities_;QPushButton *save_,*quit_;
 ShortcutEditor* shortcuts_;QHash<QString,QComboBox*> destinations_,actionProfiles_;QHash<QString,QLineEdit*> folders_,s3_,numbers_,templates_;
 QLineEdit *root_,*profileId_,*profileLabel_,*reference_,*access_,*secret_,*token_;QComboBox *profile_,*credentialMode_,*acl_,*naming_,*collision_,*addressing_,*regionChoices_;QPlainTextEdit* providerJson_;QCheckBox *keep_,*notifications_,*login_;
};
}
