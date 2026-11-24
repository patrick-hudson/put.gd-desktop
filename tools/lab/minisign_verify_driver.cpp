// A batch front end to putgd::minisign::verify for tools/lab/minisign_differential.py.
// stdin: one case per line, "<message-file> <signature-file> <public-key-file>" (paths without spaces).
// stdout: one line per case, "OK <trusted comment>" or "ERR <check>".
#include "cli/Minisign.h"
#include <QByteArray>
#include <QFile>
#include <iostream>
#include <string>
namespace {
QByteArray read(const std::string& path) {
 QFile file(QString::fromStdString(path));
 return file.open(QIODevice::ReadOnly)?file.readAll():QByteArray();
}
}
int main() {
 std::string message,signature,key;
 while(std::cin>>message>>signature>>key) {
  QString comment;
  const auto error=putgd::minisign::verify(read(message),read(signature),read(key),&comment);
  if(error)std::cout<<"ERR "<<error.details["check"].toString().toStdString()<<"\n";
  else std::cout<<"OK "<<comment.toUtf8().toStdString()<<"\n";
  std::cout.flush();
 }
}
