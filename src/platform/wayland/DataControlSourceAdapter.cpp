#include "DataControlSourceAdapter.h"
#include <QJsonArray>
#include <QPointer>
namespace putgd {
namespace {
const auto captureUnavailable="Capture is not available on this desktop";
}
DataControlSourceAdapter::DataControlSourceAdapter(QObject* parent):QObject(parent) {
 connect(&clipboard_,&DataControlClipboard::changed,this,[this]{
  if(!connected_&&clipboard_.available()){connected_=true;connectedRevision_=clipboard_.revision();}
  emit capabilitiesChanged();
 });
}
quint64 DataControlSourceAdapter::clipboardRevision()const {
 // Reading drains compositor events first, which may set the baseline.
 const auto revision=clipboard_.revision();
 return connected_?revision-connectedRevision_:0;
}
std::optional<quint64> DataControlSourceAdapter::lastCopyRevision()const {
 const auto revision=clipboard_.lastCopyRevision();
 if(!revision)return {};
 return *revision-connectedRevision_;
}
QJsonObject DataControlSourceAdapter::capabilities()const {
 const QJsonObject capture{{"available",false},{"error",captureUnavailable}};
 return {{"adapter","wayland-data-control"},{"capture",QJsonObject{{"region",capture},{"window",capture},{"desktop",capture}}},{"clipboard",clipboard_.status()},{"shortcuts",QJsonObject{{"available",false}}}};
}
Error DataControlSourceAdapter::unavailable()const {
 return {"source_unsupported",clipboard_.status()["error"].toString(),{}};
}
SourceAdapter::PreparedAcquisition DataControlSourceAdapter::prepareAcquisition(const QString& action) {
 if(action!="clipboard")return [](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},{"source_unsupported",captureUnavailable,{}}});};
 const auto revision=clipboard_.revision();
 if(!clipboard_.available())return [error=unavailable()](const ResourceLimits&,std::function<void(PayloadResult)> cb){cb({{},error});};
 QPointer<DataControlSourceAdapter> guard(this);
 return [guard,revision](const ResourceLimits& limits,std::function<void(PayloadResult)> cb){if(guard)guard->clipboard_.acquire(limits,std::move(cb),revision);else cb({{},{"source_unavailable","Desktop adapter closed",{}}});};
}
void DataControlSourceAdapter::acquire(const QString& action,const ResourceLimits& limits,std::function<void(PayloadResult)> cb) {prepareAcquisition(action)(limits,std::move(cb));}
void DataControlSourceAdapter::cancelAcquisition() {clipboard_.cancel();}
void DataControlSourceAdapter::copy(const CopyPayload& payload,std::function<void(Error)> cb) {
 clipboard_.revision();
 if(!clipboard_.available()){cb(unavailable());return;}
 clipboard_.copy(payload,std::move(cb));
}
void DataControlSourceAdapter::invalidateCopyEligibility() {clipboard_.invalidateCopyEligibility();}
}
