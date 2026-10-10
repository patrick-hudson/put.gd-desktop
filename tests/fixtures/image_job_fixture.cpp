#include "app/UiControl.h"
#include "ui/StdioDevice.h"
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTimer>
#include <csignal>
#include <cstdio>
#include <sys/resource.h>
#include <unistd.h>

using namespace putgd;
int main(int argc,char** argv) {
    const rlimit noCore{0,0};::setrlimit(RLIMIT_CORE,&noCore);
    QCoreApplication app(argc,argv);
    StdioDevice pipes;if(!pipes.isOpen())return 2;
    UiControl control(pipes);
    // These controls exist only in this test executable. The product helper
    // never reads them, and ImageJob selects this executable by constructor.
    const auto mode=qEnvironmentVariable("PUTGD_TEST_IMAGE_JOB_MODE");
    const auto recordPath=qEnvironmentVariable("PUTGD_TEST_IMAGE_JOB_RECORD");
    QJsonObject request,envelope;bool accepted=false;
    auto record=[&]{
        QFile file(recordPath);if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return;
        file.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
        file.write(QJsonDocument(QJsonObject{{"pid",qint64(::getpid())},
            {"jobDirectory",QFileInfo(request["output"].toObject()["path"].toString()).absolutePath()},
            {"accepted",accepted}}).toJson(QJsonDocument::Compact));
    };
    record();
    QObject::connect(&pipes,&QIODevice::readChannelFinished,&app,[&]{app.exit(5);});
    QObject::connect(&control,&UiControl::failed,&app,[&]{app.exit(3);});
    QObject::connect(&control,&UiControl::frame,&app,[&](const QString& type,const QString&,const QJsonObject& payload){
        if(type=="bootstrap"){
            request=payload;envelope={{"role",payload["role"]},{"nonce",payload["nonce"]},{"generation",payload["generation"]}};record();
            if(mode=="malformed"){std::fputs("{broken}\n",stdout);std::fflush(stdout);return;}
            if(mode=="version-mismatch"){
                std::fputs("{\"version\":2,\"type\":\"ready\",\"sequence\":\"1\",\"payload\":{}}\n",stdout);std::fflush(stdout);return;
            }
            if(mode=="partial-frame"){std::fputs("{",stdout);std::fflush(stdout);return;}
            auto ready=envelope;ready["build"]=payload["build"];
            if(mode=="stale-nonce")ready["nonce"]="00000000-0000-0000-0000-000000000000";
            if(mode=="stale-generation")ready["generation"]="2";
            if(mode=="wrong-role")ready["role"]="settings";
            if(mode=="build-mismatch")ready["build"]=QJsonObject{};
            control.send("ready",ready);
        }else if(type=="activate"){
            if(mode=="stall"||mode=="cancel-stall")return;
            if(mode=="exit-before-result"){app.exit(0);return;}
            if(mode=="error-result"){
                auto result=envelope;result["ok"]=false;result["code"]="source_unsupported";control.send("checkpoint",result);return;
            }
            const auto outputPath=request["output"].toObject()["path"].toString();
            QByteArray bytes;
            const bool discover=request["task"]=="discover";
            if(discover){
                bytes=mode=="invalid-discovery"?QByteArray("[1]"):
                    QJsonDocument(QJsonArray{"image/x-custom-first","application/x-vendor-image","image/png","image/x-custom-first"}).toJson(QJsonDocument::Compact);
            }else{
                QFile input(request["input"].toObject()["path"].toString());
                if(!input.open(QIODevice::ReadOnly)){app.exit(8);return;}bytes=input.readAll();
            }
            if(mode=="replace-output")QFile::rename(outputPath,outputPath+".original");
            QFile output(outputPath);if(!output.open(QIODevice::WriteOnly|QIODevice::Truncate)){app.exit(9);return;}
            output.setPermissions(QFile::ReadOwner|QFile::WriteOwner);
            if(output.write(bytes)!=bytes.size()){app.exit(10);return;}output.close();
            if(mode=="output-permissions")QFile::setPermissions(outputPath,QFile::ReadOwner|QFile::WriteOwner|QFile::ReadGroup|QFile::ReadOther);
            if(mode=="output-hardlink")::link(QFile::encodeName(outputPath).constData(),QFile::encodeName(outputPath+".link").constData());
            if(mode=="replace-input"){
                const auto path=request["input"].toObject()["path"].toString();
                if(!QFile::rename(path,path+".original")){app.exit(11);return;}
                QFile replacement(path);if(!replacement.open(QIODevice::WriteOnly)){app.exit(12);return;}
                replacement.write(bytes);replacement.setPermissions(QFile::ReadOwner);replacement.close();
            }
            auto result=envelope;result["ok"]=true;
            result["bytes"]=QString::number(bytes.size()+(mode=="wrong-size"?1:0));
            result["sha256"]=mode=="wrong-hash"?QString(64,'0'):QString::fromLatin1(QCryptographicHash::hash(bytes,QCryptographicHash::Sha256).toHex());
            result["width"]=discover?0:32;result["height"]=discover?0:16;
            if(mode=="stale-result")result["nonce"]="old-job";
            control.send("checkpoint",result);
        }else if(type=="checkpointAccepted"){
            accepted=true;record();
            if(mode=="no-exit")return;
            if(mode=="crash-after-result"){::raise(SIGKILL);return;}
            if(mode=="duplicate-result"){
                auto result=envelope;result["ok"]=false;result["code"]="source_unsupported";control.send("checkpoint",result);return;
            }
            QTimer::singleShot(mode=="delayed-exit"?200:10,&app,[&]{app.exit(mode=="nonzero-after-result"?17:0);});
        }
    });
    return app.exec();
}
