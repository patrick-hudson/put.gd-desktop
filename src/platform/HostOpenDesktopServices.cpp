#include "platform/HostOpen.h"
#include <QDesktopServices>
#include <QFileInfo>

// Other platforms have no AppImage environment to undo: Qt's own opener.
namespace putgd::host {
namespace {
std::function<void(const QUrl&)> testOpener;
void opened(const Opened& done,const QUrl& target){
 if(testOpener){testOpener(target);if(done)done({{"route","test"},{"opened",true}});return;}
 const bool ok=QDesktopServices::openUrl(target);if(done)done({{"route","desktop-services"},{"opened",ok}});
}
}
void setOpenTimeoutsMs(int,int){}
void setTestOpener(std::function<void(const QUrl&)> opener){testOpener=std::move(opener);}
void openLink(const QUrl& link,Opened done){
 if(!link.isValid()||(link.scheme()!="https"&&link.scheme()!="http")){if(done)done({{"route","none"},{"opened",false},{"error","unsupported_link"}});return;}
 opened(done,link);
}
void showInFolder(const QString& path,Opened done){opened(done,QUrl::fromLocalFile(QFileInfo(path).absolutePath()));}
void openFile(const QString& path,Opened done){opened(done,QUrl::fromLocalFile(path));}
}
