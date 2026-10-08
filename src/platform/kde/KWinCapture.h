#pragma once
#include "platform/Geometry.h"
#include "core/Settings.h"
#include <QObject>
#include <QJsonObject>
#include <QVariantMap>
#include <QDBusServiceWatcher>
#include <QPointer>
#include <QThread>
namespace putgd {
class KWinCapture final:public QObject {
 Q_OBJECT
public:
 using Reply=std::function<void(QImage,QVariantMap,Error)>;
 explicit KWinCapture(QObject* parent=nullptr);
 ~KWinCapture() override;
 void probe();bool available()const{return version_>=4&&entryMatches_&&!permissionDenied_;}uint version()const{return version_;}
 QJsonObject status()const;
 void request(const QString& method,const QVariantList& args,const ResourceLimits&,Reply);
 void cancel();
signals:
 void changed();
private:
 quint64 probeGeneration_=0;bool entryMatches_=false,permissionDenied_=false;uint version_=0;QString error_="ScreenShot2 has not been probed";
 QDBusServiceWatcher watcher_;QList<QObject*> requests_;QList<QThread*> workers_;
};
}
