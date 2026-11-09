#pragma once
#include "core/Error.h"
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <functional>
namespace putgd {
// Delayed capture (#1363) on the residents that own no window: the countdown is
// drawn by a capture helper started in its countdown mode (putgd-capture on
// X11, putgd-region on layer-shell compositors). That mode reads no pixels,
// takes no target and no keyboard: it shows the pill, and exits once the pill
// is gone from the screen. Exit status is the whole protocol: 0 the delay ran
// out, cancelledExit the user clicked the pill, buildExit the helper is another
// build. Anything else, or a helper that outlives its delay, is a failure.
class CountdownProcess final:public QObject {
public:
 static constexpr int cancelledExit=10,buildExit=5;
 // The arguments a helper's main() takes to enter countdown mode.
 static constexpr const char* argument="--countdown";
 explicit CountdownProcess(QString helper,QObject* parent=nullptr);
 ~CountdownProcess() override;
 bool busy()const{return process_!=nullptr;}
 // Answers once: {} at expiry, cancelled, or why the countdown could not run.
 void start(int seconds,const QJsonObject& appearance,std::function<void(Error)> reply);
 // Ends a running countdown with cancelled.
 void cancel();
private:
 void finish(Error);
 QString helper_;
 QProcess* process_=nullptr;
 QTimer deadline_;
 std::function<void(Error)> reply_;
 bool cancelling_=false;
};
}
