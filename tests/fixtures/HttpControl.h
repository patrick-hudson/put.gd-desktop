#pragma once
#include <QByteArray>
#include <QElapsedTimer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTcpSocket>
#include <QUrl>
#include <optional>
namespace putgd::test {
// One request to a Python fixture server's control endpoints (/__reset, /__rules,
// /__state) at `base`, e.g. "http://127.0.0.1:PORT". Answers the JSON body, or
// nothing when the server does not answer within five seconds, so a test that owns
// processes can fail and still clean them up.
inline std::optional<QJsonObject> tryHttpControl(const QString& base,const QString& path,const QJsonObject& value={},bool post=false) {
 QTcpSocket socket;
 const QUrl address(base);
 socket.connectToHost(address.host(),quint16(address.port()));
 if(!socket.waitForConnected(5000))return std::nullopt;
 const auto body=post?QJsonDocument(value).toJson(QJsonDocument::Compact):QByteArray{};
 const auto request=(post?QByteArray("POST "):QByteArray("GET "))+path.toUtf8()+" HTTP/1.1\r\nHost: "+address.authority().toUtf8()+
                    "\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: "+QByteArray::number(body.size())+"\r\n\r\n"+body;
 socket.write(request);
 QByteArray response;
 QElapsedTimer deadline;
 deadline.start();
 while(deadline.elapsed()<5000) {
  if(!socket.bytesAvailable())socket.waitForReadyRead(100);
  response+=socket.readAll();
  const auto boundary=response.indexOf("\r\n\r\n");
  if(boundary<0)continue;
  qint64 length=-1;
  for(auto header:response.left(boundary).split('\n'))
   if(header.trimmed().toLower().startsWith("content-length:"))length=header.trimmed().mid(15).trimmed().toLongLong();
  if(length>=0&&response.size()-boundary-4>=length)return QJsonDocument::fromJson(response.mid(boundary+4,length)).object();
 }
 return std::nullopt;
}
// The same, ending the test run when the server does not answer.
inline QJsonObject httpControl(const QString& base,const QString& path,const QJsonObject& value={},bool post=false) {
 const auto answer=tryHttpControl(base,path,value,post);
 if(!answer)qFatal("fixture control request failed");
 return *answer;
}
}
