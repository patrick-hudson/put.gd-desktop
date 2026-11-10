#pragma once
#include "ScrollBackend.h"
#include "RecordingService.h"
#include <QHash>
namespace putgd {
// One private scroll session. The operation journal is committed before the
// backend may acquire pixels. Terminal descriptors remain inspectable.
class ScrollCaptureService final : public QObject {
 Q_OBJECT
public:
 using Host = RecordingService::Host;
 ScrollCaptureService(QString root, Host, QObject* parent=nullptr);
 ~ScrollCaptureService() override;
 void setBackend(std::unique_ptr<ScrollBackend>);
 void setShell(RecordingShell* shell) { shell_=shell; }
 bool active() const { return !id_.isEmpty(); }
 QString activeId() const { return id_; }
 QJsonObject capabilities() const;
 void command(const QString&,const QJsonObject&,JsonReply);
 void invoke(bool edit,JsonReply);
 void shutdown();
signals:
 void warning(const QString& id,const QString& message);
private:
 void prepare(QJsonObject,bool edit,bool autoStart,JsonReply);
 void prepareProbed(QJsonObject,bool,bool,JsonReply);
 void placeShellBar();
 void event(QJsonObject);
 void complete(QJsonObject);
 void rememberResult();
 void terminate(Error,bool cancelled=false);
 void update(QString);
 QString root_,id_,directory_,pending_;
 Host host_;
 Operation op_;
 QJsonObject view_;
 QHash<QString,QJsonObject> results_;
 QStringList resultOrder_;
 std::unique_ptr<ScrollBackend> backend_;
 RecordingShell* shell_=nullptr;
 int requestedCap_=30000;
 bool shellPlacing_=false;
 bool shellAutoStart_=false;
 bool shellSelecting_=false;
 bool edit_=false;
};
}
