#pragma once
#include <QIODevice>
class QSocketNotifier;
namespace putgd {
// Platform pipe adapter for the private helper. Service IPC remains SessionClient.
class StdioDevice final:public QIODevice {
 Q_OBJECT
public:
 explicit StdioDevice(QObject* parent=nullptr);
 ~StdioDevice() override;
 bool isSequential()const override{return true;}
 qint64 bytesAvailable()const override;
 bool atEnd()const override{return eof_;}
protected:
 qint64 readData(char*,qint64)override;
 qint64 writeData(const char*,qint64)override;
private:
 QSocketNotifier *reader_=nullptr,*writer_=nullptr;
 int inputFlags_=-1,outputFlags_=-1;
 bool eof_=false;
};
}
