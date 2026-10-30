#pragma once
// Catalog-only fixtures shared with gui_tray regression tests; no app UI changes.
#include "core/StorageProvider.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QMenu>
#include <QSet>
#include <QWidgetAction>
#include <mutex>
#include <utility>

namespace putgd::test {
// A provider fixture, never ProtonProvider: hold one real OperationService upload
// until its menu has been captured, then return the Proton account-busy error.
// This exercises resultReady -> TrayController::latchFailure without a session,
// provider process or network request.
class TrayCatalogProvider final:public StorageProvider {
 std::mutex mutex_;std::function<void()> finish_;bool released_=false;
public:
 bool held(){std::lock_guard lock(mutex_);return bool(finish_);}
 void release(){std::function<void()> done;{std::lock_guard lock(mutex_);released_=true;done=std::exchange(finish_,{});}if(done)done();}
 ProviderCapabilities capabilities(const Destination&)const override {ProviderCapabilities c;c.conditionalCreate=true;c.conditionalReplace=true;c.urls=true;c.sharing=true;c.privateUploads=true;return c;}
 QString serializationKey(const Destination&,const QJsonObject&,const QString& name)const override{return name;}
 Error validateSettings(const QJsonObject&)const override{return {};}
 void checkName(const Destination&,const QString& name,std::function<void(NameCheck)> done,std::shared_ptr<std::atomic_bool> = {})override {done({"absent",{{"syntheticName",name}},{},{}});}
 void upload(const UploadRequest&,std::function<void(UploadResult)> done)override {
  auto complete=[done]{done({"failed",{},{"proton_account_busy","Synthetic Proton account is busy",{}}});};
  {std::lock_guard lock(mutex_);if(!released_){finish_=complete;return;}}
  complete();
 }
 void stat(const StorageReceipt&,std::function<void(StatResult)> done)override {done({"unknown",{},{}});}
 void reconcile(const UploadRequest&,std::function<void(StatResult)> done)override {done({"unknown",{},{}});}
 void remove(const DeleteRequest&,DurableCheckpoint,std::function<void(DeleteResult)> done)override {done({"failed",{},{"synthetic_only","No remote objects exist",{}}});}
};
// QAction labels use mnemonics; QWidgetAction output labels are plain text.
inline std::optional<QJsonArray> catalogMenuRows(QMenu& root){
 QJsonArray rows;QSet<QString> ids;
  for(auto* action:root.actions()){
   if(!action->isVisible()||action->isSeparator())continue;
   const auto rect=root.actionGeometry(action);
   if(rect.isEmpty()||!root.rect().contains(rect)){qCritical("Tray menu row does not fit: %s",qPrintable(action->text()));return std::nullopt;}
   auto id=action->objectName();if(id.isEmpty())id=action->data().toString();if(id.isEmpty())id="trayMenuAction-"+QString::fromLatin1(QCryptographicHash::hash(action->text().toUtf8(),QCryptographicHash::Sha256).toHex().left(16));
   if(ids.contains(id)){qCritical("Duplicate tray row identity: %s",qPrintable(id));return std::nullopt;}ids.insert(id);
   // QWidgetAction's QLabel uses PlainText, so escape its literal ampersands
   // for the same mnemonic-aware inventory contract as QAction labels.
   auto text=action->text();if(qobject_cast<QWidgetAction*>(action))text.replace("&","&&");
   rows.append(QJsonObject{{"name",id},{"class","QAction"},{"kind","menu-item"},{"text",text},{"rect",QJsonArray{rect.x(),rect.y(),rect.width(),rect.height()}},{"enabled",action->isEnabled()},{"submenu",action->menu()!=nullptr}});
  }
 return rows;
}
}
