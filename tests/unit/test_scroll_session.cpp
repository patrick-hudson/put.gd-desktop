// The scroll session (#1515) against the synthetic application: a person's
// scrolling as a script (slow, fast, stopping, back up), put.gd's auto-scroll
// to the end of the page, a switch between the two, the mouse that pauses
// auto-scroll, frames that cannot be placed, the cap, Done and Cancel in every
// state, and the idle bound. Only what a wrong image, an injection while the
// user drives, a capture that never ends, or Done that loses the image would
// hide. The image lives on the spool canvas the helper uses. Plain checks with
// a summary; exit code 1 on any failure.
#include <algorithm>
#include <atomic>
#include <climits>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>

#include "fixtures/ScrollSynthetic.h"
#include "record/ScrollSession.h"
#include "record/ScrollSpool.h"

using namespace putgd::record;
using namespace putgd::scroll;
using namespace synth;

namespace {

int g_fail = 0, g_pass = 0;

std::string fmt(const char* f, ...) __attribute__((format(printf, 1, 2)));
std::string fmt(const char* f, ...) {
  char buf[2048];
  va_list ap;
  va_start(ap, f);
  std::vsnprintf(buf, sizeof buf, f, ap);
  va_end(ap);
  return buf;
}

void report(const std::string& name, bool pass, const std::string& detail) {
  (pass ? g_pass : g_fail) += 1;
  std::printf("  %-64s %s  %s\n", name.c_str(), pass ? "PASS" : "FAIL", detail.c_str());
  std::fflush(stdout);
}

constexpr int W = 480, H = 400;

// Paragraphs, a photo, a gradient and a blank stretch shorter than the
// viewport. (One taller than the viewport, with no scrollbar in the frames,
// shows nothing that moves: it looks like the end of the page, a known limit.)
Document page(uint64_t seed, double blank = 0.3) {
  DocSpec ds;
  ds.seed = seed;
  ds.width = W;
  ds.viewport = H;
  ds.flatFactor = 0.2;
  ds.tableFactor = 0;
  ds.noiseFactor = 0.3;
  ds.gradientFactor = 0.3;
  ds.blankFactor = blank;
  ds.paragraphsPerGap = 4;
  return makeDocument(ds);
}

// The helper's canvas: the session's spool file, with room at both ends.
struct Spool {
  char path[64] = "/tmp/putgd-scroll-session-XXXXXX";
  int fd = -1;
  SpoolCanvas canvas;
  explicit Spool(long long rows) {
    fd = mkstemp(path);
    canvas.open(fd, W, rows, rows);
  }
  ~Spool() {
    canvas.close();
    if (fd >= 0) ::close(fd);
    ::unlink(path);
  }
};

bool blankRow(const uint32_t* row) {
  for (int x = 0; x < W; ++x) if (row[x] != row[0]) return false;
  return true;
}

// Where in the document the image starts: the document row of canvas row
// top(), found by its first row that shows something. INT_MIN: nowhere.
int startOf(const SpoolCanvas& c, const Image& doc) {
  std::vector<uint32_t> row(W);
  for (long long r = c.top(); r < c.bottom(); ++r) {
    c.readRow(r, reinterpret_cast<uint8_t*>(row.data()));
    if (blankRow(row.data())) continue;
    int found = INT_MIN, count = 0;
    for (int d = 0; d < doc.h; ++d)
      if (std::memcmp(row.data(), doc.row(d), size_t(W) * 4) == 0) { found = d - int(r); ++count; }
    return count == 1 ? found : INT_MIN;
  }
  return INT_MIN;
}

// The first canvas row that is not the document's at offset start (page row r
// is document row start + r), or LLONG_MIN when every row is.
long long firstWrong(const SpoolCanvas& c, const Image& doc, int start) {
  std::vector<uint8_t> row(size_t(W) * 4);
  for (long long r = c.top(); r < c.bottom(); ++r) {
    const long long d = start + r;
    if (d < 0 || d >= doc.h) return r;
    c.readRow(r, row.data());
    if (std::memcmp(row.data(), doc.row(int(d)), row.size()) != 0) return r;
  }
  return LLONG_MIN;
}

// One session against the fake application, with a script of what the user
// and the helper's main thread do at which time of the fake clock.
struct Harness {
  FakeClock clock;
  std::unique_ptr<FakeApp> app;
  std::unique_ptr<Spool> spool;
  std::atomic<bool> stop{false}, discard{false};
  bool wantAuto = false, paused = false;
  unsigned serial = 0, activity = 0;
  std::vector<std::pair<int64_t, SessionProgress>> seen;
  std::function<void(Harness&, int64_t)> script;
  SessionEnd end = SessionEnd::NoFrames;
  SessionStats stats;
  int64_t t0 = 0;
  void setAuto(bool on) { wantAuto = on; ++serial; }
  // The person's scrolling, in the script's time.
  void play(std::vector<FakeApp::UserScroll> moves) {
    for (auto& m : moves) m.at += t0;
    app->script.insert(app->script.end(), moves.begin(), moves.end());
  }
  SessionEnd run(const Image& doc, FakeAppSpec as, SessionOptions so) {
    t0 = clock.nowMs();
    app = std::make_unique<FakeApp>(doc, ViewportSpec{W, H, 0, 0, 7}, as, clock);
    spool = std::make_unique<Spool>(so.tracker.maxOutputRows + 2LL * H);
    wantAuto = so.autoScroll;
    so.request = [this] {
      if (script) script(*this, clock.nowMs() - t0);
      return std::pair<unsigned, bool>(serial, wantAuto);
    };
    so.activity = [this] { return activity; };
    // The main thread closes the gate the moment it sees the mouse: asked between any two injections.
    so.paused = [this] { if (script) script(*this, clock.nowMs() - t0); return paused; };
    so.onProgress = [this](const SessionProgress& p) { seen.push_back({clock.nowMs() - t0, p}); };
    ScrollSession session(*app, *app, clock, spool->canvas, so);
    end = session.run(stop, discard);
    stats = session.stats();
    return end;
  }
  bool saw(SessionState s, HandBack by = HandBack::None) const {
    for (const auto& [t, p] : seen) if (p.state == s && (by == HandBack::None || p.pausedBy == by)) return true;
    return false;
  }
  SessionState last() const { return seen.empty() ? SessionState::Waiting : seen.back().second.state; }
  // Injected events (pointer and wheel) in [from, to) ms of the script's time.
  int injectedBetween(int64_t from, int64_t to) const {
    int n = 0;
    for (const auto& e : app->events) n += e.t - t0 >= from && e.t - t0 < to;
    return n;
  }
  // The image holds the document's rows, exactly, from `start` (INT_MIN: wherever it begins).
  std::string exact(const Image& doc, int start = INT_MIN) const {
    const auto& c = spool->canvas;
    if (c.bottom() <= c.top()) return "empty image";
    if (start == INT_MIN) start = startOf(c, doc);
    if (start == INT_MIN) return "the image matches no place in the document";
    const long long wrong = firstWrong(c, doc, start);
    if (wrong != LLONG_MIN) return fmt("row %lld (document row %lld) is wrong", wrong, start + wrong);
    return {};
  }
};

SessionOptions options(bool autoScroll) {
  SessionOptions so;
  so.tracker.maxOutputRows = 30000;
  so.autoScroll = autoScroll;
  so.pointer = true;
  so.away = RunPoint{-16, H / 3};  // the rest on a surface of the helper's, left of the rectangle
  so.entryInset = 48;
  return so;
}

std::string counts(const Harness& h) {
  return fmt("%s, %lld rows, placed %d skipped %d recovered %d, by user %d by put.gd %d, %d steps, %.0f px/notch", toString(h.end), h.stats.bottom - h.stats.top,
             h.stats.placed, h.stats.skipped, h.stats.recovered, h.stats.movedByUser, h.stats.movedByPutgd, h.stats.steps, h.stats.pxPerNotch);
}

// ------------------------------------------------------------------ the user

void testUserScrolls() {
  std::printf("The user scrolls\n");
  const Document doc = page(11);
  Harness h;
  // Slowly, 40 px every 60 ms; then three flicks; then still for 2 s; then back
  // up; then down to the end of the page in 120 px steps.
  std::vector<FakeApp::UserScroll> script;
  int at = 1500;
  for (int i = 0; i < 20; ++i, at += 60) script.push_back({at, 40, 60});
  for (int i = 0; i < 3; ++i, at += 200) script.push_back({at, 300, 150});
  at += 2000;
  script.push_back({at, -500, 300});
  at += 800;
  for (int i = 0; i < (doc.img.h - H) / 120 + 4; ++i, at += 100) script.push_back({at, 120, 90});
  const int stopAt = at + 1500;
  h.script = [stopAt, script](Harness& x, int64_t t) {
    if (x.app->script.empty()) x.play(script);
    if (t >= stopAt) x.stop.store(true);
  };
  FakeAppSpec as;
  SessionOptions so = options(false);
  h.run(doc.img, as, so);
  report("nothing is injected while the user drives", h.app->events.empty(), fmt("%zu events", h.app->events.size()));
  const std::string wrong = h.exact(doc.img, 0);
  report("the image is the page, exactly, from the first frame to the end", wrong.empty() && h.stats.top <= 0 && h.stats.bottom >= doc.img.h,
         wrong.empty() ? counts(h) : wrong);
  report("every frame went to the tracker as it arrived", h.stats.framesSeen >= int((stopAt - 100) / 50), fmt("%d frames in %lld ms", h.stats.framesSeen, (long long)stopAt));
  report("Done ends it and says who scrolled", h.end == SessionEnd::Finished && h.stats.movedByUser > 0 && h.stats.movedByPutgd == 0 && !h.stats.endedLost, counts(h));
}

// --------------------------------------------------------------- auto-scroll

void testAutoScroll() {
  std::printf("Auto-scroll\n");
  const Document doc = page(23);
  for (const bool fromTop : {false, true}) {
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    as.startPos = fromTop ? 1800 : 0;
    SessionOptions so = options(true);
    so.fromTop = fromTop;
    h.run(doc.img, as, so);
    const std::string wrong = h.exact(doc.img, 0);
    const char* name = fromTop ? "from the middle, to the top first, then to the end" : "from the top to the end";
    report(fmt("auto-scroll %s: the whole page, exactly", name), wrong.empty() && h.stats.top == 0 && h.stats.bottom == doc.img.h, wrong.empty() ? counts(h) : wrong);
    report(fmt("auto-scroll %s: ends by itself, nothing skipped", name),
           h.end == SessionEnd::PageEnd && h.stats.reachedEnd && h.stats.skipped == 0 && (!fromTop || h.stats.topReached), counts(h) + (fromTop ? fmt(", top %s", h.stats.topReached ? "proved" : h.stats.toTopGaveUp.c_str()) : std::string()));
    // Every wheel step a visit: in, the step, back to the rest at once.
    int bad = 0, steps = 0, biggest = 0;
    const auto& e = h.app->events;
    for (size_t i = 0; i < e.size(); ++i) {
      if (e[i].kind != 's') continue;
      ++steps;
      // Down; the way to the top takes large steps up, which make no image.
      if (e[i].a > 0) biggest = std::max(biggest, e[i].a);
      const bool in = i > 0 && e[i - 1].kind == 'm' && e[i - 1].a >= 0 && e[i - 1].a < W && e[i - 1].b >= 0 && e[i - 1].b < H;
      const bool out = i + 1 < e.size() && e[i + 1].kind == 'm' && e[i + 1].a == -16 && e[i + 1].t - e[i].t <= 40;
      bad += !in || !out;
    }
    report(fmt("auto-scroll %s: each step a short visit, under half the frame", name), steps > 0 && bad == 0 && biggest * as.pxPerNotch <= H * 0.45,
           fmt("%d steps, %d without a visit, largest %d notches", steps, bad, biggest));
  }
}

// A switch to auto-scroll and back, and the mouse that pauses it.
void testSwitchAndPause() {
  std::printf("Switching, and the mouse\n");
  const Document doc = page(31);
  Harness h;
  FakeAppSpec as;
  as.pxPerNotch = 50;
  std::vector<FakeApp::UserScroll> script;
  for (int i = 0; i < 10; ++i) script.push_back({1500 + 100 * i, 100, 80});
  h.script = [script](Harness& x, int64_t t) {
    if (x.app->script.empty()) x.play(script);
    if (t >= 4000 && x.serial == 0) x.setAuto(true);
    // The user takes the mouse at 7 s, scrolls back up a little, gives it back at 9 s.
    if (t >= 7000 && t < 9000) x.paused = true;
    if (t >= 7500 && x.app->script.size() == 10) x.play({{t, -250, 200}});
    if (t >= 9000 && x.paused) { x.paused = false; x.setAuto(true); }
  };
  h.run(doc.img, as, options(false));
  if (h.end != SessionEnd::PageEnd)
    for (const auto& [t, p] : h.seen) std::printf("    t=%lld %s %s %s height %lld placed %d steps %d\n", (long long)t, toString(p.state), toString(p.driver), toString(p.pausedBy), p.height, p.placed, p.steps);
  const std::string wrong = h.exact(doc.img, 0);
  report("user, auto-scroll, a pause by the mouse, auto-scroll: one exact image", wrong.empty() && h.stats.top == 0 && h.stats.bottom == doc.img.h, wrong.empty() ? counts(h) : wrong);
  report("it ends at the end of the page, scrolled by both", h.end == SessionEnd::PageEnd && h.stats.movedByUser > 0 && h.stats.movedByPutgd > 0, counts(h));
  report("the mouse pauses auto-scroll and says so", h.saw(SessionState::Paused, HandBack::Pointer), "");
  report("nothing injected while the user drives or has the mouse", h.injectedBetween(0, 4000) == 0 && h.injectedBetween(7001, 9000) == 0,
         fmt("%d before auto-scroll, %d while paused", h.injectedBetween(0, 4000), h.injectedBetween(7001, 9000)));
}

// --------------------------------------------------------- lost, and found again

void testLostAndFound() {
  std::printf("Frames that cannot be placed\n");
  const Document doc = page(41);
  for (const bool back : {true, false}) {
    Harness h;
    std::vector<FakeApp::UserScroll> script{{1500, 200, 150}, {2500, 1500, 0}};
    if (back) script.push_back({4000, -1500, 0}), script.push_back({4800, 400, 200});
    const int stopAt = back ? 6500 : 3800;
    h.script = [script, stopAt](Harness& x, int64_t t) {
      if (x.app->script.empty()) x.play(script);
      if (t >= stopAt) x.stop.store(true);
    };
    h.run(doc.img, FakeAppSpec{}, options(false));
    const std::string wrong = h.exact(doc.img, 0);
    if (back) {
      bool lostThenFound = false, lost = false;
      for (const auto& [t, p] : h.seen) { lost = lost || p.state == SessionState::Lost; lostThenFound = lostThenFound || (lost && p.state == SessionState::Following); }
      report("a jump past the screen is lost, then found again on the way back", lostThenFound && h.stats.skipped > 0 && !h.stats.endedLost && h.stats.bottom == 200 + H + 400, counts(h));
      report("nothing of the frames it could not place is in the image", wrong.empty(), wrong.empty() ? fmt("%lld rows", h.stats.bottom - h.stats.top) : wrong);
    } else {
      report("Done while lost keeps the image so far and says it ended lost", h.end == SessionEnd::Finished && h.stats.endedLost && wrong.empty(), wrong.empty() ? counts(h) : wrong);
    }
  }
}

// ------------------------------------------------------------------- limits

void testCapAndIdle() {
  std::printf("Limits\n");
  const Document doc = page(51);
  {
    Harness h;
    SessionOptions so = options(true);
    so.tracker.maxOutputRows = 1200;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    h.run(doc.img, as, so);
    const std::string wrong = h.exact(doc.img, 0);
    report("the height cap ends it with the image so far", h.end == SessionEnd::Capped && h.stats.bottom - h.stats.top <= 1200 && wrong.empty(), wrong.empty() ? counts(h) : wrong);
  }
  {
    Harness h;
    SessionOptions so = options(false);
    so.idleLimitMs = 5000;
    std::vector<FakeApp::UserScroll> script{{1500, 300, 150}};
    h.script = [script](Harness& x, int64_t t) {
      if (x.app->script.empty()) x.play(script);
      if (t >= 30000) x.stop.store(true);  // the bound must come first
    };
    h.run(doc.img, FakeAppSpec{}, so);
    const std::string wrong = h.exact(doc.img, 0);
    report("a capture nobody touches ends by itself, the image kept", h.end == SessionEnd::Idle && h.stats.elapsedMs < 8000 && wrong.empty(),
           wrong.empty() ? fmt("%s after %lld ms", toString(h.end), (long long)h.stats.elapsedMs) : wrong);
  }
}

// ----------------------------------------------------------- Done and Cancel

void testDoneAndCancel() {
  std::printf("Done and Cancel in every state\n");
  const Document doc = page(61);
  struct Case {
    const char* name;
    SessionState state;
    bool autoScroll, fromTop;
    std::vector<FakeApp::UserScroll> script;
    int pauseAt;
  };
  const std::vector<Case> cases{
      {"waiting", SessionState::Waiting, false, false, {}, -1},
      {"following", SessionState::Following, false, false, {{1500, 300, 150}}, -1},
      {"lost", SessionState::Lost, false, false, {{1500, 300, 150}, {2300, 1500, 0}}, -1},
      {"auto-scrolling", SessionState::AutoScrolling, true, false, {}, -1},
      {"paused by the mouse", SessionState::Paused, true, false, {}, 2500},
      {"on the way to the top", SessionState::ScrollingToTop, true, true, {}, -1},
  };
  for (const Case& c : cases) {
    for (const bool cancel : {false, true}) {
      Harness h;
      FakeAppSpec as;
      as.pxPerNotch = 40;
      if (c.fromTop) as.startPos = 2400;
      SessionOptions so = options(c.autoScroll);
      so.fromTop = c.fromTop;
      int64_t inState = -1;
      h.script = [&c, &inState, cancel](Harness& x, int64_t t) {
        if (x.app->script.empty() && !c.script.empty()) x.play(c.script);
        if (c.pauseAt >= 0 && t >= c.pauseAt) x.paused = true;
        if (inState < 0 && x.last() == c.state && t > 1200) inState = t;
        if (inState >= 0 && t >= inState + 300) (cancel ? x.discard : x.stop).store(true);
        if (t >= 20000) x.stop.store(true);
      };
      h.run(doc.img, as, so);
      const std::string wrong = cancel ? std::string() : h.exact(doc.img);
      const bool ok = inState >= 0 && (cancel ? h.end == SessionEnd::Discarded : h.end == SessionEnd::Finished && h.stats.placed > 0 && wrong.empty());
      report(fmt("%s while %s", cancel ? "Cancel" : "Done", c.name), ok, inState < 0 ? "the state was never reached" : wrong.empty() ? counts(h) : wrong);
    }
  }
}

}  // namespace

int main() {
  testUserScrolls();
  testAutoScroll();
  testSwitchAndPause();
  testLostAndFound();
  testCapAndIdle();
  testDoneAndCancel();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
