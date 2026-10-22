#pragma once
// Bounded waits for tests. Every wait for an asynchronous outcome goes through
// these helpers so its budget scales with PUTGD_TEST_TIME_SCALE: 1 locally, more
// on loaded CI runners, where a step that takes 0.2 s alone can take over 5 s.
// A budget only bounds how long a test waits for something that will happen;
// a passing test returns as soon as the condition holds, so scaling never slows
// it. Product timing contracts are not budgets and are never scaled.
//
// tools/ci/check_test_waits.py rejects bare QTest::qWait, qWaitFor, QTRY_*,
// QSignalSpy::wait and qWaitForWindowExposed/Active in tests. Use:
//   PUTGD_TRY_VERIFY(expr) / PUTGD_TRY_COMPARE(actual, expected)
//       wait until it holds (budget 5 s x scale), then assert it;
//   PUTGD_TRY_VERIFY_WITH_TIMEOUT / PUTGD_TRY_COMPARE_WITH_TIMEOUT
//       the same with an explicit base budget in ms;
//   PUTGD_VERIFY_STEADY(expr, ms) / PUTGD_COMPARE_STEADY(actual, expected, ms)
//       wait until it holds, keep running the event loop for ms, and assert it
//       still holds: for "this arrives once and nothing more follows";
//   putgd::test::waitFor(predicate[, ms]), waitForSignal(spy[, count, ms]),
//   waitForWindowExposed(window[, ms]) and waitForWindowActive(window[, ms])
//       the same budgets for helpers, callbacks and windows;
//   putgd::test::settle(ms)
//       run the event loop for a fixed, unscaled time: to let a product timer
//       elapse, or before a check that nothing happened. Never use it as a
//       wait for completion; the lint rejects settle() followed by a plain
//       QVERIFY/QCOMPARE.
#include <QtGlobal>
#include <QByteArray>
#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <cmath>

namespace putgd::test {

inline constexpr int defaultWaitMs = 5000;

// PUTGD_TEST_TIME_SCALE, clamped to [1, 20]; unset or invalid means 1.
inline double timeScale() {
 static const double scale = [] {
  bool ok = false;
  const double value = qEnvironmentVariable("PUTGD_TEST_TIME_SCALE").toDouble(&ok);
  return ok && std::isfinite(value) ? std::clamp(value, 1.0, 20.0) : 1.0;
 }();
 return scale;
}

inline int scaled(int ms) { return int(std::lround(ms * timeScale())); }

// Polls like QTRY_*: the predicate is checked every 50 ms (less for short
// budgets) while QTest::qWait runs the event loop in between, so a chain of
// queued events completes within one step. The wait ends when both the
// scaled budget has elapsed and the steps add up to it, so it is never
// stricter than the QTRY_* loop it replaces, only longer when scaled.
template <class Predicate>
[[nodiscard]] bool waitFor(Predicate&& predicate, int ms = defaultWaitMs) {
 if (predicate()) return true;
 const int budget = scaled(ms);
 const int step = budget < 350 ? budget / 7 + 1 : 50;
 QDeadlineTimer deadline(budget, Qt::PreciseTimer);
 QTest::qWait(0);
 for (int waited = 0; !predicate(); waited += step) {
  if (waited >= budget && deadline.hasExpired()) return predicate();
  QTest::qWait(step);
 }
 return true;
}

[[nodiscard]] inline bool waitForSignal(QSignalSpy& spy, int count = 1, int ms = defaultWaitMs) {
 return waitFor([&] { return spy.size() >= count; }, ms);
}

#ifdef QT_GUI_LIB
// QTest's window waits with a scaled budget (Qt GUI tests only).
template <class Window>
[[nodiscard]] bool waitForWindowExposed(Window* window, int ms = defaultWaitMs) {
 return QTest::qWaitForWindowExposed(window, scaled(ms));
}
template <class Window>
[[nodiscard]] bool waitForWindowActive(Window* window, int ms = defaultWaitMs) {
 return QTest::qWaitForWindowActive(window, scaled(ms));
}
#endif

// Fixed, unscaled event-loop time. Deliberately not a synchronization.
inline void settle(int ms) { QTest::qWait(ms); }

// Reported before the final assertion of a timed-out wait, so a CI log shows
// the budget and scale beside Qt's own failure message.
inline void reportTimeout(const char* what, int baseMs, qint64 elapsedMs, const char* file, int line) {
 qWarning("putgd wait: %s still false after %lld ms (budget %d ms x scale %.2f) at %s:%d", what, elapsedMs, baseMs, timeScale(), file, line);
}

} // namespace putgd::test

#define PUTGD_TRY_VERIFY_WITH_TIMEOUT(expr, ms) \
 do { \
  QElapsedTimer putgd_wait_clock; putgd_wait_clock.start(); \
  if (!::putgd::test::waitFor([&]() -> bool { return static_cast<bool>(expr); }, (ms))) \
   ::putgd::test::reportTimeout(#expr, (ms), putgd_wait_clock.elapsed(), __FILE__, __LINE__); \
  QVERIFY(expr); \
 } while (false)

#define PUTGD_TRY_COMPARE_WITH_TIMEOUT(actual, expected, ms) \
 do { \
  QElapsedTimer putgd_wait_clock; putgd_wait_clock.start(); \
  if (!::putgd::test::waitFor([&]() -> bool { return (actual) == (expected); }, (ms))) \
   ::putgd::test::reportTimeout(#actual " == " #expected, (ms), putgd_wait_clock.elapsed(), __FILE__, __LINE__); \
  QCOMPARE(actual, expected); \
 } while (false)

#define PUTGD_TRY_VERIFY2_WITH_TIMEOUT(expr, message, ms) \
 do { \
  QElapsedTimer putgd_wait_clock; putgd_wait_clock.start(); \
  if (!::putgd::test::waitFor([&]() -> bool { return static_cast<bool>(expr); }, (ms))) \
   ::putgd::test::reportTimeout(#expr, (ms), putgd_wait_clock.elapsed(), __FILE__, __LINE__); \
  QVERIFY2(expr, message); \
 } while (false)

#define PUTGD_TRY_VERIFY(expr) PUTGD_TRY_VERIFY_WITH_TIMEOUT(expr, ::putgd::test::defaultWaitMs)
#define PUTGD_TRY_VERIFY2(expr, message) PUTGD_TRY_VERIFY2_WITH_TIMEOUT(expr, message, ::putgd::test::defaultWaitMs)
#define PUTGD_TRY_COMPARE(actual, expected) PUTGD_TRY_COMPARE_WITH_TIMEOUT(actual, expected, ::putgd::test::defaultWaitMs)

#define PUTGD_VERIFY_STEADY(expr, ms) \
 do { \
  PUTGD_TRY_VERIFY(expr); \
  ::putgd::test::settle(ms); \
  QVERIFY(expr); \
 } while (false)

#define PUTGD_COMPARE_STEADY(actual, expected, ms) \
 do { \
  PUTGD_TRY_COMPARE(actual, expected); \
  ::putgd::test::settle(ms); \
  QCOMPARE(actual, expected); \
 } while (false)
