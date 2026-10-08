#pragma once
#include "core/SettingsService.h"
#include "secrets/CredentialStore.h"
#include <QWidget>
#include <QHash>
class QComboBox;class QLineEdit;class QCheckBox;class QLabel;class QPushButton;class QPlainTextEdit;
namespace putgd {
class ShortcutEditor;
class SettingsWindow final:public QWidget {
 Q_OBJECT
public:
 using DestinationTester=std::function<void(const QString&,const QJsonObject&,std::function<void(Error)>)>;
 SettingsWindow(SettingsService&,CredentialStore&,DestinationTester={},std::function<QJsonObject()> diagnostics={},QWidget* parent=nullptr);
 void refreshDiagnostics();
signals:
 void actionRequested(const QString&);
private:
 void refresh();void refreshMetadata();void dirty();void save();QJsonObject candidate() const;void loadProfile();void storeProfile();void rebuildProfiles(int selected,const QString& defaultProfile={});void status(const Error&);
 SettingsService& settings_;CredentialStore& secrets_;DestinationTester tester_;std::function<QJsonObject()> diagnostics_;
 QJsonObject draft_;QString observed_;bool loading_=false,dirty_=false,conflict_=false;int profileIndex_=-1;
 QLabel *status_,*metadata_,*capabilities_;QPushButton* save_;
 ShortcutEditor* shortcuts_;QHash<QString,QComboBox*> destinations_,actionProfiles_;QHash<QString,QLineEdit*> folders_,s3_,numbers_;
 QLineEdit *root_,*profileId_,*profileLabel_,*reference_,*access_,*secret_,*token_;QComboBox *profile_,*credentialMode_,*acl_,*naming_,*collision_,*addressing_;QPlainTextEdit* providerJson_;QCheckBox *keep_,*notifications_,*login_;
};
}
