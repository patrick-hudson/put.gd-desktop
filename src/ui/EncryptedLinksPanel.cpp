#include "EncryptedLinksPanel.h"
#include "PreferenceRows.h"
#include "Ui.h"
#include <QComboBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QStandardItemModel>
namespace putgd {
namespace {
// What the destination type says before any check: Proton and Nextcloud never encrypt.
QString typeReason(const QString& provider) {
 if(provider=="proton-drive")return QStringLiteral("Proton Drive already encrypts files end to end.");
 if(provider=="nextcloud")return QStringLiteral("Nextcloud share links download HTML files instead of opening them, so an encrypted page can't open there.");
 return {};
}
}
EncryptedLinksPanel::EncryptedLinksPanel(QWidget* parent):QObject(parent) {}
void EncryptedLinksPanel::build(QFormLayout* form,const Setting& setting) {
 form_=form;auto* parent=qobject_cast<QWidget*>(this->parent());
 mode_=new QComboBox(parent);mode_->setObjectName("encryptedLinksMode");
 mode_->addItem("Off","off");mode_->addItem("On","on");mode_->addItem("On with a passphrase","passphrase");
 const auto hint=QStringLiteral("New uploads here become encrypted pages whose link carries the key after # (default: off). Encrypted links show no preview in chat apps.");
 mode_->setToolTip(hint);mode_->setAccessibleDescription(hint);mode_->setProperty("settingDescription",hint);
 auto* row=setting(form,"Encrypted links",mode_,hint);
 state_=new QLabel(parent);state_->setObjectName("encryptedLinksState");state_->setWordWrap(true);Ui::role(state_,"muted");row->layout()->addWidget(state_);
 passphrase_=new QLineEdit(parent);passphrase_->setObjectName("encryptedLinksPassphrase");passphrase_->setEchoMode(QLineEdit::Password);passphrase_->setPlaceholderText("Type a new passphrase");
 const auto passHint=QStringLiteral("Needed beside the link to open it. Send it to recipients separately. Kept in the keychain like the destination's keys.");
 passphrase_->setToolTip(passHint);passphrase_->setAccessibleDescription(passHint);
 auto* holder=new QWidget(parent);auto* line=new QHBoxLayout(holder);line->setContentsMargins(0,0,0,0);line->setSpacing(Ui::scaled(8));
 store_=new QPushButton("Save passphrase",holder);store_->setObjectName("encryptedLinksStorePassphrase");
 unlock_=new QPushButton("Unlock passphrase",holder);unlock_->setObjectName("encryptedLinksUnlockPassphrase");
 line->addWidget(passphrase_,1);line->addWidget(store_);line->addWidget(unlock_);
 passphraseRow_=setting(form,"Passphrase",holder,passHint);
 stored_=new QLabel(parent);stored_->setObjectName("encryptedLinksPassphraseState");Ui::role(stored_,"muted");passphraseRow_->layout()->addWidget(stored_);
 connect(mode_,&QComboBox::activated,this,[this]{refresh();emit edited();});
 connect(store_,&QPushButton::clicked,this,[this]{const auto text=passphrase_->text();if(text.isEmpty())return;emit storePassphrase(text);});
 connect(passphrase_,&QLineEdit::returnPressed,store_,&QPushButton::click);
 connect(passphrase_,&QLineEdit::textChanged,this,[this]{refresh();});
 connect(unlock_,&QPushButton::clicked,this,[this]{emit unlockPassphrase();});
 refresh();
}
void EncryptedLinksPanel::load(const QJsonObject& profile,const QJsonObject& status,bool passphraseLoaded,bool edited) {
 provider_=profile.value("provider").toString();if(profileId_!=profile.value("id").toString())hintFor_.clear();profileId_=profile.value("id").toString();
 // Where the passphrase itself is kept: its own reference when saved, else the destination's kind of storage.
 const auto passReference=profile.value("encrypt").toObject().value("passphraseCredentials").toObject();
 credentialMode_=(passReference.isEmpty()?profile.value("credentials").toObject():passReference).value("mode").toString("keychain");
 const auto check=status.value("encrypt").toObject();checkState_=check.value("state").toString("untested");checkReason_=check.value("reason").toString();
 // A status refresh (edited) keeps what the credential hint said; a newly shown destination waits for it.
 const auto id=profile.value("id").toString();if(!edited||id!=hintFor_){loaded_=passphraseLoaded;}
 if(!edited){const auto mode=profile.value("encrypt").toObject().value("mode").toString("off");mode_->setCurrentIndex(qMax(0,mode_->findData(mode)));passphrase_->clear();}
 refresh();
}
void EncryptedLinksPanel::clearPassphrase() {if(passphrase_)passphrase_->clear();}
void EncryptedLinksPanel::setPassphraseLoaded(bool loaded) {loaded_=loaded;hintFor_=profileId_;refresh();}
QWidget* EncryptedLinksPanel::control() const {return mode_;}
QString EncryptedLinksPanel::mode() const {return mode_?mode_->currentData().toString():QStringLiteral("off");}
QJsonObject EncryptedLinksPanel::apply(QJsonObject profile) const {
 const auto chosen=mode();
 if(chosen=="off"){if(profile.contains("encrypt"))profile["encrypt"]=QJsonObject{{"mode","off"}};return profile;}
 QJsonObject encrypt{{"mode",chosen}};
 if(chosen=="passphrase") {
  // A saved reference stays as it is; a new one is its own entry, beside the destination's keys,
  // in the same kind of storage.
  const auto saved=profile.value("encrypt").toObject().value("passphraseCredentials").toObject();
  const bool session=profile.value("credentials").toObject().value("mode")=="session";
  encrypt["passphraseCredentials"]=!saved.isEmpty()?saved:QJsonObject{{"mode",session?"session":"keychain"},{"reference",session?QJsonValue(QJsonValue::Null):QJsonValue("putgd/encrypt/"+profile.value("id").toString())}};
 }
 profile["encrypt"]=encrypt;return profile;
}
void EncryptedLinksPanel::refresh() {
 if(!mode_)return;
 const auto reason=typeReason(provider_);
 const bool passed=reason.isEmpty()&&checkState_=="passed";
 QString text;
 if(!reason.isEmpty())text=reason;
 else if(checkState_=="passed")text=QStringLiteral("Test upload and link showed this destination opens encrypted pages.");
 else if(checkState_=="failed"||checkState_=="unsupported")text=checkReason_.isEmpty()?QStringLiteral("This destination didn't pass the encrypted-link check."):checkReason_;
 else text=QStringLiteral("Select Test upload and link to check this destination can open encrypted pages.");
 state_->setText(text);
 // On stays offered only after a passed check; Off is always there.
 for(int i=1;i<mode_->count();++i)if(auto* model=qobject_cast<QStandardItemModel*>(mode_->model()))if(auto* item=model->item(i))item->setEnabled(passed||mode_->currentIndex()==i);
 mode_->setEnabled(passed||mode()!="off");
 const bool pass=mode()=="passphrase";
 if(form_&&passphraseRow_)form_->setRowVisible(passphraseRow_,pass);
 store_->setEnabled(!passphrase_->text().isEmpty());
 unlock_->setVisible(pass&&!loaded_&&credentialMode_=="keychain");
 stored_->setText(loaded_?QStringLiteral("Passphrase loaded"):credentialMode_=="session"?QStringLiteral("No passphrase for this session yet"):QStringLiteral("Passphrase not loaded"));
}
QJsonObject EncryptedLinksPanel::viewState() const {
 // available: whether encrypting can be turned on here; modeEnabled: whether the choice can change (Off always can).
 return {{"mode",mode()},{"check",checkState_},{"reason",state_?state_->text():QString()},{"available",typeReason(provider_).isEmpty()&&checkState_=="passed"},{"modeEnabled",mode_&&mode_->isEnabled()},
  {"passphraseLoaded",loaded_},{"passphraseTyped",passphrase_&&!passphrase_->text().isEmpty()},{"unlockOffered",unlock_&&unlock_->isVisibleTo(unlock_->parentWidget())}};
}
bool EncryptedLinksPanel::drive(const QJsonObject& command,QString* refusal) {
 const auto action=command.value("action").toString();
 if(action=="mode") {
  const int index=mode_->findData(command.value("mode").toString());
  if(index<0){*refusal=QStringLiteral("mode is off, on or passphrase");return false;}
  if(index>0&&!(typeReason(provider_).isEmpty()&&checkState_=="passed")&&mode_->currentIndex()!=index){*refusal=state_->text();return false;}
  mode_->setCurrentIndex(index);refresh();emit edited();return true;
 }
 if(action=="unlock"){if(mode()!="passphrase"){*refusal=QStringLiteral("Choose On with a passphrase first");return false;}emit unlockPassphrase();return true;}
 *refusal=QStringLiteral("action is inspect, mode or unlock");return false;
}
}
