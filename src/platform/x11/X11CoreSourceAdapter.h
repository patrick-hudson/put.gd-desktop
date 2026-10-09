#pragma once
#include "app/OperationService.h"
#include "X11NativeSnapshot.h"
#include "X11Clipboard.h"
#include "X11Shortcuts.h"
#include "CaptureProcessController.h"
namespace putgd {
class X11CoreSourceAdapter final:public QObject,public SourceAdapter {
 Q_OBJECT
public:
 explicit X11CoreSourceAdapter(QObject* parent=nullptr);
 ~X11CoreSourceAdapter() override;
 quint64 clipboardRevision()const override;
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&)override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override;
 void cancelAcquisition()override;
 void copy(const CopyPayload&,std::function<void(Error)>)override;
 void bindShortcuts(const QHash<QString,QString>&);
 void setUiProcess(qint64 pid)override{uiProcess_=pid>0?pid:0;}
signals:
 void actionRequested(const QString&);
 void capabilitiesChanged();
private:
 X11NativeSnapshot capture_;X11Clipboard clipboard_;X11Shortcuts shortcuts_;CaptureProcessController helper_;qint64 uiProcess_=0;
};
}
