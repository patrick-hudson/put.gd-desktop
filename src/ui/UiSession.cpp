#include "UiSession.h"
#include <QPointer>
#include <QRunnable>
#include <utility>
namespace putgd {
UiSession::UiSession(SessionClient& client,QObject* parent):QObject(parent),client_(client){previews_.setMaxThreadCount(1);previews_.setExpiryTimeout(1000);}
UiSession::~UiSession(){previews_.clear();previews_.waitForDone();}
SessionClient::RequestId UiSession::request(const QString& method,const QJsonObject& params,QObject* receiver,SessionClient::Reply reply){return client_.request(method,params,receiver,std::move(reply));}
void UiSession::applySettings(const QJsonObject& settings){document_=settings["document"].toObject();metadata_=settings["metadata"].toObject();}
void UiSession::applySnapshot(const QJsonObject& data){const auto previous=revision();const auto wasReady=ready_;applySettings(data["settings"].toObject());diagnostics_=data["diagnostics"].toObject();build_=data["build"].toObject();epoch_=data["residentEpoch"].toString();ready_=data["operationsReady"].toBool();if(previous!=revision())emit accepted(revision());emit statusChanged();if(!wasReady&&ready_)emit initialized();emit operationsChanged();}
void UiSession::refreshSnapshot(std::function<void(Error)> callback){
 if(callback)snapshotCallbacks_.append(std::move(callback));
 if(snapshotActive_||mutations_){snapshotPending_=true;return;}
 snapshotActive_=true;snapshotPending_=false;const auto generation=generation_;
 request("ui.snapshot",{},this,[this,generation](Error e,QJsonObject data){snapshotActive_=false;if(generation!=generation_){snapshotPending_=true;refreshSnapshot();return;}if(!e)applySnapshot(data);else emit unavailable(e);auto callbacks=std::exchange(snapshotCallbacks_,{});for(auto& cb:callbacks)cb(e);if(snapshotPending_)refreshSnapshot();});
}
void UiSession::invalidate(){++generation_;refreshSnapshot();}
void UiSession::saveAsync(const QJsonObject& document,const QString& expected,std::function<void(Result)> callback){
 ++mutations_;++generation_;
 request("settings.save",{{"document",document},{"expectedRevision",expected}},this,[this,callback=std::move(callback)](Error e,QJsonObject data){const auto previous=revision();if(!e)applySettings(data);--mutations_;callback({e,!e}); // Clear the window's dirty flag before publishing our accepted revision.
 if(!e){if(previous!=revision())emit accepted(revision());emit statusChanged();}else snapshotPending_=true;
 if(snapshotPending_)refreshSnapshot();});
}
void UiSession::reloadAsync(std::function<void(Result)> callback){
 ++mutations_;++generation_;request("settings.reload",{},this,[this,callback=std::move(callback)](Error reloadError,QJsonObject){request("settings.get",{},this,[this,reloadError,callback](Error e,QJsonObject data){const auto previous=revision();if(!e)applySettings(data);--mutations_;callback({reloadError?reloadError:e,!reloadError&&!e});if(previous!=revision())emit accepted(revision());emit statusChanged();if(snapshotPending_)refreshSnapshot();});});
}
void UiSession::setShortcutRecording(bool active,QObject* receiver,std::function<void(Error)> reply){
 QPointer<QObject> guard(receiver);
 auto complete=[guard,reply=std::move(reply)](Error error){if(guard&&reply)reply(error);};
 if(!shortcutRecording_){complete({"shortcut_recording_unavailable","Shortcut recording requires the resident interface connection.",{}});return;}
 shortcutRecording_(active,std::move(complete));
}
void UiSession::background(std::function<QJsonObject(Error&)> work,QObject* receiver,SessionClient::Reply reply){
 QPointer<QObject> guard(receiver);previews_.start(QRunnable::create([this,guard,work=std::move(work),reply=std::move(reply)]{Error error;auto result=work(error);QMetaObject::invokeMethod(this,[guard,reply,error,result]{if(guard)reply(error,result);},Qt::QueuedConnection);}));
}
}
