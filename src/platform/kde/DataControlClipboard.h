#pragma once
#include "app/OperationService.h"
#include <QObject>
#include <memory>
namespace putgd {
class DataControlClipboard final:public QObject {
 Q_OBJECT
public:
 explicit DataControlClipboard(QObject* parent=nullptr);
 ~DataControlClipboard() override;
 bool available()const;quint64 revision()const;QJsonObject status()const;
 void acquire(const ResourceLimits&,std::function<void(PayloadResult)>,quint64 expectedRevision);
 void copy(const CopyPayload&,std::function<void(Error)>);
 void cancel();
 void invalidateCopyEligibility();
signals:
 void changed();
private:
 struct Impl;std::unique_ptr<Impl> d_;
};
}
