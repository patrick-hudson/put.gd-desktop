#pragma once
#include "core/Error.h"
#include <QIODevice>
#include <QPointer>
#include <QElapsedTimer>
namespace putgd {
// Bounded private child pipes, containing descriptors only, never image bytes.
class CaptureControl final:public QObject {
 Q_OBJECT
public:
 static constexpr qsizetype maximumFrameBytes=128*1024;
 explicit CaptureControl(QIODevice&,QObject* parent=nullptr);
 bool send(const QString&,const QJsonObject&);
 void stop();
signals:
 void frame(const QString&,const QJsonObject&);
 void failed(putgd::Error);
private:
 void read();void write();void fail();
 QPointer<QIODevice> device_;QByteArray input_,output_;quint64 sent_=0,received_=0;
 QElapsedTimer rate_;qint64 rateBytes_=0;int rateFrames_=0;bool stopped_=false,writing_=false;
};
}
