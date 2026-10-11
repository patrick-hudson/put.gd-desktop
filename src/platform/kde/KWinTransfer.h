#pragma once
#include "core/Settings.h"
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <QImage>
#include <QVariantMap>
namespace putgd {
// Owns the read fd, never waits synchronously, and completes exactly once.
class AsyncFdReader final:public QObject {
public:
 using Reply=std::function<void(QByteArray,Error)>;
 explicit AsyncFdReader(QObject* parent=nullptr):QObject(parent){}
 ~AsyncFdReader() override;
 void start(int fd,qint64 bound,int timeoutMs,Reply);
 void cancel();
 void setBound(qint64 bound);
private:
 void drain();void finish(Error={});
 int fd_=-1;qint64 bound_=0;QByteArray bytes_;Reply reply_;
 QSocketNotifier* notifier_=nullptr;QTimer timer_;
};
struct KWinImageLayout {int width=0,height=0,stride=0;QImage::Format format=QImage::Format_Invalid;qreal scale=1;qint64 bytes=0;};
KWinImageLayout validateKWinMetadata(const QVariantMap&,const ResourceLimits&,Error&);
QImage decodeKWinImage(const QVariantMap&,const QByteArray&,const ResourceLimits&,Error&);
}
