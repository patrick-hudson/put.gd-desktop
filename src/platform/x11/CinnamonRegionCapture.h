#pragma once
#include "core/Settings.h"
#include "platform/Geometry.h"
#include <QObject>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
class QSocketNotifier;
class QThread;
namespace putgd {
// One native capture owns its private outputs and decoders until they settle.
class CinnamonRegionCapture final:public QObject {
 Q_OBJECT
public:
 CinnamonRegionCapture(QList<CaptureFrame>,ResourceLimits,QObject* parent=nullptr);
 ~CinnamonRegionCapture() override;
 void start();
 void cancel();
signals:
 void acquired(QList<CaptureFrame>,const Error&);
private:
 void completedFiles();
 void decode(int,const QString&);
 void finish(Error);
 void cleanFiles();
 QList<CaptureFrame> frames_;
 ResourceLimits limits_;
 QTemporaryDir directory_;
 QTimer deadline_;
 QSocketNotifier* notifier_=nullptr;
 QList<QThread*> workers_;
 QSet<QString> pending_;
 int fd_=-1,remaining_=0;
 bool active_=true;
};
}
