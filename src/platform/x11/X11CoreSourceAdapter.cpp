#include "X11CoreSourceAdapter.h"
#include <QCoreApplication>
#include <QPointer>
#include <QJsonArray>
namespace putgd {
X11CoreSourceAdapter::X11CoreSourceAdapter(QObject* parent):QObject(parent){connect(&shortcuts_,&X11Shortcuts::activated,this,&X11CoreSourceAdapter::actionRequested);connect(&shortcuts_,&X11Shortcuts::bindingsChanged,this,&X11CoreSourceAdapter::capabilitiesChanged);}
X11CoreSourceAdapter::~X11CoreSourceAdapter(){helper_.cancel();clipboard_.cancel();}
quint64 X11CoreSourceAdapter::clipboardRevision()const{return clipboard_.revision();}
QJsonObject X11CoreSourceAdapter::capabilities()const{
 const QJsonObject capture{{"available",capture_.available()},{"mode","visible-pixels"},{"consent","none"},{"helper",helper_.diagnostics()}};
 return {{"adapter","cinnamon-core-x11"},{"capture",QJsonObject{{"region",capture},{"window",capture},{"desktop",capture}}},{"clipboard",QJsonObject{{"available",clipboard_.available()},{"types",QJsonArray{"files","image","text"}},{"import","asynchronous-x11-selection"},{"result","native-x11-ownership"}}},{"shortcuts",QJsonObject{{"available",shortcuts_.available()},{"bindings",shortcuts_.status()}}}};
}
SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& action){
 QPointer<X11CoreSourceAdapter> guard(this);
 if(action=="clipboard"){const auto revision=clipboard_.revision();return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> callback){if(guard)guard->clipboard_.acquire(limits,std::move(callback),revision);else callback({{},{"source_unavailable","Desktop adapter closed",{}}});};}
 if(helper_.busy())return [](const ResourceLimits&,std::function<void(PayloadResult)> callback){callback({{},{"busy_selecting","Region selection is already active",{}}});};
 Error error;QSet<qint64> excluded{QCoreApplication::applicationPid()};if(uiProcess_)excluded.insert(uiProcess_);if(helper_.processId())excluded.insert(helper_.processId());const auto frozen=capture_.freeze(action,excluded,error);
 return [guard,frozen,error](const ResourceLimits& limits,std::function<void(PayloadResult)> callback){if(error){callback({{},error});return;}if(!guard){callback({{},{"source_unavailable","Desktop adapter closed",{}}});return;}Error changed;if(!guard->capture_.unchanged(frozen,changed)){callback({{},changed});return;}guard->helper_.start(frozen,limits,std::move(callback));};
}
SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& action,const ResourceLimits& acceptedLimits){
 return prepareAcquisition(action,acceptedLimits,{});
}
SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& action,const ResourceLimits& acceptedLimits,std::function<void(Error)> earlyTerminal){
 return prepareAcquisition(action,acceptedLimits,std::move(earlyTerminal),{});
}
SourceAdapter::PreparedAcquisition X11CoreSourceAdapter::prepareAcquisition(const QString& action,const ResourceLimits& acceptedLimits,std::function<void(Error)> earlyTerminal,std::function<void()> earlyReleased){
 if(action=="clipboard"){auto acquisition=prepareAcquisition(action);if(earlyReleased)earlyReleased();return acquisition;}
 QPointer<X11CoreSourceAdapter> guard(this);
 if(helper_.busy()){if(earlyReleased)earlyReleased();return [](const ResourceLimits&,std::function<void(PayloadResult)> callback){callback({{},{"busy_selecting","Region selection is already active",{}}});};}
 Error error;QSet<qint64> excluded{QCoreApplication::applicationPid()};if(uiProcess_)excluded.insert(uiProcess_);if(helper_.processId())excluded.insert(helper_.processId());const auto frozen=capture_.freeze(action,excluded,error);
 if(error&&earlyReleased)earlyReleased();
 auto ticket=error?CaptureProcessController::Ticket{}:helper_.prepare(frozen,acceptedLimits,std::move(earlyTerminal),std::move(earlyReleased));
 return [guard,frozen,error,acceptedLimits,ticket](const ResourceLimits& limits,std::function<void(PayloadResult)> callback){
  if(error){callback({{},error});return;}
  if(!guard){callback({{},{"source_unavailable","Desktop adapter closed",{}}});return;}
  if(limits.imagePixels!=acceptedLimits.imagePixels||limits.imageBytes!=acceptedLimits.imageBytes||limits.maxFileBytes!=acceptedLimits.maxFileBytes){guard->helper_.discard(ticket);callback({{},{"input_excessive","Prepared capture limits changed",{}}});return;}
  Error changed;if(!guard->capture_.unchanged(frozen,changed)){guard->helper_.discard(ticket);callback({{},changed});return;}
  guard->helper_.begin(ticket,std::move(callback));
 };
}
void X11CoreSourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> callback){prepareAcquisition(action,limits)(limits,std::move(callback));}
void X11CoreSourceAdapter::cancelAcquisition(){helper_.cancel();}
void X11CoreSourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> callback){clipboard_.copy(payload,std::move(callback));}
void X11CoreSourceAdapter::bindShortcuts(const QHash<QString,QString>& bindings){shortcuts_.bind(bindings);}
}
