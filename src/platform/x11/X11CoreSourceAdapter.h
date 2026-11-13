#pragma once
#include "core/SourceAdapter.h"
#include "X11NativeSnapshot.h"
#include "X11Clipboard.h"
#include "X11Shortcuts.h"
#include "CaptureProcessController.h"
#include "platform/CountdownProcess.h"
#include <optional>
namespace putgd {
class X11CoreSourceAdapter final:public QObject,public SourceAdapter {
 Q_OBJECT
public:
 explicit X11CoreSourceAdapter(QObject* parent=nullptr,QString helperPath={});
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
 // Delayed capture (#1363): putgd-capture's countdown mode draws the pill.
 void countdown(int seconds,std::function<void(Error)>)override;
 void sessionLocked(std::function<void(bool)>)override;
signals:
 void actionRequested(const QString&);
 void capabilitiesChanged();
 // The display's X server went away, as it does when its session ends.
 void displayLost();
private:
 bool crosshairHeld_=false;
 // Freezes the action's target; repeat needs the remembered region on an unchanged layout.
 QJsonObject freeze(const QString& action,Error&);
 // Remembers a region the helper captured, for Repeat last region (resident memory only).
 // A text capture (#1428) passes remember=false: it never changes that region.
 std::function<void(PayloadResult)> remembering(const QJsonObject& frozen,std::function<void(PayloadResult)>,bool remember=true);
 struct RegionMemory {QJsonArray selection,layout;};
 std::optional<RegionMemory> lastRegion_;
 CountdownProcess countdown_{QStringLiteral("putgd-capture")};
 X11NativeSnapshot capture_;X11Clipboard clipboard_;X11Shortcuts shortcuts_;CaptureProcessController helper_;qint64 uiProcess_=0;
};
}
