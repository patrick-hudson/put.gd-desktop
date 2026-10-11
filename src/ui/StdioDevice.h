#pragma once
#include <QIODevice>
class QSocketNotifier;
namespace putgd {
// Capture private parent pipes before QApplication loads plugins. Standard
// streams are then unavailable to the control protocol; plugin stdout goes to
// stderr. Direct terminal launches are rejected before changing their flags.
class UiControlPipes final {
public:
 UiControlPipes();
 ~UiControlPipes();
 UiControlPipes(const UiControlPipes&)=delete;
 UiControlPipes& operator=(const UiControlPipes&)=delete;
 bool valid()const{return input_>=0&&output_>=0;}
 int input()const{return input_;}
 int output()const{return output_;}
private:
 int input_=-1,output_=-1;
};
// Platform pipe adapter for the private helper. Service IPC remains SessionClient.
class StdioDevice final:public QIODevice {
 Q_OBJECT
public:
 explicit StdioDevice(int input=0,int output=1,QObject* parent=nullptr);
 ~StdioDevice() override;
 bool isSequential()const override{return true;}
 qint64 bytesAvailable()const override;
 bool atEnd()const override{return eof_;}
protected:
 qint64 readData(char*,qint64)override;
 qint64 writeData(const char*,qint64)override;
private:
 QSocketNotifier *reader_=nullptr,*writer_=nullptr;
 int input_=-1,output_=-1,inputFlags_=-1,outputFlags_=-1;
 bool eof_=false;
};
}
