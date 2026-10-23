#include "OperationService.h"
#include "core/Log.h"
#include <QMap>
#include <QPointer>

namespace putgd {
struct OperationService::PendingCopy {
 std::function<CopyPayload()> payload;
 std::function<void(Error)> reply;
 std::function<bool()> eligible;
 CopyPreparation prepare;QStringList operations;std::shared_ptr<CopyPreparationState> preparationState;
 bool upload=false,retryUnsent=false,nativeStarted=false;
};
struct OperationService::UploadCopy {
 Operation operation;
 quint64 epoch=0,revision=0;
 QMap<int,QString> links;
 QSet<QString> seen;
 // each_link batches copy in source order: a ready link waits in `held` until
 // every earlier file has settled (uploaded, failed, skipped or cancelled).
 QMap<int,QString> held;QSet<int> settled;int cursor=0;
 std::shared_ptr<std::atomic_bool> cancellation;
 std::shared_ptr<UploadCopyGroup> group;
 int pending=0;
 bool finished=false,waitingForGroup=false,recorded=false,copied=false,suppressed=false;
 // Why the automatic copy was refused, for the log record (copyGuard).
 QJsonObject suppression;
 Error error;
 JsonReply reply;
};
struct OperationService::UploadCopyGroup {
 quint64 epoch=0,revision=0;
 int pending=0;
 QMap<quint64,std::weak_ptr<UploadCopy>> members;
 QJsonObject suppression;
};
QJsonObject OperationService::copyGuard(bool stopping,bool cancelled,const char* epochGuard,quint64 epoch,quint64 currentEpoch,quint64 revision,quint64 currentRevision) {
 QString guard;
 if(stopping)guard="stopping";
 else if(cancelled)guard="cancelled";
 else if(epoch!=currentEpoch)guard=QLatin1String(epochGuard);
 else if(revision!=currentRevision)guard="clipboard_changed";
 if(guard.isEmpty())return {};
 return {{"guard",guard},{"revisionAtInvocation",qint64(revision)},{"revisionAtDispatch",qint64(currentRevision)},{"epochAtInvocation",qint64(epoch)},{"epochAtDispatch",qint64(currentEpoch)}};
}
QJsonObject OperationService::nativeCopyGuard(const Error& error) {return {{"guard","native_clipboard"},{"code",error.code},{"message",error.message.left(256)}};}
quint64 OperationService::invalidateCopyEligibility() {
 ++uploadCopyEpoch_;
 if(currentCopy_&&currentCopy_->preparationState)currentCopy_->preparationState->cancelled->store(true);
 for(const auto& copy:copies_)if(copy->preparationState)copy->preparationState->cancelled->store(true);
 source_.invalidateCopyEligibility();
 return ++invocation_;
}
void OperationService::beginUploadCopies(const Operation& op,bool importing) {
 auto state=std::make_shared<UploadCopy>();
 state->operation=op;state->epoch=uploadCopyEpoch_;state->revision=op.clipboardRevision;
 state->cancellation=active_.value(op.id);
 uploadCopies_.insert(op.id,state);
 if(op.settings.uploadClipboardBehavior=="list") {
  if(!collectingUploadGroup_||collectingUploadGroup_->pending==0||collectingUploadGroup_->epoch!=state->epoch||collectingUploadGroup_->revision!=state->revision) {
   collectingUploadGroup_=std::make_shared<UploadCopyGroup>();
   collectingUploadGroup_->epoch=state->epoch;collectingUploadGroup_->revision=state->revision;
  }
  state->group=collectingUploadGroup_;state->waitingForGroup=true;
  ++state->group->pending;state->group->members.insert(op.invocation,state);
 }
 if(importing) {
  clipboardImports_.insert(op.id);
  // An unsent native result may already be awaiting acknowledgement. Pause it
  // before freezing an import, then retry only a confirmed unsent suppression.
  interruptUploadCopy();
 }
 if(currentCopy_&&!currentCopy_->upload){if(currentCopy_->preparationState)currentCopy_->preparationState->cancelled->store(true);source_.invalidateCopyEligibility();}
}
void OperationService::abandonUploadCopies(const QString& id) {
 clipboardImports_.remove(id);
 const auto state=uploadCopies_.take(id);
 if(state&&state->group) {
  auto group=state->group;group->members.remove(state->operation.invocation);
  --group->pending;settleUploadGroup(group);
 }
 pumpCopies();
}
bool OperationService::detachCompletedUploadCopies(const QString& id,JsonReply& pendingReply) {
 const auto state=uploadCopies_.value(id);
 if(!state||!state->group||!state->finished||state->recorded)return false;
 // The upload has finished; only its automatic list is waiting. A deliberate
 // Activity copy takes ownership of the final copy status and any retry reply.
 state->recorded=true;pendingReply=std::move(state->reply);
 state->group->members.remove(state->operation.invocation);
 uploadCopies_.remove(id);releaseActive(id);return true;
}
void OperationService::interruptUploadCopy(const QString& id) {
 if(currentCopy_&&currentCopy_->upload&&currentCopy_->nativeStarted&&(id.isEmpty()||currentCopy_->operations.contains(id))) {
  currentCopy_->retryUnsent=true;source_.invalidateCopyEligibility();
 }
}
void OperationService::uploadLinkReady(const QString& id,const QString& item,int index,const QString& url) {
 const auto state=uploadCopies_.value(id);
 if(!state||state->seen.contains(item)||url.isEmpty())return;
 state->seen.insert(item);state->links.insert(index,url);
 if(state->group)return;
 // The cursor has passed this file already: nothing earlier can still be
 // waiting on it, so its link is copied now rather than held forever (#478).
 if(index<state->cursor){queueLinkCopy(state,item,index);return;}
 state->held.insert(index,item);flushUploadLinks(state);
}
void OperationService::uploadItemSettled(const QString& id,int index) {
 const auto state=uploadCopies_.value(id);
 if(!state||state->group)return;
 state->settled.insert(index);flushUploadLinks(state);
}
void OperationService::flushUploadLinks(const std::shared_ptr<UploadCopy>& state) {
 for(;;) {
  if(state->held.contains(state->cursor)) {const auto item=state->held.take(state->cursor);queueLinkCopy(state,item,state->cursor);++state->cursor;}
  else if(state->settled.contains(state->cursor))++state->cursor;
  else break;
 }
}
void OperationService::queueLinkCopy(const std::shared_ptr<UploadCopy>& state,const QString& item,int index) {
 ++state->pending;
 auto payload=std::make_shared<CopyPayload>();payload->kind="text";
 const auto path=databasePath();const auto id=state->operation.id;
 queueCopy([payload]{return *payload;},[this,state,index](Error error) {
  --state->pending;
  if(!error)state->copied=true;
  else if(error.code=="copy_suppressed"){state->suppressed=true;if(state->suppression.isEmpty())state->suppression=nativeCopyGuard(error);}
  else if(error.code=="copy_no_result")state->links.remove(index);
  else state->error=error;
  recordUploadCopy(state);
 },[this,state]{
  // An explicit copy or another history action moves the epoch (invalidateCopyEligibility).
  const auto guard=copyGuard(stopping_,state->cancellation->load(),"superseded",state->epoch,uploadCopyEpoch_,state->revision,source_.clipboardRevision());
  if(!guard.isEmpty())state->suppression=guard;return guard.isEmpty();
 },true,
 [this,path,id,item,payload](auto ready) {
  // Resolve the durable item again at dispatch, including every resumed copy.
  background([path,id,item](Error& error) {
   HistoryStore store(path);const auto op=store.operation(id,&error);QString url;
   for(const auto& value:op.items)if(value.artifact.id==item&&value.linkAvailable())url=value.receipt.directUrl;
   return QJsonObject{{"url",url}};
  },[payload,ready](Error error,QJsonObject value){payload->text=value["url"].toString();ready(error);});
 },{id});
}
void OperationService::refreshUploadLinks(UploadCopy& state,const Operation& op) {
 state.operation=op;state.links.clear();
 for(int index=0;index<op.items.size();++index) {
  const auto& item=op.items[index];
  if(state.seen.contains(item.artifact.id)&&item.linkAvailable())state.links.insert(index,item.receipt.directUrl);
 }
}
void OperationService::finishUploadCopies(Operation op,JsonReply reply) {
 const auto state=uploadCopies_.value(op.id);
 if(!state)return;
 refreshUploadLinks(*state,op);state->reply=std::move(reply);
 // Every file has settled now; release any link still waiting for an earlier one.
 for(int index=0;index<op.items.size();++index)state->settled.insert(index);
 // Final receipts also cover reconciliation/retry paths which made no new PUT.
 for(int index=0;index<op.items.size();++index) {
  const auto& item=op.items[index];
  if(item.linkAvailable())uploadLinkReady(op.id,item.artifact.id,index,item.receipt.directUrl);
 }
 state->finished=true;
 if(state->group){--state->group->pending;settleUploadGroup(state->group);}
 else {flushUploadLinks(state);recordUploadCopy(state);}
}
void OperationService::settleUploadGroup(const std::shared_ptr<UploadCopyGroup>& group) {
 if(group->pending!=0)return;
 if(collectingUploadGroup_==group)collectingUploadGroup_.reset();
 QList<std::shared_ptr<UploadCopy>> members;
 for(const auto& weak:group->members)if(auto state=weak.lock())members.append(state);
 if(members.isEmpty())return;
 const auto path=databasePath();QStringList ids;for(const auto& state:members)ids.append(state->operation.id);
 auto included=std::make_shared<QSet<QString>>();
 queueCopy([members,included] {
  CopyPayload payload;payload.kind="text";QStringList urls;included->clear();
  for(const auto& state:members)if(!state->cancellation->load()&&!state->links.isEmpty()) {
   included->insert(state->operation.id);
   for(const auto& url:state->links)urls.append(url);
  }
  payload.text=urls.join('\n');return payload;
 },[this,members,included,group](Error error) {
  for(const auto& state:members) {
   state->waitingForGroup=false;
   if(included->contains(state->operation.id)) {
    if(!error)state->copied=true;
    else if(error.code=="copy_suppressed")state->suppressed=true;
    else if(error.code!="copy_no_result")state->error=error;
   }else if(error.code=="copy_suppressed"&&!state->links.isEmpty()&&!state->cancellation->load())state->suppressed=true;
   if(state->suppressed&&state->suppression.isEmpty())state->suppression=group->suppression.isEmpty()?nativeCopyGuard(error):group->suppression;
   recordUploadCopy(state);
  }
 },[this,group]{
  const auto guard=copyGuard(stopping_,false,"superseded",group->epoch,uploadCopyEpoch_,group->revision,source_.clipboardRevision());
  if(!guard.isEmpty())group->suppression=guard;return guard.isEmpty();
 },true,
 [this,path,ids,members](auto ready) {
  // Re-read at dispatch rather than group settlement: a clipboard import or
  // another native commit may hold this list after a known deletion/revocation.
  background([path,ids](Error&) {
   HistoryStore store(path);QJsonArray values;
   for(const auto& id:ids){Error error;auto op=store.operation(id,&error);values.append(QJsonObject{{"operation",op.toJson()},{"error",error.toJson()}});}
   return QJsonObject{{"members",values}};
  },[members,ready](Error error,QJsonObject result) {
   const auto values=result["members"].toArray();
   for(int index=0;index<members.size();++index) {
    const auto state=members[index];const auto value=values[index].toObject();const auto failure=value["error"].toObject();
    if(!failure["code"].toString().isEmpty()){state->links.clear();state->error={failure["code"].toString(),failure["message"].toString(),failure["details"].toObject()};}
    else refreshUploadLinks(*state,Operation::fromJson(value["operation"].toObject()));
   }
   ready(error);
  });
 },ids);
}

void OperationService::recordUploadCopy(const std::shared_ptr<UploadCopy>& state) {
 if(!state->finished||state->pending||state->waitingForGroup||state->recorded)return;
 state->recorded=true;auto op=state->operation;
 if(state->copied&&(state->error||state->suppressed))op.copyStatus="partial";
 else if(state->copied)op.copyStatus="copied";
 else if(state->cancellation->load()||state->links.isEmpty())op.copyStatus="no_result";
 else if(state->error)op.copyStatus="failed";
 else if(state->suppressed)op.copyStatus="suppressed";
 else {
  // A link was available and no copy of it was refused or failed, so none was
  // ever queued. That is a defect, reported as such rather than as a quiet
  // "nothing to copy" (#478).
  state->error={"copy_not_attempted","A link was available but its automatic copy was never attempted",{}};op.copyStatus="failed";
  logging::warning(logOperation(),"Automatic copy not attempted",{{"operationId",op.id},{"action",op.action},{"behavior",op.settings.uploadClipboardBehavior},{"links",state->links.size()},{"held",state->held.size()},{"cursor",state->cursor}});
 }
 if(state->error)op.error=state->error;
 {
  // One record per upload (#473): the outcome the notification and Activity
  // show, and for a suppressed copy which guard refused it and the revisions
  // it compared. Never the links or the clipboard text.
  QJsonObject fields{{"operationId",op.id},{"action",op.action},{"automatic",true},{"behavior",op.settings.uploadClipboardBehavior},{"copyStatus",op.copyStatus},{"links",state->links.size()},{"cancelled",state->cancellation->load()}};
  if(state->error)fields["error"]=logging::errorFields(state->error);
  if(!state->suppression.isEmpty())fields["suppression"]=state->suppression;
  logging::write(logClipboard(),op.copyStatus=="failed"?QtWarningMsg:QtInfoMsg,"Automatic copy finished",fields);
 }
 const auto path=databasePath();
 background([path,op](Error& error){HistoryStore store(path);error=store.saveOperation(op);return op.publicJson();},[this,state,op](Error error,QJsonObject result) {
  uploadCopies_.remove(op.id);releaseActive(op.id);
  emit changed(op.id);if(!error)emit resultReady(op.id);
  if(state->reply)state->reply(error,result);
 });
}
void OperationService::acknowledgeCopyRevision(quint64 before,quint64 after) {
 // Advance only snapshots that still expected the overwritten revision. A
 // manual change, even one received before the queued callback, stays distinct.
 for(const auto& state:uploadCopies_) {
  if(state->revision==before)state->revision=after;
  if(state->group&&state->group->revision==before)state->group->revision=after;
 }
 const auto advance=[before,after](const std::shared_ptr<PendingCopy>& copy){if(copy&&copy->preparationState&&copy->preparationState->revision==before)copy->preparationState->revision=after;};
 advance(currentCopy_);for(const auto& copy:copies_)advance(copy);
}
void OperationService::deliverCopy(const CopyPayload& payload,std::function<void(Error)> reply,std::function<bool()> eligible,bool upload) {
 queueCopy([payload]{return payload;},std::move(reply),std::move(eligible),upload);
}
void OperationService::queueCopy(std::function<CopyPayload()> payload,std::function<void(Error)> reply,std::function<bool()> eligible,bool upload,CopyPreparation prepare,QStringList operations,std::shared_ptr<CopyPreparationState> preparationState) {
 auto request=std::make_shared<PendingCopy>();request->payload=std::move(payload);request->reply=std::move(reply);request->eligible=std::move(eligible);request->upload=upload;request->prepare=std::move(prepare);request->operations=std::move(operations);request->preparationState=std::move(preparationState);
 copies_.enqueue(request);pumpCopies();
}
void OperationService::pumpCopies() {
 if(stopping_||currentCopy_||copies_.isEmpty()||!clipboardImports_.isEmpty())return;
 auto request=copies_.dequeue();currentCopy_=request;QPointer<OperationService> owner(this);
 auto complete=[owner,request](Error error,quint64 before) {
  if(!owner)return;
  QMetaObject::invokeMethod(owner,[owner,request,before,error] {
   if(!owner||owner->stopping_)return;
   owner->currentCopy_.reset();request->nativeStarted=false;
   if(request->retryUnsent&&error.code=="copy_suppressed") {
    request->retryUnsent=false;owner->copies_.prepend(request);
   }else {
    // Copies are serialized, so the frozen receipt still belongs to this
    // request even when its callback arrived on another thread.
    if(!error)if(const auto acknowledged=owner->source_.lastCopyRevision())owner->acknowledgeCopyRevision(before,*acknowledged);
    request->reply(error);
   }
   owner->pumpCopies();owner->scheduleHeapReclamation();
  },Qt::QueuedConnection);
 };
 auto dispatch=[owner,request,complete](Error error) {
  if(!owner||owner->stopping_)return;
  if(!owner->clipboardImports_.isEmpty()) {
   owner->currentCopy_.reset();owner->copies_.prepend(request);return;
  }
  const auto before=owner->source_.clipboardRevision();
  if(error){complete(error,before);return;}
  if(request->eligible&&!request->eligible()){complete({"copy_suppressed","A user clipboard change, cancellation or explicit copy superseded automatic copying",{}},before);return;}
  const auto payload=request->payload();
  if(payload.kind=="text"&&payload.text.isEmpty()){complete({"copy_no_result","No successful links to copy",{}},before);return;}
  request->nativeStarted=true;
  owner->source_.copy(payload,[complete,before](Error error){complete(error,before);});
 };
 if(request->prepare)request->prepare(std::move(dispatch));else dispatch({});
}
}
