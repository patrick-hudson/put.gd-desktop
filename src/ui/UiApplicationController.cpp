#include "UiApplicationController.h"
#include "SettingsWindow.h"
#include "HistoryWindow.h"
#include "RecentOperations.h"
#include "AboutWindow.h"
#include "ViewState.h"
#include <QApplication>
#include <QCloseEvent>
#include <QMessageBox>
#include <QWindow>
#include <utility>
namespace putgd {
UiApplicationController::UiApplicationController(UiSession& session,UiControl& control,QJsonObject state,QObject* parent)
 :QObject(parent),session_(session),control_(control),saved_(std::move(state)) {
 recordingDeadline_.setSingleShot(true);recordingDeadline_.setInterval(12000);
 connect(&recordingDeadline_,&QTimer::timeout,this,[this]{
  // A timeout is not evidence of suspension: discard capture and restore via
  // the ordered private pipe. The supervisor also restores if this pipe dies.
  control_.send("shortcutRecording",{{"active",false}});
  finishShortcutRecordingRequests({"shortcut_recording_timeout","Global shortcuts could not be paused. Try recording again.",{}});
 });
 session_.setShortcutRecordingHandler([this](bool active,std::function<void(Error)> reply){
  if(exiting_){reply({"ui_stopping","The interface is closing.",{}});return;}
  const auto sequence=control_.send("shortcutRecording",{{"active",active}});
  if(sequence.isEmpty()){reply({"ui_unavailable","The interface connection is unavailable.",{}});return;}
  recordingRequests_.insert(sequence,{active,std::move(reply)});recordingDeadline_.start();
 });
 connect(&control_,&UiControl::failed,this,[this](Error error){finishShortcutRecordingRequests(error);});
 checkpointDelay_.setSingleShot(true);checkpointDelay_.setInterval(250);
 closeDeadline_.setSingleShot(true);closeDeadline_.setInterval(5000);
 connect(&checkpointDelay_,&QTimer::timeout,this,&UiApplicationController::checkpoint);
 connect(&closeDeadline_,&QTimer::timeout,this,[this]{
  // A local timeout cannot prove the resident has not accepted the old draft.
  // Keep editing disabled until the resident acknowledges cancellation.
  if(closeSequence_.isEmpty()){
   if(closing_){closing_->setEnabled(true);closing_->show();openViews_.insert(closing_);}
   closing_.clear();notify({"ui_checkpoint_timeout","Window state is still being saved. Your draft is open; try closing it again.",{}});return;
  }
  closeCancelSequence_=control_.send("closeCancel",{{"replyTo",closeSequence_}});
 });
 connect(&control_,&UiControl::frame,this,&UiApplicationController::receive);
}
UiApplicationController::~UiApplicationController(){
 exiting_=true;session_.setShortcutRecordingHandler({});finishShortcutRecordingRequests({"ui_stopping","The interface is closing.",{}});checkpointDelay_.stop();closeDeadline_.stop();
 disconnect(&control_,nullptr,this,nullptr);
 // QWidget destruction emits hide events. Remove filters and callbacks before
 // state/timer members are destroyed, then let the view owners release widgets.
 for(auto* window:QList<QWidget*>{settings_.get(),history_.get(),activity_.get(),about_.get()})
  if(window){window->removeEventFilter(this);disconnect(window,nullptr,this,nullptr);}
}
QWidget* UiApplicationController::view(const QString& name){
 const auto restore=[this](auto* window,const QString& key){
  if(saved_.contains(key)&&!window->restoreViewState(saved_[key].toObject()))
   notify({"ui_state_invalid","The saved window state could not be restored.",{}});
  window->installEventFilter(this);
  connect(window,&std::remove_pointer_t<decltype(window)>::viewStateChanged,this,[this]{dirtyCheckpoint_=true;checkpointDelay_.start();});
 };
 if(name=="settings"){
  if(!settings_){
   settings_=std::make_unique<SettingsWindow>(session_);restore(settings_.get(),name);
   connect(settings_.get(),&SettingsWindow::quitRequested,this,[this]{
    session_.request("ui.quit",{},this,[this](Error error,QJsonObject){if(error)notify(error);});
   });
   connect(settings_.get(),&SettingsWindow::actionRequested,this,[this](const QString& action){
    session_.request("action.invoke",{{"action",action}},this,[this](Error error,QJsonObject){if(error)notify(error);});
   });
  }
  return settings_.get();
 }
 if(name=="history"){
  if(!history_){history_=std::make_unique<HistoryWindow>(session_);restore(history_.get(),name);}
  else history_->refresh();
  return history_.get();
 }
 if(name=="activity"){
  if(!activity_){activity_=std::make_unique<RecentOperations>(session_);restore(activity_.get(),name);}
  else activity_->refresh();
  return activity_.get();
 }
 if(name=="about"){
  if(!about_){about_=std::make_unique<AboutWindow>(session_.build());about_->installEventFilter(this);}
  return about_.get();
 }
 return nullptr;
}
QJsonObject UiApplicationController::snapshot()const{
 auto state=saved_;state["schemaVersion"]=1;
 if(settings_)state["settings"]=settings_->saveViewState();
 if(history_)state["history"]=history_->saveViewState();
 if(activity_)state["activity"]=activity_->saveViewState();
 return state;
}
void UiApplicationController::checkpoint(){
 if(exiting_||closing_)return;
 if(!checkpointSequence_.isEmpty()){dirtyCheckpoint_=true;return;}
 const auto state=snapshot();
 // Reject unclassified data locally before it can cross the private pipe.
 if(validateUiState(state))return;
 dirtyCheckpoint_=false;
 checkpointSequence_=control_.send("checkpoint",{{"state",state}});
}
void UiApplicationController::notify(const Error& error){
 QWidget* parent=closing_?closing_.data():QApplication::activeWindow();
 auto* notice=new QMessageBox(QMessageBox::Warning,"put.gd",error.message,QMessageBox::Ok,parent);
 notice->setTextFormat(Qt::PlainText);notice->setWindowModality(Qt::NonModal);
 notice->setAttribute(Qt::WA_DeleteOnClose);notice->show();
}
void UiApplicationController::allowClose(){
 exiting_=true;checkpointDelay_.stop();closeDeadline_.stop();
 if(settings_)settings_->clearCredentialInputs();
 if(history_)history_->releasePreview();
 emit exitRequested();
}
void UiApplicationController::finishShortcutRecordingRequests(Error error){
 recordingDeadline_.stop();auto requests=std::exchange(recordingRequests_,{});
 for(auto& request:requests)if(request.reply)request.reply(error);
}
void UiApplicationController::receive(const QString& type,const QString& sequence,const QJsonObject& payload){
 if(exiting_)return;
 if(type=="shutdown"){allowClose();return;}
 if(type=="shortcutRecordingChanged"){
  auto it=recordingRequests_.find(payload["replyTo"].toString());if(it==recordingRequests_.end())return;
  const auto request=std::move(it.value());recordingRequests_.erase(it);if(recordingRequests_.isEmpty())recordingDeadline_.stop();
  const auto value=payload["error"].toObject();Error error{value["code"].toString(),value["message"].toString(),value["details"].toObject()};
  if(!error&&payload["active"]!=request.active)error={"ui_protocol","Invalid shortcut recording acknowledgement.",{}};
  if(request.reply)request.reply(error);
  return;
 }
 if(type=="activate"){
  const auto name=payload["view"].toString();
  auto* window=closing_?nullptr:view(name);
  if(!window){control_.send("activationFailed",{{"replyTo",sequence},{"view",name}});return;}
  openViews_.insert(window);
  if(window->isMinimized())window->showNormal();else window->show();
  window->raise();window->activateWindow();
  control_.send("shown",{{"replyTo",sequence},{"view",name}});return;
 }
 if(type=="invalidated"){
  QPointer<UiApplicationController> guard(this);
  session_.refreshSnapshot([guard,sequence](Error){if(guard&&!guard->exiting_)guard->control_.send("observed",{{"replyTo",sequence}});});return;
 }
 if(type=="checkpointAccepted"||type=="checkpointRejected"){
  if(payload["replyTo"]!=checkpointSequence_)return;
  checkpointSequence_.clear();
  if(closing_&&closeSequence_.isEmpty()){submitClose();return;}
  if(type=="checkpointRejected")dirtyCheckpoint_=true;
  else if(dirtyCheckpoint_)checkpointDelay_.start();
  return;
 }
 if(type=="closeCancelled"){
  if(closeCancelSequence_.isEmpty()||payload["replyTo"]!=closeCancelSequence_)return;
  closeSequence_.clear();closeCancelSequence_.clear();closeDeadline_.stop();
  if(closing_){closing_->setEnabled(true);closing_->show();openViews_.insert(closing_);}
  closing_.clear();notify({"ui_close_cancelled","Closing took too long. Your draft is still open; try closing it again.",{}});return;
 }
 if(type=="closeAccepted"||type=="closeRejected"){
  if(closeSequence_.isEmpty()||payload["replyTo"]!=closeSequence_)return;
  if(!closeCancelSequence_.isEmpty())return; // Cancellation wins locally; wait for its acknowledgement.
  closeDeadline_.stop();const auto request=closeSequence_;closeSequence_.clear();
  if(type=="closeAccepted"){
   exiting_=true;control_.send("closeConfirmed",{{"replyTo",request}});allowClose();return;
  }
  if(closing_){closing_->setEnabled(true);closing_->show();openViews_.insert(closing_);}
  closing_.clear();
  const auto error=payload["error"].toObject();
  notify({error["code"].toString(),error["message"].toString("The draft could not be retained. Keep this window open."),{}});
 }
}
bool UiApplicationController::eventFilter(QObject* object,QEvent* event){
 auto* window=qobject_cast<QWidget*>(object);
 if(!window||exiting_)return QObject::eventFilter(object,event);
 if(event->type()==QEvent::Move||event->type()==QEvent::Resize||event->type()==QEvent::WindowStateChange){dirtyCheckpoint_=true;checkpointDelay_.start();}
 if(event->type()==QEvent::Hide&&window==about_.get()&&!event->spontaneous()&&openViews_.remove(window)){
  // QDialog::accept/reject hides directly, bypassing QCloseEvent. Defer until
  // that dispatch unwinds before attempting the same final-close handshake.
  QTimer::singleShot(0,this,[this,window]{if(exiting_||closing_)return;if(openViews_.isEmpty())requestClose(window);else checkpointDelay_.start();});
 }
 if(event->type()!=QEvent::Close)return QObject::eventFilter(object,event);
 auto* close=static_cast<QCloseEvent*>(event);
 if(closing_){close->ignore();return true;}
 // Native dialogs and transient children must finish before their owning view.
 if(QApplication::activeModalWidget()||QGuiApplication::modalWindow()){close->ignore();return true;}
 for(auto* child:window->findChildren<QWidget*>())if(child->isWindow()&&child->isVisible()){close->ignore();return true;}
 if(auto* native=window->windowHandle())for(auto* child:QGuiApplication::allWindows())
  if(child->isVisible())for(auto* parent=child->transientParent();parent;parent=parent->transientParent())
   if(parent==native){close->ignore();return true;}
 if(window==settings_.get())settings_->clearCredentialInputs();
 if(window==history_.get())history_->releasePreview();
 if(openViews_.size()>(openViews_.contains(window)?1:0)){
  openViews_.remove(window);dirtyCheckpoint_=true;checkpointDelay_.start();return QObject::eventFilter(object,event);
 }
 close->ignore();requestClose(window);return true;
}
void UiApplicationController::requestClose(QWidget* window){
 const auto state=snapshot();
 if(const auto error=validateUiState(state);error){window->show();openViews_.insert(window);notify(error);return;}
 closing_=window;window->setEnabled(false);checkpointDelay_.stop();
 // One large checkpoint may already fill the bounded output queue. Wait for
 // its acknowledgement before adding the final state, retaining newer edits.
 if(checkpointSequence_.isEmpty())submitClose();else closeDeadline_.start();
}
void UiApplicationController::submitClose(){
 const auto state=snapshot();
 if(const auto error=validateUiState(state);error){
  closeDeadline_.stop();if(closing_){closing_->setEnabled(true);closing_->show();openViews_.insert(closing_);}
  closing_.clear();notify(error);return;
 }
 closeSequence_=control_.send("closeRequested",{{"state",state}});
 closeDeadline_.start();
}
}
