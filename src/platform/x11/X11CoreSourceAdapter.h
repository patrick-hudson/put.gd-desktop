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
 std::optional<quint64> lastCopyRevision()const override{return clipboard_.lastCopyRevision();}
 QJsonObject capabilities()const override;
 PreparedAcquisition prepareAcquisition(const QString&)override;
 PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&)override;
 PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)>)override;
 PreparedAcquisition prepareAcquisition(const QString&,const ResourceLimits&,std::function<void(Error)>,std::function<void()>)override;
 void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)override;
 void cancelAcquisition()override;
 void copy(const CopyPayload&,std::function<void(Error)>)override;
 void setShortcutRecording(bool active,std::function<void(Error)> reply){shortcuts_.setRecording(active,std::move(reply));}
 void bindShortcuts(const QHash<QString,QString>&);
 void setUiProcess(qint64 pid)override{uiProcess_=pid>0?pid:0;}
signals:
 void actionRequested(const QString&);
 void capabilitiesChanged();
 // The display's X server went away, as it does when its session ends.
 void displayLost();
private:
 X11NativeSnapshot capture_;X11Clipboard clipboard_;X11Shortcuts shortcuts_;CaptureProcessController helper_;qint64 uiProcess_=0;
};
}
