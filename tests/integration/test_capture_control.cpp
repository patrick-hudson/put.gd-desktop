#include "platform/x11/CaptureControl.h"
#include "platform/x11/X11NativeSnapshot.h"
#include <QTest>
#include <QJsonDocument>
#include <QJsonArray>
#include <cstring>
class Duplex final:public QIODevice {
public:
 Duplex(){open(ReadWrite);}
 QByteArray input,output;
 void feed(QByteArray bytes){input+=bytes;emit readyRead();}
 qint64 bytesAvailable()const override{return input.size()+QIODevice::bytesAvailable();}
 bool isSequential()const override{return true;}
protected:
 qint64 readData(char* target,qint64 n)override{const auto count=qMin(n,qint64(input.size()));std::memcpy(target,input.constData(),size_t(count));input.remove(0,count);return count;}
 qint64 writeData(const char* source,qint64 n)override{output.append(source,n);return n;}
};
QJsonObject frozen(){return {{"schemaVersion",1},{"action","desktop"},{"window","0"},{"windowRect",QJsonArray{}},{"layout",QJsonObject{{"outputs",QJsonArray{QJsonObject{{"identity","monitor"},{"native",QJsonArray{0,0,640,480}},{"rotation",1}}}},{"configuration","1"},{"root",QJsonArray{0,0,640,480}},{"scale",QString(64,'a')}}}};}
class CaptureControlTests final:public QObject {
 Q_OBJECT
private slots:
 void fragmentedFrameAndSequence(){Duplex pipe;putgd::CaptureControl control(pipe);int frames=0,failures=0;connect(&control,&putgd::CaptureControl::frame,this,[&](const QString& type,const QJsonObject& o){QCOMPARE(type,QString("ready"));QCOMPARE(o["nonce"].toString(),QString("nonce"));++frames;});connect(&control,&putgd::CaptureControl::failed,this,[&](putgd::Error){++failures;});const QByteArray frame="{\"version\":1,\"sequence\":\"1\",\"type\":\"ready\",\"payload\":{\"nonce\":\"nonce\"}}\n";pipe.feed(frame.left(12));QCOMPARE(frames,0);pipe.feed(frame.mid(12));QCOMPARE(frames,1);pipe.feed(frame);QCOMPARE(failures,1);QCOMPARE(frames,1);}
 void stoppingInsideDeliveryDiscardsFollowingBufferedCommand(){
  Duplex pipe;putgd::CaptureControl control(pipe);int delivered=0;
  connect(&control,&putgd::CaptureControl::frame,this,[&]{++delivered;control.stop();});
  const QByteArray batch="{\"version\":1,\"sequence\":\"1\",\"type\":\"begin\",\"payload\":{}}\n{\"version\":1,\"sequence\":\"2\",\"type\":\"begin\",\"payload\":{}}\n";
  pipe.feed(batch);QCOMPARE(delivered,1);QVERIFY(!control.send("ready",{}));
 }
 void excessiveFrameStopsBeforeDelivery(){Duplex pipe;putgd::CaptureControl control(pipe);int failed=0,frames=0;connect(&control,&putgd::CaptureControl::failed,this,[&](putgd::Error){++failed;});connect(&control,&putgd::CaptureControl::frame,this,[&]{++frames;});pipe.feed(QByteArray(putgd::CaptureControl::maximumFrameBytes,'x'));QCOMPARE(failed,1);QCOMPARE(frames,0);QVERIFY(!control.send("ready",{}));}
 void unknownTypesCannotBecomeCommands(){Duplex pipe;putgd::CaptureControl control(pipe);pipe.feed("{\"version\":1,\"sequence\":\"1\",\"type\":\"delete\",\"payload\":{}}\n");QVERIFY(!control.send("ready",{}));QVERIFY(pipe.output.isEmpty());}
 void validSnapshotRejectsMalformedOrDuplicateIdentity(){auto value=frozen();QVERIFY(!putgd::X11NativeSnapshot::validate(value));value["window"]="4294967296";QVERIFY(putgd::X11NativeSnapshot::validate(value));value=frozen();value["action"]="window";QVERIFY(putgd::X11NativeSnapshot::validate(value));value=frozen();auto layout=value["layout"].toObject();auto outputs=layout["outputs"].toArray();outputs.append(outputs.first());layout["outputs"]=outputs;value["layout"]=layout;QVERIFY(putgd::X11NativeSnapshot::validate(value));value=frozen();layout=value["layout"].toObject();layout["scale"]=QString(64,'z');value["layout"]=layout;QVERIFY(putgd::X11NativeSnapshot::validate(value));}
};
QTEST_GUILESS_MAIN(CaptureControlTests)
#include "test_capture_control.moc"
