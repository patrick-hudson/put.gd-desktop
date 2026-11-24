#pragma once
#include <QIODevice>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstring>
namespace putgd::test {
// The in-process end of a UiControl pipe: a test sends frames in with send() and
// reads what the application wrote back from `output`. With a nonzero
// `outputLimit` a long run keeps only the last 64 KiB once the output passes it.
class ControlDevice final:public QIODevice {
public:
 QByteArray input,output;quint64 sequence=0;
 explicit ControlDevice(qint64 outputLimit=0):outputLimit_(outputLimit){open(ReadWrite|Unbuffered);}
 bool isSequential()const override{return true;}
 qint64 bytesAvailable()const override{return input.size();}
 qint64 readData(char* data,qint64 max)override{const auto n=qMin(max,qint64(input.size()));memcpy(data,input.constData(),n);input.remove(0,n);return n;}
 qint64 writeData(const char* data,qint64 n)override{output.append(data,n);if(outputLimit_>0&&output.size()>outputLimit_)output=output.right(1<<16);return n;}
 void send(const QString& type,QJsonObject payload={}){input+=QJsonDocument(QJsonObject{{"version",1},{"type",type},{"sequence",QString::number(++sequence)},{"payload",payload}}).toJson(QJsonDocument::Compact)+'\n';emit readyRead();}
 // The last frame of this type the application wrote.
 QJsonObject last(const QString& type){QJsonObject result;for(const auto& line:output.split('\n')){const auto frame=QJsonDocument::fromJson(line).object();if(frame["type"]==type)result=frame;}return result;}
private:
 qint64 outputLimit_;
};
}
