#pragma once
#include "AccountName.h"
#include "editor/QuickRedact.h"
#include <QDeadlineTimer>
#include <QObject>
#include <functional>

// #1618: runs quickRedact::find with the name and list sources off the UI
// thread and calls `done` on the UI thread. It never blocks the editor.
// `context` going away drops the callback.
//
// With `name` chosen and no `sources.login`, the account is read off the
// interface thread (src/ui/AccountName.h), one lookup at a time for the whole
// process. The other categories wait for it at most quickRedactAccountWaitMs;
// without an answer they run on and Result::nameUnchecked is set.
//
// User patterns run in a child process, `putgd-ui --match-patterns`, which the
// worker kills at the deadline (`budgetMs` from the call): nothing bounds a
// regular expression's time in process, so the kill is the bound. The child
// reports each pattern as it ends; a pattern it did not finish is named in
// Result::skippedPatterns, and every other category and every finished pattern
// still lands. The child is started only when there are patterns to run. It
// gets the subject texts and the patterns over a pipe and returns match ranges
// (pattern, subject, start, length); the words and claims stay here. It has no
// new privileges, a cleared environment, no file it may write or (with
// Landlock) open, and it dies with its parent thread. Measured costs are in the
// PR (#1618 round 3): starting the child, and killing it at the deadline.
//
// `done` comes when the worker ends, or at `budgetMs` + quickRedactGraceMs with
// what it had finished by then.
//
// Exit has one owner: QApplication's destructor cancels every worker (each
// stops while it waits for the account, and within 50 ms while the child runs,
// killing it) and waits at most quickRedactExitWaitMs, then lets any still
// running end on its own. An account lookup still running then never reaches
// the system bus.
namespace putgd::ui {
inline constexpr int quickRedactGraceMs=1000;
inline constexpr int quickRedactExitWaitMs=1000;
// The longest the other categories wait for the account lookup (#1618): it
// then goes on in the background, and Result::nameUnchecked says the name was
// not checked. One lookup at a time for the whole process.
inline constexpr int quickRedactAccountWaitMs=1000;
void runQuickRedact(QObject* context,QList<editor::RecognizedWord> candidates,QStringList categories,editor::quickRedact::Sources sources,std::function<void(editor::quickRedact::Result)> done,int budgetMs=2000);
// Cancels every running worker and waits, on the interface thread, until all
// have ended or `limit` passes. Whether all ended. QApplication's destructor
// calls it with quickRedactExitWaitMs; tests call it too.
bool stopQuickRedactWorkers(QDeadlineTimer limit);
// The account lookup a worker uses (currentAccount with its stop check); a test
// can slow it down. Empty restores the default. Takes effect for runs started
// after it.
using AccountLookup=std::function<Account(const std::function<bool()>& stopped)>;
void setQuickRedactAccountLookup(AccountLookup);
// The matcher program; by default putgd-ui itself, or putgd-ui beside the
// running program. Tests name the built one.
void setQuickRedactMatcherProgram(QString path);
// How many matcher children this process has started.
int quickRedactMatcherStarts();
// Whether an account lookup is still running (tests wait for it to end).
bool quickRedactAccountLookupRunning();
// The child's side, `putgd-ui --match-patterns`: reads the request on stdin,
// writes one line a pattern on stdout, and exits.
int runQuickRedactMatcher();
}
