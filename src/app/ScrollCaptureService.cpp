#include "ScrollCaptureService.h"
#include "core/EditorJob.h"
#include <QDir>
#include <QFileInfo>
#include <QLocale>
#include <QFile>
#include <QJsonArray>
#include <QKeySequence>
#include <QPointer>
#include <QTimer>
#include <QCryptographicHash>
#include <QtEndian>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
namespace putgd {
namespace {
Error invalid(){return {"invalid_request","Invalid scrolling capture parameters",{}};}
bool rect(const QJsonValue& v){if(!v.isArray()||v.toArray().size()!=4)return false;const auto a=v.toArray();for(const auto& n:a)if(!n.isDouble()||n.toDouble()!=n.toInteger()||qAbs(n.toDouble())>1000000)return false;return a[2].toInt()>=64&&a[3].toInt()>=64;}
bool fields(const QJsonObject& p,QStringList allowed){for(auto i=p.begin();i!=p.end();++i)if(!allowed.contains(i.key()))return false;return true;}
}
ScrollCaptureService::ScrollCaptureService(QString root,Host host,QObject* parent):QObject(parent),root_(std::move(root)),host_(std::move(host)){clock_.start();holdTimer_.setSingleShot(true);connect(&holdTimer_,&QTimer::timeout,this,&ScrollCaptureService::releaseHeld);QDir().mkpath(root_);QFile::setPermissions(root_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);RecordingService::discardStaging(root_);setBackend(std::make_unique<UnavailableScrollBackend>());}
ScrollCaptureService::~ScrollCaptureService(){shutdown();}
void ScrollCaptureService::setBackend(std::unique_ptr<ScrollBackend> backend){if(active())return;backend_=std::move(backend);QPointer<ScrollCaptureService> g(this);backend_->setEventSink([g](QJsonObject e){if(g)QMetaObject::invokeMethod(g,[g,e]{if(g)g->event(e);},Qt::QueuedConnection);});}
QJsonObject ScrollCaptureService::capabilities()const{auto c=backend_->capabilities();if(c["reason"]=="scroll_not_probed"||c["reason"]=="scroll_probe_pending"||c["reason"]=="scroll_probe_failed")backend_->refreshCapabilities([]{});c["maxHeight"]=host_.settings().scrollCaptureMaxHeight;return c;}
void ScrollCaptureService::update(QString state){
 if(shell_&&view_.contains("rect")&&(state=="capturing"||state=="ready")){
  const QJsonObject frame{{"rect",view_["rect"]},{"kind","region"},{"state",state=="capturing"?"recording":"ready"},{"pid",double(backend_->processId(id_))}};
  if(frame!=shownFrame_){shownFrame_=frame;shell_->showFrame(frame,[](Error,QJsonObject){});}
 }
 view_["state"]=state;op_.phase=state;host_.observe(op_);
}
void ScrollCaptureService::holdStart(int ms,bool fromNow){
 const qint64 until=clock_.elapsed()+std::clamp(ms,0,5000);
 notBefore_=fromNow?until:std::max(notBefore_,until);
 if(held_)holdTimer_.start(int(std::max<qint64>(0,notBefore_-clock_.elapsed())));
}
void ScrollCaptureService::failPreparation(Error error){
 if(!active()||view_["state"]=="capturing"||view_["state"]=="finishing")return;
 terminate(error);notBefore_=0;holdTimer_.stop();releaseHeld();
}
void ScrollCaptureService::maybeStartHelper(){
 if(!helperAutoStart_||!pending_.isEmpty()||view_["state"]!="ready"||!active())return;
 helperAutoStart_=false;command("start",{{"id",id_}},[](Error,QJsonObject){});
}
// The hold is over, or was moved: the command it kept back runs, or waits on.
void ScrollCaptureService::releaseHeld(){
 if(!heldRun_)return;
 if(clock_.elapsed()<notBefore_){holdTimer_.start(int(notBefore_-clock_.elapsed()));return;}
 held_=false;std::exchange(heldRun_,{})();
}
void ScrollCaptureService::invoke(bool edit,JsonReply reply){if(active()){command("stop",{{"id",id_}},reply);return;}prepare({},edit,true,reply);}
void ScrollCaptureService::prepare(QJsonObject p,bool edit,bool autoStart,JsonReply reply){
 QPointer<ScrollCaptureService> g(this);
 backend_->refreshCapabilities([g,p,edit,autoStart,reply]{if(g)QMetaObject::invokeMethod(g,[g,p,edit,autoStart,reply]{if(g)g->prepareProbed(p,edit,autoStart,reply);},Qt::QueuedConnection);});
}
void ScrollCaptureService::prepareProbed(QJsonObject p,bool edit,bool autoStart,JsonReply reply){
 if(!fields(p,{"rect","maxHeight","driver"})||(p.contains("driver")&&p["driver"]!="user"&&p["driver"]!="putgd")||(p.contains("rect")&&!rect(p["rect"]))||(p.contains("maxHeight")&&(!p["maxHeight"].isDouble()||p["maxHeight"].toDouble()!=p["maxHeight"].toInt()||p["maxHeight"].toInt()<2000||p["maxHeight"].toInt()>60000))){reply(invalid(),{});return;}
 if(active()){reply({"scroll_active","A scrolling capture is active",{{"operationId",id_}}},{});return;}
 const auto snapshot=host_.settings();const auto route=snapshot.actions.value(edit?"edit":"scroll");
 if(!(route.destination=="upload"&&!route.profileId.isEmpty())&&!snapshot.archive.keepUploads){reply({"output_required","No output is enabled. Choose a destination or turn on Keep a local copy in Settings → Capture.",{}},{});return;}
 if(!capabilities()["available"].toBool()){reply({"scroll_unavailable","Scrolling capture is unavailable",{{"reason",capabilities()["reason"]}}},{});return;}
 // No selector in this build: no capture, for a caller that names the rectangle either (see probeScroll).
 if(!shell_&&capabilities()["selection"]=="none"){reply({"scroll_unavailable","This desktop has no region selector for a scroll capture",{{"reason","region_unavailable"}}},{});return;}
 if(shell_&&!shell_->selectsRegions()){reply(shell_->regionError(),{});return;}
 op_={};op_.action=edit?"edit":"scroll";if(auto e=host_.accept(op_)){reply(e,{});return;}
 // A command an earlier session's hold still keeps back is answered now: its session has ended.
 notBefore_=0;holdTimer_.stop();releaseHeld();
 shownFrame_={};edit_=edit;id_=op_.id;held_=false;cancelling_=false;emit preparing();directory_=root_+"/"+id_;QDir().mkpath(directory_);QFile::setPermissions(directory_,QFile::ReadOwner|QFile::WriteOwner|QFile::ExeOwner);
 op_.advance(OperationState::Acquiring);view_={{"id",id_},{"operationId",id_},{"state","preparing"},{"height",0},{"steps",0},{"pixelsPerNotch",0},{"placed",0},{"skipped",0}};
 const auto settings=op_.settings;p["maxHeight"]=p["maxHeight"].toInt(settings.scrollCaptureMaxHeight);p["fromTop"]=settings.scrollCaptureFromTop;p["autoStart"]=autoStart;
 // #1515: who turns the page at the start (the setting, unless the caller says), and the key that finishes it here (the `scroll` hotkey pressed again).
 if(!p.contains("driver"))p["driver"]=settings.scrollCaptureAutoScroll?"putgd":"user";
 view_["driver"]=p["driver"];view_["paused"]=false;
 {const auto key=QKeySequence::fromString(settings.actions.value("scroll").shortcut,QKeySequence::PortableText).toString(QKeySequence::NativeText);if(!key.isEmpty())p["finishKey"]=key;}
 p["maxPixels"]=double(std::min<qint64>({settings.limits.imagePixels,settings.limits.imageBytes/4,edit?EditorJob::admissionPixels:settings.limits.imagePixels}));
 shellAutoStart_=shell_&&autoStart;helperAutoStart_=!shell_&&autoStart;shellPlacing_=false;
 if(shell_||helperAutoStart_)p["autoStart"]=false;
 p["selection"]=shell_?"shell":"helper";
 requestedCap_=p["maxHeight"].toInt();const auto id=id_;QPointer<ScrollCaptureService> g(this);
 host_.persist(op_,[g,id,p,reply](Error e){if(!g)return;if(g->id_!=id){reply({"cancelled","Scrolling capture ended",{}},{});return;}if(e){g->terminate(e);reply(e,{});return;}
 g->backend_->prepare(id,p,g->directory_,[g,id,p,reply](Error e,QJsonObject result){if(!g)return;QMetaObject::invokeMethod(g,[g,id,p,e,result,reply]{if(!g)return;if(g->id_!=id){reply({"cancelled","Scrolling capture ended",{}},{});return;}if(e){g->terminate(e);reply(e,{});return;}g->update(result["state"].toString("selecting"));if(p.contains("rect")){g->view_["rect"]=p["rect"];if(g->shell_)g->placeShellBar();}reply({},g->view_);g->maybeStartHelper();
 if(g->shell_&&!p.contains("rect")){g->shellSelecting_=true;g->shell_->selectRegion([g,id](Error e,QJsonObject r){if(!g||g->id_!=id||!g->shellSelecting_)return;g->shellSelecting_=false;if(e){g->terminate(e,e.code=="cancelled");return;}g->command("select",{{"id",id},{"rect",r["rect"]}},[](Error,QJsonObject){});});}
 },Qt::QueuedConnection);});});
}
void ScrollCaptureService::command(const QString& cmd,const QJsonObject& p,JsonReply reply){
 if(cmd=="capabilities"){if(!p.isEmpty()){reply(invalid(),{});return;}QPointer<ScrollCaptureService> g(this);backend_->refreshCapabilities([g,reply]{if(g)QMetaObject::invokeMethod(g,[g,reply]{if(g)reply({},g->capabilities());},Qt::QueuedConnection);});return;}
 if(cmd=="prepare"){prepare(p,false,false,reply);return;}
 if(!QStringList{"select","start","autoScroll","pause","resume","stop","cancel","inspect"}.contains(cmd)||!fields(p,cmd=="select"?QStringList{"id","rect"}:cmd=="autoScroll"?QStringList{"id","on"}:QStringList{"id"})||!p["id"].isString()||p["id"].toString().isEmpty()||(cmd=="select"&&!rect(p["rect"]))||(cmd=="autoScroll"&&!p["on"].isBool())){reply(invalid(),{});return;}
 const auto id=p["id"].toString();if(cmd=="inspect"&&results_.contains(id)){reply({},results_[id]);return;}
 if(id!=id_){reply({"scroll_not_found","No scrolling capture has this ID",{}},{});return;}
 if(cmd=="inspect"){reply({},view_);return;}
 // What was taken off the screen for this capture gets its time to go before a frame can be read.
 if((cmd=="start"||cmd=="select")&&clock_.elapsed()<notBefore_){
  if(held_){reply({"scroll_busy","A scroll command is pending",{}},{});return;}
  held_=true;
  heldRun_=[this,id,cmd,p,reply]{if(id_!=id){const auto e=results_.value(id)["error"].toObject();if(!e["code"].toString().isEmpty())reply({e["code"].toString(),e["message"].toString(),e["details"].toObject()},{});else reply({"scroll_not_found","Session ended",{}},{});return;}notBefore_=0;command(cmd,p,reply);};
  holdTimer_.start(int(notBefore_-clock_.elapsed()));
  return;
 }
 // GNOME: `ready` is answered while the Shell still places the bar. A start
 // that arrives meanwhile waits for the placement; it is not refused.
 if(cmd=="start"&&shellPlacing_&&pending_.isEmpty()&&!startWaiting_&&view_["state"].toString()=="ready"){startWaiting_=std::move(reply);return;}
 if(((!pending_.isEmpty()||cancelling_)&&cmd!="cancel")||(cmd=="start"&&shellPlacing_)){reply({"scroll_busy","A scroll command is pending",{}},{});return;}
 const auto state=view_["state"].toString();if((cmd=="start"&&state!="ready")||(cmd=="select"&&state!="selecting")||((cmd=="stop"||cmd=="autoScroll"||cmd=="pause"||cmd=="resume")&&state!="capturing")){reply({"scroll_state","Command is unavailable in this state",{}},{});return;}
 if(cmd=="select"&&shellSelecting_){shellSelecting_=false;shell_->cancelSelection();}
 if(cmd!="cancel")pending_=cmd;else cancelling_=true;if(cmd=="stop")update("finishing");QPointer<ScrollCaptureService> g(this);
 auto controlParams=p;controlParams.remove("id");
 backend_->control(id,cmd,controlParams,[g,id,cmd,p,reply](Error e,QJsonObject r){if(!g)return;QMetaObject::invokeMethod(g,[g,id,cmd,p,e,r,reply]{if(!g)return;if(g->id_!=id){if(g->results_.contains(id))reply({},g->results_[id]);else reply({"scroll_not_found","Session ended",{}},{});return;}
  // The Cancel that is on its way decides (see cancelling_), not this answer.
  if(g->cancelling_&&cmd!="cancel"){reply({"cancelled","Scrolling capture canceled",{}},{});return;}
  g->pending_.clear();if(e){g->terminate(e,e.code=="cancelled");reply(e,{});return;}if(cmd=="cancel")g->terminate({},true);else if(cmd=="stop"){if(r["state"]=="cancelled")g->terminate({},true);else g->complete(r);}else {if(cmd=="select")g->view_["rect"]=p["rect"];if(cmd=="autoScroll"||cmd=="pause"||cmd=="resume")g->paused(r);if(cmd=="start")g->view_["paused"]=false;g->update(r["state"].toString(cmd=="start"||cmd=="autoScroll"||cmd=="pause"||cmd=="resume"?"capturing":"ready"));if(cmd=="select"&&g->shell_)g->placeShellBar();g->maybeStartHelper();}reply({},g->results_.contains(id)?g->results_[id]:g->view_);},Qt::QueuedConnection);});
}
void ScrollCaptureService::placeShellBar(){
 shellPlacing_=true;
 const auto id=id_;QPointer<ScrollCaptureService> g(this);
 shell_->showFrame({{"rect",view_["rect"]},{"kind","region"},{"state","ready"},{"pid",double(backend_->processId(id))}},[g,id](Error e,QJsonObject r){
  if(!g)return;QMetaObject::invokeMethod(g,[g,id,e,r]{if(!g||g->id_!=id)return;
   auto start=[g,id](Error error,QJsonObject){if(!g)return;QMetaObject::invokeMethod(g,[g,id,error]{if(!g||g->id_!=id)return;if(error){g->terminate(error);return;}g->shellPlacing_=false;
    if(auto waiting=std::exchange(g->startWaiting_,{}))g->command("start",{{"id",id}},waiting);
    else if(g->shellAutoStart_)g->command("start",{{"id",id}},[](Error,QJsonObject){});},Qt::QueuedConnection);};
   // Outside: the helper rests the pointer on the bar between wheel steps, where it sees the user's motion (#1514).
   // Inside the rectangle's corner (no room beside it): shown all the same, its rows kept out of the image (#1519).
   const auto bar=r["bar"].toObject();const bool placed=!e&&bar["rect"].isArray()&&bar["rect"].toArray().size()==4;
   if(!placed)g->backend_->control(id,"overlay",{{"visible",false}},start);
   else g->backend_->control(id,"overlay",{{"visible",true},{"rect",bar["rect"]},{"inside",!bar["outside"].toBool()}},start);
  },Qt::QueuedConnection);
 });
}
void ScrollCaptureService::event(QJsonObject e){if(e.value("id")!=id_||id_.isEmpty()||cancelling_)return;const auto type=e.take("event").toString();if(type=="finished")complete(e);else if(type=="cancelled")terminate({},true);else if(type=="failed"){auto error=e["error"].toObject();terminate({error["code"].toString("scroll_failed"),error["message"].toString(),error["details"].toObject()});}else if(type=="selected"){view_["rect"]=e["rect"];update("ready");maybeStartHelper();}else if(type=="progress"){for(auto i=e.begin();i!=e.end();++i)if(i.key()!="paused"&&i.key()!="pausedBy"&&i.key()!="state"&&i.key()!="picture"&&i.key()!="serial")view_[i.key()]=i.value();
  // What the bar says, as one word: waiting, following, lost, scrollingToTop, autoScrolling, paused, resuming.
  if(e["state"].isString())view_["activity"]=e["state"];paused(e);update("capturing");}}
// #1515: who turns the page ("user", "putgd"), and why auto-scroll handed the
// wheel back by itself ("pointer": the user moved the mouse; "scrolled",
// "lost", "no_movement").
void ScrollCaptureService::paused(const QJsonObject& r){if(r["driver"].isString())view_["driver"]=r["driver"];view_["paused"]=r["paused"].toBool();if(view_["paused"].toBool()&&r["pausedBy"].isString())view_["pausedBy"]=r["pausedBy"];else view_.remove("pausedBy");}
void ScrollCaptureService::complete(QJsonObject r){
 const auto path=r["path"].toString();const auto bytes=r["bytes"].toInteger();const int w=r["width"].toInt(),h=r["height"].toInt();const auto reason=r["endReason"].toString();const int cap=r["appliedMaxHeight"].toInt();
 Error bad{"scroll_output_invalid","Scrolling capture output failed validation",{}};
 if(QFileInfo(path).absolutePath()!=directory_||bytes<33||bytes>op_.settings.limits.imageBytes||w<1||h<1||qint64(w)*h>op_.settings.limits.imagePixels||cap<1||cap>requestedCap_||h>cap||qint64(w)*h*4>op_.settings.limits.imageBytes||(edit_&&qint64(w)*h>EditorJob::admissionPixels)||!QStringList{"end_of_content","top_not_reached","height_cap","done","lost_track"}.contains(reason)||(r.contains("topReached")&&!r.value("topReached").isBool())||(reason=="top_not_reached"&&r.value("topReached")!=QJsonValue(false))||(reason=="end_of_content"&&!r.value("topReached").toBool(true))
  // How it was made (#1515), and what each end reason promises of it.
  ||!QStringList{"user","putgd","both","none"}.contains(r["scrolledBy"].toString())||!r["framesPlaced"].isDouble()||r["framesPlaced"].toInt()<1||!r["framesSkipped"].isDouble()||r["framesSkipped"].toInt()<0||!r["endedLost"].isBool()||!r["reachedEnd"].isBool()
  ||(reason=="end_of_content"&&(!r["reachedEnd"].toBool()||r["framesSkipped"].toInt()!=0||r["endedLost"].toBool()))||(reason=="lost_track"&&!r["endedLost"].toBool())||r.value("truncated")!=QJsonValue(reason=="height_cap"||reason=="lost_track"||reason=="top_not_reached")){terminate(bad);return;}
 const int fd=::open(QFile::encodeName(path).constData(),O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);struct stat st{};
 if(fd<0){terminate(bad);return;}QFile file;if(!file.open(fd,QIODevice::ReadOnly,QFileDevice::AutoCloseHandle)){::close(fd);terminate(bad);return;}
 if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_uid!=getuid()||st.st_size!=bytes){terminate(bad);return;}
 const auto png=file.read(bytes+1);if(png.size()!=bytes||!png.startsWith(QByteArray::fromHex("89504e470d0a1a0a0000000d49484452"))||qFromBigEndian<quint32>(png.constData()+16)!=quint32(w)||qFromBigEndian<quint32>(png.constData()+20)!=quint32(h)||QString::fromLatin1(QCryptographicHash::hash(png,QCryptographicHash::Sha256).toHex())!=r["sha256"].toString()){terminate(bad);return;}
 r.remove("path");view_.remove("paused");view_.remove("pausedBy");view_.remove("activity");view_["result"]=r;view_["endReason"]=reason;view_["truncated"]=r["truncated"];view_["height"]=h;
 for(const auto* key:{"scrolledBy","framesPlaced","framesSkipped","endedLost","reachedEnd"})view_[key]=r[key];view_["bytes"]=double(bytes);view_["stats"]=r.value("stats");const auto stats=r.value("stats").toObject();for(auto i=stats.begin();i!=stats.end();++i)view_[i.key()]=i.value();
 // The scroll-to-top step did not prove the top: said with every end reason, and the image is kept.
 if(r.contains("topReached"))view_["topReached"]=r.value("topReached");
 update("done");
 if(reason=="height_cap"){const auto note=QString("Stopped at %1 px. The page continues below.").arg(QLocale(QLocale::English).toString(cap));op_.note=note;view_["note"]=note;emit warning(id_,note,reason);}
 else if(reason=="top_not_reached"){const auto note=QString("Could not reach the top of the page. The image starts partway down.");op_.note=note;view_["note"]=note;emit warning(id_,note,reason);}
 else if(reason=="lost_track"){const auto note=QString("Finished while put.gd had lost its place. The image ends where it last found the page.");op_.note=note;view_["note"]=note;emit warning(id_,note,reason);}
 else if(r["note"].toString().startsWith("nobody scrolled")){const auto note=QString("Finished by itself after 10 minutes without scrolling.");op_.note=note;view_["note"]=note;}
 backend_->discard(id_);
 rememberResult();auto op=op_;const auto id=id_;id_.clear();pending_.clear();if(shell_)shell_->hideFrame();emit ended();
 ClipboardPayload payload;if(edit_){payload.kind="files";payload.files={path};}else{payload.kind="png";payload.bytes=png;payload.mime="image/png";}
 QPointer<ScrollCaptureService> g(this);host_.admit(op,std::move(payload),[g,id,path]{if(g)QMetaObject::invokeMethod(g,[g,id,path]{if(g){QFile::remove(path);QDir(g->root_+"/"+id).removeRecursively();}},Qt::QueuedConnection);});
}
void ScrollCaptureService::terminate(Error e,bool cancelled){if(!active())return;const auto id=id_;view_.remove("paused");view_.remove("pausedBy");view_.remove("activity");op_.error=e;view_["error"]=e.toJson();op_.cancelled=cancelled;op_.advance(cancelled?OperationState::Cancelled:OperationState::Failed);update(op_.stateName());rememberResult();id_.clear();pending_.clear();cancelling_=false;shellSelecting_=false;shellPlacing_=false;held_=false;if(shell_){shell_->cancelSelection();shell_->hideFrame();}backend_->discard(id);QDir(directory_).removeRecursively();host_.finish(op_);
 // A start that was waiting for the bar's placement ends with the session.
 if(auto waiting=std::exchange(startWaiting_,{}))waiting(e?e:Error{"cancelled","Scrolling capture ended",{}},{});
 emit ended();}
void ScrollCaptureService::rememberResult(){results_[id_]=view_;resultOrder_.append(id_);while(resultOrder_.size()>32)results_.remove(resultOrder_.takeFirst());}
void ScrollCaptureService::shutdown(){if(active())terminate({"cancelled","Resident stopped",{}},true);}
}
