#pragma once
#include "core/Settings.h"
#include <QObject>
#include <QSocketNotifier>
#include <QTimer>
#include <functional>
namespace putgd {
// Owns the read fd, never waits synchronously, and completes at most once:
// destroying a pending reader drops its reply without invoking it.
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
}
