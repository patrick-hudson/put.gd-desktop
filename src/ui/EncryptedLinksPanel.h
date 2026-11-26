#pragma once
#include <QJsonObject>
#include <QObject>
#include <functional>
class QComboBox;class QFormLayout;class QLabel;class QLineEdit;class QPushButton;class QWidget;
namespace putgd {
// Destinations → (destination) → Encrypted links (#2079): this destination's default for new
// uploads (Off, On, On with a passphrase), the passphrase kept like any other secret, and
// whether Test upload and link showed the destination serving an encrypted page. Encrypt is offered
// only once that check passed; Off stays selectable whatever it says.
class EncryptedLinksPanel final:public QObject {
 Q_OBJECT
public:
 using Setting=std::function<QWidget*(QFormLayout*,const QString& label,QWidget* control,const QString& hint)>;
 explicit EncryptedLinksPanel(QWidget* parent);
 void build(QFormLayout* form,const Setting& setting);
 // The saved or edited profile, its destination status record ({encrypt: {state, reason?}}) and
 // whether the passphrase is loaded (setup.credentialHint's encryptPassphraseLoaded).
 void load(const QJsonObject& profile,const QJsonObject& status,bool passphraseLoaded,bool edited);
 void setPassphraseLoaded(bool loaded);
 // Clears the typed passphrase once a store of it is accepted; a refused one keeps it.
 void clearPassphrase();
 // The profile with this panel's choice written into `encrypt`.
 QJsonObject apply(QJsonObject profile) const;
 QJsonObject viewState() const;
 // ui.encryptedLinks: mode {mode off|on|passphrase} or unlock. A passphrase is typed in the window only.
 bool drive(const QJsonObject& command,QString* refusal);
 QString mode() const;
 QWidget* control() const;
signals:
 void edited();
 void storePassphrase(const QString& passphrase);
 void unlockPassphrase();
private:
 void refresh();
 QComboBox* mode_=nullptr;QLabel* state_=nullptr;QLineEdit* passphrase_=nullptr;QPushButton* store_=nullptr;QPushButton* unlock_=nullptr;QLabel* stored_=nullptr;
 QFormLayout* form_=nullptr;QWidget* passphraseRow_=nullptr;
 QString provider_,profileId_,credentialMode_,checkState_,checkReason_,hintFor_;bool loaded_=false;
};
}
