#include "X11CoreSourceAdapter.h"
#include "platform/SessionLock.h"
#include <QCoreApplication>
#include <QPointer>
#include <QJsonArray>
#include <utility>
namespace putgd {
X11CoreSourceAdapter::X11CoreSourceAdapter(QObject* parent,QString helperPath):QObject(parent),helper_(std::move(helperPath)){connect(&shortcuts_,&X11Shortcuts::escapePressed,this,[this]{
  // Selection is not up yet, so nothing else can take this: the keyboard is free and
  // the focused window receives the key too, as it does before any selector shows.
  if(helper_.crosshairHeld())helper_.cancel();
 });connect(&shortcuts_,&X11Shortcuts::pressed,this,[this](const QString& action){
  // First thing at the press, before logging, settings, freezing or the helper's load.
  crosshairHeld_=(action=="region"||action=="edit"||action=="text")&&!regionScreenshotFirst_&&helper_.holdCrosshair();
 });connect(&shortcuts_,&X11Shortcuts::activated,this,[this](const QString& action){
  emit actionRequested(action);
  if(std::exchange(crosshairHeld_,false))helper_.releaseUnclaimedCrosshair();
 });connect(&shortcuts_,&X11Shortcuts::bindingsChanged,this,&X11CoreSourceAdapter::capabilitiesChanged);connect(&shortcuts_,&X11Shortcuts::connectionLost,this,&X11CoreSourceAdapter::displayLost);}
X11CoreSourceAdapter::~X11CoreSourceAdapter(){helper_.cancel();clipboard_.cancel();}
QJsonObject X11CoreSourceAdapter::freeze(const QString& action,Error& error){
 QSet<qint64> excluded{QCoreApplication::applicationPid()};if(uiProcess_)excluded.insert(uiProcess_);if(helper_.processId())excluded.insert(helper_.processId());
 if(action!="repeat")return capture_.freeze(action,excluded,error);
 if(!lastRegion_){error=noLastRegion();return {};}
 // Outputs, native rectangles and rotations are checked here; the helper, which
 // knows Qt's logical rectangles, checks the rest before it reads anything.
 if(!capture_.sameNativeOutputs(lastRegion_->layout,error)){if(!error)error=lastRegionLayoutChanged();return {};}
 return capture_.freeze(action,excluded,error,lastRegion_->selection,lastRegion_->layout);
}
std::function<void(PayloadResult)> X11CoreSourceAdapter::remembering(const QJsonObject& frozen,std::function<void(PayloadResult)> callback,bool remember){
 if(!remember||frozen["action"]!="region")return callback;
 QPointer<X11CoreSourceAdapter> guard(this);
 return [guard,callback=std::move(callback)](PayloadResult result)mutable{
  if(guard&&!result.error)if(const auto selected=guard->helper_.selection())guard->lastRegion_=RegionMemory{QJsonArray{selected->x(),selected->y(),selected->width(),selected->height()},guard->helper_.selectionLayout()};
  callback(std::move(result));
 };
}
void X11CoreSourceAdapter::countdown(int seconds,std::function<void(Error)> reply){
 if(helper_.busy()){reply({"busy_selecting","Region selection is already active",{}});return;}
 countdown_.start(seconds,captureAppearance_,std::move(reply));
}
void X11CoreSourceAdapter::sessionLocked(std::function<void(bool)> reply){querySessionLocked(this,std::move(reply));}
quint64 X11CoreSourceAdapter::clipboardRevision()const{return clipboard_.revision();}
QJsonObject X11CoreSourceAdapter::capabilities()const{
 const QJsonObject capture{{"available",capture_.available()},{"mode","visible-pixels"},{"consent","none"},{"helper",helper_.diagnostics()}};
 auto repeat=capture;repeat["remembered"]=lastRegion_.has_value();
 return {{"adapter","cinnamon-core-x11"},{"capture",QJsonObject{{"region",capture},{"text",capture},{"window",capture},{"desktop",capture},{"monitor",capture},{"repeat",repeat}}},{"delay",QJsonObject{{"available",capture_.available()},{"seconds",QJsonArray{3,5,10}},{"indicator","helper-window"}}},{"clipboard",QJsonObject{{"available",clipboard_.available()},{"types",QJsonArray{"files","image","text"}},{"import","asynchronous-x11-selection"},{"result","native-x11-ownership"}}},{"shortcuts",QJsonObject{{"available",shortcuts_.available()},{"bindings",shortcuts_.status()},{"keyPaths",shortcuts_.keyPaths()}}}};
}
SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& requested){
 // Copy text (#1428) selects exactly as a region does; only Repeat's memory is left alone.
 const bool text=requested==QLatin1String("text");const QString action=text?QStringLiteral("region"):requested;
 QPointer<X11CoreSourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> callback){if(guard)guard->clipboard_.acquire(limits,std::move(callback),revision);else callback({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 if(helper_.busy())return [](const ResourceLimits&,std::function<void(PayloadResult)> callback){callback({{},{"busy_selecting","Region selection is already active",{}}});};
 Error error;const auto frozen=freeze(action,error);
 return [guard,frozen,error,text,outcome=captureOutcome_,appearance=captureAppearance_,screenshot=action=="region"&&regionScreenshotFirst_,delay=captureDelay_](const ResourceLimits& limits,std::function<void(PayloadResult)> callback){if(error){callback({{},error});return;}if(!guard){callback({{},{"source_unavailable","Desktop adapter closed",{}}});return;}Error changed;if(!guard->capture_.unchanged(frozen,changed)){callback({{},changed});return;}guard->helper_.start(frozen,limits,guard->remembering(frozen,std::move(callback),!text),outcome,screenshot,appearance,delay);};
}
SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& action,const ResourceLimits& acceptedLimits){
 return prepareAcquisition(action,acceptedLimits,{});
}
SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& action,const ResourceLimits& acceptedLimits,std::function<void(Error)> earlyTerminal){
 return prepareAcquisition(action,acceptedLimits,std::move(earlyTerminal),{});
}
SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& requested,const ResourceLimits& acceptedLimits,std::function<void(Error)> earlyTerminal,std::function<void()> earlyReleased){
 const bool text=requested==QLatin1String("text");const QString action=text?QStringLiteral("region"):requested;
 if(action=="clipboard"){auto acquisition=prepareAcquisition(action);if(earlyReleased)earlyReleased();return acquisition;}
 QPointer<X11CoreSourceAdapter> guard(this);
 if(helper_.busy()){if(earlyReleased)earlyReleased();return [](const ResourceLimits&,std::function<void(PayloadResult)> callback){callback({{},{"busy_selecting","Region selection is already active",{}}});};}
 Error error;const auto frozen=freeze(action,error);
 if(error&&earlyReleased)earlyReleased();
 auto ticket=error?CaptureProcessController::Ticket{}:helper_.prepare(frozen,acceptedLimits,std::move(earlyTerminal),std::move(earlyReleased),captureOutcome_,action=="region"&&regionScreenshotFirst_,captureAppearance_,captureDelay_);
 return [guard,frozen,error,acceptedLimits,ticket,text](const ResourceLimits& limits,std::function<void(PayloadResult)> callback){
  if(error){callback({{},error});return;}
  if(!guard){callback({{},{"source_unavailable","Desktop adapter closed",{}}});return;}
  if(limits.imagePixels!=acceptedLimits.imagePixels||limits.imageBytes!=acceptedLimits.imageBytes||limits.maxFileBytes!=acceptedLimits.maxFileBytes){guard->helper_.discard(ticket);callback({{},{"input_excessive","Prepared capture limits changed",{}}});return;}
  Error changed;if(!guard->capture_.unchanged(frozen,changed)){guard->helper_.discard(ticket);callback({{},changed});return;}
  guard->helper_.begin(ticket,guard->remembering(frozen,std::move(callback),!text));
 };
}
void X11CoreSourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> callback){prepareAcquisition(action,limits)(limits,std::move(callback));}
void X11CoreSourceAdapter::cancelAcquisition(){countdown_.cancel();helper_.cancel();}
void X11CoreSourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> callback){clipboard_.copy(payload,std::move(callback));}
void X11CoreSourceAdapter::bindShortcuts(const QHash<QString,QString>& bindings){shortcuts_.bind(bindings);}
}
