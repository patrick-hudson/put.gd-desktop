#pragma once
#include "AccountName.h"
#include "editor/QuickRedact.h"
#include <QDeadlineTimer>
#include <QObject>
#include <functional>

// #1618: runs quickRedact::find with the name and list sources off the UI
// thread and calls `done` on the UI thread. It never blocks the editor: a
// pattern that runs out of time is skipped (Result::skippedPatterns).
// `context` going away drops the callback.
//
// With `name` chosen and no `sources.login`, the account is read on the worker
// (src/ui/AccountName.h). Bounds: the patterns end within `budgetMs` plus one
// search (QuickRedact.cpp, patternLimits: 33 ms at worst measured); the account
// lookup adds at most two 250 ms bus calls, plus whatever the system's account
// lookup takes. `done` comes when the worker ends, or at `budgetMs` +
// quickRedactGraceMs with what it had finished by then (the rest of the
// patterns reported skipped).
//
// Exit has one owner: QApplication's destructor cancels every worker (each
// stops after its account lookup or before its next search) and waits at most
// quickRedactExitWaitMs, then lets any still running end on its own.
namespace putgd::ui {
inline constexpr int quickRedactGraceMs=1000;
inline constexpr int quickRedactExitWaitMs=1000;
void runQuickRedact(QObject* context,QList<editor::RecognizedWord> candidates,QStringList categories,editor::quickRedact::Sources sources,std::function<void(editor::quickRedact::Result)> done,int budgetMs=2000);
// Cancels every running worker and waits, on the interface thread, until all
// have ended or `limit` passes. Whether all ended. QApplication's destructor
// calls it with quickRedactExitWaitMs; tests call it too.
bool stopQuickRedactWorkers(QDeadlineTimer limit);
// The account lookup a worker uses (currentAccount); a test can slow it down.
// Empty restores the default. Takes effect for runs started after it.
using AccountLookup=std::function<Account()>;
void setQuickRedactAccountLookup(AccountLookup);
}
