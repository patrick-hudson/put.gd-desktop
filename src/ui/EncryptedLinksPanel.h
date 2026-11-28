#pragma once
#include <QJsonObject>
#include <QObject>
#include <functional>
class QComboBox;class QFormLayout;class QLabel;class QLineEdit;class QPushButton;class QWidget;
namespace putgd {
// Destinations → (destination) → Encrypted links (#2079): this destination's default for new
// uploads (Off, On, On with a passphrase), the passphrase kept like any other secret, and
// whether Test upload and link showed the destination serving an encrypted page. Encrypt is offered
// only once that check passed; Off stays selectable whatever it says. Until then the card says so
// beside the choice, with its own Test upload and link button (#2118).
class EncryptedLinksPanel final:public QObject {
 Q_OBJECT
public:
 using Setting=std::function<QWidget*(QFormLayout*,const QString& label,QWidget* control,const QString& hint)>;
 explicit EncryptedLinksPanel(QWidget* parent);
 void build(QFormLayout* form,const Setting& setting);
 // The saved or edited profile, its destination status record ({state, encrypt: {state, reason?}};
 // encrypt state passed|failed|unsupported|changed|untested) and
 // whether the passphrase is loaded (setup.credentialHint's encryptPassphraseLoaded).
 void load(const QJsonObject& profile,const QJsonObject& status,bool passphraseLoaded,bool edited);
 void setPassphraseLoaded(bool loaded);
 // #2118: whether a Test upload and link runs for this destination, and whether one can start now.
 void setTesting(bool running,bool startable);
 // Clears the typed passphrase once a store of it is accepted; a refused one keeps it.
 void clearPassphrase();
 // The profile with this panel's choice written into `encrypt`.
 QJsonObject apply(QJsonObject profile) const;
 QJsonObject viewState() const;
 // ui.encryptedLinks: mode {mode off|on|passphrase}, unlock or test. A passphrase is typed in the window only.
 bool drive(const QJsonObject& command,QString* refusal);
 QString mode() const;
 QWidget* control() const;
signals:
 void edited();
 void storePassphrase(const QString& passphrase);
 void unlockPassphrase();
 // #2118: the card's Test upload and link, the same test as the destination's own button.
 void testRequested();
private:
 bool available() const;
 bool testOffered() const;
 void refresh();
 QComboBox* mode_=nullptr;QLabel* state_=nullptr;QLineEdit* passphrase_=nullptr;QPushButton* store_=nullptr;QPushButton* unlock_=nullptr;QLabel* stored_=nullptr;QPushButton* test_=nullptr;
 QFormLayout* form_=nullptr;QWidget* passphraseRow_=nullptr;
 QString provider_,profileId_,credentialMode_,checkState_,checkReason_,hintFor_;bool loaded_=false,connectionPassed_=false,testing_=false,testStartable_=true;
};
}
