#pragma once
#include "editor/QuickRedact.h"
#include <QObject>
#include <functional>

// #1618: runs quickRedact::find with the name and list sources off the UI
// thread and calls `done` on the UI thread. It never blocks the editor: a
// runaway pattern is skipped at the budget (Result::skippedPatterns).
// `context` going away drops the callback.
namespace putgd::ui {
void runQuickRedact(QObject* context,QList<editor::RecognizedWord> candidates,QStringList categories,editor::quickRedact::Sources sources,std::function<void(editor::quickRedact::Result)> done);
}
