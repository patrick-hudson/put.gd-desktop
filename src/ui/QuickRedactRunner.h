#pragma once
#include "editor/QuickRedact.h"
#include <QDeadlineTimer>
#include <QObject>
#include <functional>

// #1618: runs quickRedact::find with the name and list sources off the UI
// thread and calls `done` on the UI thread. It never blocks the editor: a
// runaway pattern is skipped at the budget (Result::skippedPatterns).
// `context` going away drops the callback.
//
// With `name` chosen and no `sources.login`, the account is read on the worker
// (src/ui/AccountName.h). Bounds: the worker's patterns end within `budgetMs`
// plus one bounded match call (QuickRedact.cpp, patternLimits); the account
// lookup adds at most two 250 ms bus calls. `done` comes when the worker ends,
// or at `budgetMs` + quickRedactGraceMs with what it had finished by then (the
// rest of the patterns reported skipped). At exit the application waits for a
// running worker at most until that same deadline.
namespace putgd::ui {
inline constexpr int quickRedactGraceMs=1000;
void runQuickRedact(QObject* context,QList<editor::RecognizedWord> candidates,QStringList categories,editor::quickRedact::Sources sources,std::function<void(editor::quickRedact::Result)> done,int budgetMs=2000);
// Waits, on the interface thread, until every running worker has ended, each at
// most until its own deadline and all at most until `limit`. Whether all ended.
// QApplication's destructor calls it; tests call it too.
bool quickRedactWorkersEnded(QDeadlineTimer limit);
}
