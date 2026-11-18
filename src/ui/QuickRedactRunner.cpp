#include "QuickRedactRunner.h"
#include <QPointer>
#include <QThread>
#include <memory>

namespace putgd::ui {
void runQuickRedact(QObject* context,QList<editor::RecognizedWord> candidates,QStringList categories,editor::quickRedact::Sources sources,std::function<void(editor::quickRedact::Result)> done) {
 // #1618 stub: the matcher worker owns this file (budget, abandoned threads).
 auto result=std::make_shared<editor::quickRedact::Result>();
 auto* thread=QThread::create([candidates=std::move(candidates),categories=std::move(categories),sources=std::move(sources),result]{*result=editor::quickRedact::find(candidates,categories,sources);});
 QPointer<QObject> guard(context);
 QObject::connect(thread,&QThread::finished,context,[guard,result,done=std::move(done)]{if(guard&&done)done(*result);});
 QObject::connect(thread,&QThread::finished,thread,&QObject::deleteLater);
 thread->start();
}
}
