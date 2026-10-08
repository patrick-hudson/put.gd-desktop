#pragma once
#include "core/Error.h"
#include <QElapsedTimer>
#include <QIODevice>
#include <QObject>
#include <QPointer>
namespace putgd {
// Private framing on the supervised helper's pipes; never a service command API.
class UiControl final : public QObject {
 Q_OBJECT
public:
 static constexpr qsizetype maximumFrameBytes=10*1024*1024;
 explicit UiControl(QIODevice&,QObject* parent=nullptr);
 QString send(const QString& type,const QJsonObject& payload={});
 void stop();
signals:
 void frame(const QString& type,const QString& sequence,const QJsonObject& payload);
 void failed(putgd::Error error);
private:
 void read();void write();void fail(const QString& code);
 QPointer<QIODevice> device_;
 QByteArray input_,output_;
 quint64 sent_=0,received_=0;
 QElapsedTimer rate_;qint64 receivedBytes_=0;
 bool stopped_=false,writing_=false;int receivedFrames_=0;
};
}
