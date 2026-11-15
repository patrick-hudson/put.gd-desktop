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
#include <cerrno>
#include <climits>
#include <cstdint>
#include <optional>
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
  // A header and a footer pinned in the viewport (rows of it).
  int headerH = 0, footerH = 0;
  // The spool's disk: blocks beyond this many bytes are refused, as on a full disk.
  size_t spoolSpace = SIZE_MAX;
  // Auto-scroll asked for after the session was set up but before its loop read the request.
  bool autoBeforeRun = false;
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
    app = std::make_unique<FakeApp>(doc, ViewportSpec{W, H, headerH, footerH, 7}, as, clock);
    spool = std::make_unique<Spool>(so.tracker.maxOutputRows + 2LL * H);
    spool->canvas.setSpaceLimit(spoolSpace);
    wantAuto = so.autoScroll;
    if (autoBeforeRun) setAuto(true);
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

// A side bar that takes the wheel at the left edge once the page has moved:
// still steps there are not the end of the page.
void testWheelTakenByAPart() {
  std::printf("A part that takes the wheel\n");
  const Document doc = page(71);
  Harness h;
  FakeAppSpec as;
  as.pxPerNotch = 40;
  as.deadAfterInjection = 1;
  as.deadX = 0, as.deadY = 0, as.deadW = 120, as.deadH = H;
  h.run(doc.img, as, options(true));
  const std::string wrong = h.exact(doc.img, 0);
  report("a part that scrolls by itself under the wheel is no end of the page", h.end == SessionEnd::PageEnd && h.stats.bottom == doc.img.h && wrong.empty(),
         wrong.empty() ? counts(h) : wrong);
}

// #1519: put.gd's bar drawn inside the region's bottom-right corner, its
// counter changing every frame. None of its pixels is in the image, and the
// page under it comes from frames where it was clear.
void testBarInside() {
  std::printf("The bar inside the region\n");
  const Document doc = page(81);
  const TrackerRect bar{W - 260, H - 60, 250, 50};
  for (const bool autoScroll : {true, false}) {
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    as.barX = bar.x, as.barY = bar.y, as.barW = bar.w, as.barH = bar.h;
    SessionOptions so = options(autoScroll);
    so.excluded = bar;
    std::vector<FakeApp::UserScroll> script;
    for (int i = 0; i < (doc.img.h - H) / 120 + 4; ++i) script.push_back({1500 + 100 * i, 120, 90});
    const int stopAt = 1500 + 100 * int(script.size()) + 1500;
    h.script = [script, stopAt, autoScroll](Harness& x, int64_t t) {
      if (autoScroll) return;
      if (x.app->script.empty()) x.play(script);
      if (t >= stopAt) x.stop.store(true);
    };
    h.run(doc.img, as, so);
    // Every row is the page's, except, at the page's end, cells only the bar ever covered: those are plain.
    const auto& c = h.spool->canvas;
    std::vector<uint32_t> row(W);
    long long wrong = 0, barPixels = 0, hidden = 0;
    for (long long r = c.top(); r < c.bottom(); ++r) {
      c.readRow(r, reinterpret_cast<uint8_t*>(row.data()));
      for (int x = 0; x < W; ++x) {
        if (((row[x] >> 16) & 0xff) == 250 && ((row[x] >> 8) & 0xff) < 64 && (row[x] & 0xff) < 64) ++barPixels;
        // The tracker rounds the rectangle out to whole cells and blocks of 8.
        const bool underBarAtEnd = r >= doc.img.h - (H - bar.y) - 8 && x >= bar.x - 8 && x < bar.x + bar.w + 8;
        if (row[x] != doc.img.at(x, int(r))) (underBarAtEnd ? hidden : wrong) += 1;
      }
    }
    report(fmt("%s with the bar inside: no bar pixel, every other row exact", autoScroll ? "auto-scroll" : "you scroll"),
           barPixels == 0 && wrong == 0 && h.stats.bottom == doc.img.h && (hidden == 0) == (h.stats.hiddenCells == 0),
           fmt("%s, bar pixels %lld, wrong %lld, under the bar at the end %lld (hiddenCells %lld)", counts(h).c_str(), barPixels, wrong, hidden, h.stats.hiddenCells));
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

// A page that jumps under auto-scroll (GitHub in Chrome lays itself out anew
// near 20,000 px). Into the page's end: the steps back find it, and the image
// is exact to the end. Far past the image, on a page of text and on one with
// plain stretches (where a frame with nothing in common with the last was
// once placed on the hint, #1534): auto-scroll hands the page back and says
// lost; the user scrolls back, it says found; Auto-scroll again carries on to
// the end, and the image is exact.
void testAutoJump() {
  std::printf("A page that jumps under auto-scroll\n");
  DocSpec ds;
  ds.seed = 43;
  ds.width = W;
  ds.viewport = H;
  ds.flatFactor = 0;
  ds.tableFactor = 0;
  ds.noiseFactor = 0.5;
  ds.gradientFactor = 0;
  ds.blankFactor = 0;
  ds.paragraphsPerGap = 12;
  const Document text = makeDocument(ds);
  ds.flatFactor = 0.2;
  ds.gradientFactor = 0.3;
  ds.blankFactor = 0.3;
  ds.noiseFactor = 0.3;
  const Document plain = makeDocument(ds);
  struct Case { const char* name; const Document* doc; bool intoEnd; };
  for (const Case& c : {Case{"a jump into the page's end", &text, true}, Case{"a jump far past the image, a page of text", &text, false},
                        Case{"a jump far past the image, plain stretches", &plain, false}}) {
    const Image& doc = c.doc->img;
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    const int jumpAt = c.intoEnd ? doc.h - 2 * H - 300 : 1500, jump = c.intoEnd ? 1500 : 2000;
    int before = -1;
    int64_t jumpedAt = -1, handedAt = -1, foundAt = -1, againAt = -1;
    h.script = [&](Harness& x, int64_t t) {
      if (before < 0 && x.app->positionNow() >= jumpAt) { before = x.app->positionNow(); jumpedAt = t; x.play({{t, jump, 0}}); }
      if (handedAt < 0 && !x.seen.empty() && x.seen.back().second.driver == Driver::User && x.seen.back().second.pausedBy == HandBack::Lost) {
        handedAt = t;
        // A person, seeing it stop, scrolls back up to where it was.
        x.play({{t + 1000, before - x.app->positionNow(), 400}});
      }
      if (handedAt >= 0 && foundAt < 0 && x.last() == SessionState::Paused) foundAt = t;
      if (foundAt >= 0 && againAt < 0 && t >= foundAt + 1000) { againAt = t; x.setAuto(true); }
      if (t >= 240000) x.stop.store(true);
    };
    h.run(doc, as, options(true));
    const std::string wrong = h.exact(doc, 0);
    if (c.intoEnd) {
      report(fmt("%s: the steps back find it, one exact image to the end", c.name), h.end == SessionEnd::PageEnd && wrong.empty() && h.stats.bottom == doc.h,
             (wrong.empty() ? counts(h) : wrong) + fmt(", page %d rows, jump at %d", doc.h, before));
      continue;
    }
    report(fmt("%s: handed back, lost", c.name), handedAt >= 0 && h.saw(SessionState::Lost, HandBack::Lost),
           handedAt < 0 ? std::string("it never handed the page back") : fmt("%lld ms after the jump, %d steps back", (long long)(handedAt - jumpedAt), h.stats.stepsBack));
    report(fmt("%s: scrolled back, found, nothing injected until Auto-scroll", c.name),
           foundAt >= 0 && h.saw(SessionState::Paused, HandBack::Lost) && againAt >= 0 && h.injectedBetween(handedAt + 1, againAt) == 0,
           foundAt < 0 ? std::string("never found") : fmt("found %lld ms after the hand-back, %d injected", (long long)(foundAt - handedAt), againAt >= 0 ? h.injectedBetween(handedAt + 1, againAt) : -1));
    report(fmt("%s: Auto-scroll again, one exact image to the end", c.name), againAt >= 0 && h.end == SessionEnd::PageEnd && wrong.empty() && h.stats.bottom == doc.h,
           (wrong.empty() ? counts(h) : wrong) + fmt(", page %d rows", doc.h));
  }
}

// From CodeRabbit's review of #1528: an Auto-scroll asked for before the loop
// began is acted on; the way to the top on a stream that sends only changes
// starts from the screen as it is, and Done on it keeps that screen; a pause in
// the middle of a visit whose frames show the pointer leaves the frames after
// it to the page.
void testReviewCases() {
  std::printf("Review cases\n");
  const Document doc = page(91);
  {
    Harness h;
    h.autoBeforeRun = true;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    h.run(doc.img, as, options(false));
    report("Auto-scroll asked for before the loop began runs to the end", h.end == SessionEnd::PageEnd && h.stats.movedByPutgd > 0 && h.exact(doc.img, 0).empty(), counts(h));
  }
  {
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    as.startPos = 1800;
    as.damageOnly = true;
    SessionOptions so = options(true);
    so.fromTop = true;
    h.run(doc.img, as, so);
    const std::string wrong = h.exact(doc.img, 0);
    report("the way to the top on a stream that sends only changes", h.stats.topReached && h.end == SessionEnd::PageEnd && wrong.empty() && h.stats.top == 0 && h.stats.bottom == doc.img.h,
           (wrong.empty() ? counts(h) : wrong) + fmt(", top %s", h.stats.topReached ? "proved" : h.stats.toTopGaveUp.c_str()));
  }
  {
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    as.startPos = 1800;
    as.damageOnly = true;
    as.reactionDelayMs = 2000;  // nothing moves before Done
    SessionOptions so = options(true);
    so.fromTop = true;
    h.script = [](Harness& x, int64_t t) { if (t >= 300) x.stop.store(true); };
    h.run(doc.img, as, so);
    const std::string wrong = h.exact(doc.img, 1800);
    report("Done on the way to the top, before another frame came, keeps the screen", h.end == SessionEnd::Finished && wrong.empty() && h.stats.bottom - h.stats.top == H,
           wrong.empty() ? counts(h) : wrong);
  }
  {
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    SessionOptions so = options(true);
    so.blindVisits = true;
    int64_t pausedAt = -1;
    long long heightAtPause = 0;
    h.script = [&](Harness& x, int64_t t) {
      // The mouse moves while the pointer is in the page for a visit.
      if (pausedAt < 0 && t >= 2000 && !x.app->events.empty() && x.app->events.back().kind == 'm' && x.app->events.back().a >= 0 && x.app->events.back().a < W) {
        x.paused = true;
        pausedAt = t;
        heightAtPause = x.seen.empty() ? 0 : x.seen.back().second.height;
        std::vector<FakeApp::UserScroll> moves;
        for (int i = 0; i < 6; ++i) moves.push_back({t + 500 + 300 * i, 150, 120});
        x.play(moves);
      }
      if (pausedAt >= 0 && t >= pausedAt + 3500) x.stop.store(true);
    };
    h.run(doc.img, as, so);
    const std::string wrong = h.exact(doc.img, 0);
    report("a pause during a visit with the pointer in the frames: the image grows after it", pausedAt >= 0 && wrong.empty() && h.stats.bottom - h.stats.top > heightAtPause + 600 && h.stats.movedByUser > 0,
           pausedAt < 0 ? std::string("no pause during a visit") : (wrong.empty() ? counts(h) : wrong) + fmt(", %lld rows at the pause", heightAtPause));
  }
}

// #1534's live runs: KWin's stream sent the same picture 92 times while
// Chrome went on scrolling, and a person's Done missed the last rows. Done
// takes a fresh frame of the screen first: the image ends where the page is.
void testStaleStreamAtDone() {
  std::printf("A stream gone stale before Done\n");
  const Document doc = page(97);
  Harness h;
  FakeAppSpec as;
  as.staleFromMs = 2200;
  std::vector<FakeApp::UserScroll> script;
  for (int i = 0; i < 12; ++i) script.push_back({1500 + 150 * i, 30, 100});  // the last 300 px after the stream went stale
  h.script = [script](Harness& x, int64_t t) {
    if (x.app->script.empty()) x.play(script);
    if (t >= 4500) x.stop.store(true);
  };
  h.run(doc.img, as, options(false));
  const int end = h.app->positionNow() + H;
  const std::string wrong = h.exact(doc.img, 0);
  report("Done after the stream went stale keeps the rows the person scrolled to", h.end == SessionEnd::Finished && wrong.empty() && h.stats.bottom == end && h.stats.rowsAtDone > 0,
         (wrong.empty() ? counts(h) : wrong) + fmt(", image to %lld, page shows to %d, Done's frame added %lld", h.stats.bottom, end, h.stats.rowsAtDone));
}

// Auto-scroll on a stream that goes stale (KWin resent its last picture while
// the page went on moving): frames keep coming, all still, after every step.
// The end is taken only from a fresh frame of the screen, which shows the page
// moved on: no end_of_content for half a page (CodeRabbit on #1528).
void testStaleStreamUnderAuto() {
  std::printf("Auto-scroll on a stream gone stale\n");
  const Document doc = page(98);
  Harness h;
  FakeAppSpec as;
  as.pxPerNotch = 40;
  as.staleFromMs = 2200;
  h.run(doc.img, as, options(true));
  const std::string wrong = h.exact(doc.img, 0);
  report("auto-scroll on a stale stream: the end only from a fresh frame, the whole page",
         h.end == SessionEnd::PageEnd && h.stats.reachedEnd && wrong.empty() && h.stats.bottom == doc.img.h,
         (wrong.empty() ? counts(h) : wrong) + fmt(", image to %lld of %d, %d fresh frames", h.stats.bottom, doc.img.h, h.app->freshFrames));
}

// One frame out of place under auto-scroll (#1535): on a starved X server a
// grab caught the page half repainted, the tracker placed it far up against
// the hint, and the session took that one frame for the user scrolling up and
// handed the wheel back to nobody. A jump up is the user's only if the next
// frame agrees; one that falls back onto auto-scroll's way is not.
void testOneFrameOutOfPlace() {
  std::printf("One frame out of place under auto-scroll\n");
  const Document doc = page(94);
  Harness h;
  FakeAppSpec as;
  as.pxPerNotch = 40;
  as.glitchAtMs = 3000;
  as.glitchUp = 300;
  h.script = [](Harness& x, int64_t t) { if (t >= 90000) x.stop.store(true); };
  h.run(doc.img, as, options(true));
  const std::string wrong = h.exact(doc.img, 0);
  report("a single frame shown 300 px up is not the user scrolling: auto-scroll goes on to the end, the image exact",
         h.end == SessionEnd::PageEnd && h.stats.reachedEnd && wrong.empty() && h.stats.bottom == doc.img.h && !h.saw(SessionState::Paused, HandBack::Scrolled),
         (wrong.empty() ? counts(h) : wrong) + fmt(", %d hand-backs", h.stats.handBacks));
  // The user's own scrolling up still hands the wheel back: the page stays up.
  Harness u;
  FakeAppSpec us;
  us.pxPerNotch = 40;
  u.script = [](Harness& x, int64_t t) {
    if (t >= 3000 && x.app->script.empty() && !x.saw(SessionState::Paused, HandBack::Scrolled)) x.play({{t, -300, 250}});
    if (t >= 9000) x.stop.store(true);
  };
  u.run(doc.img, us, options(true));
  report("the user scrolling up during auto-scroll still takes the wheel back", u.saw(SessionState::Paused, HandBack::Scrolled) && u.exact(doc.img, 0).empty(),
         counts(u) + fmt(", %d hand-backs", u.stats.handBacks));
}

// "Scroll to the top first" with a strip under the first wheel point that
// takes no wheel (#1536's review): two still steps there say nothing of the
// top. The top is proved only where the wheel moves the page, or once every
// wheel point has moved nothing. The image starts at the page's top, or the
// result says topReached false; it never starts in the middle with the top
// claimed.
void testTopUnderADeadPoint() {
  std::printf("The way to the top over a strip that takes no wheel\n");
  const Document doc = page(93);
  Harness h;
  FakeAppSpec as;
  as.pxPerNotch = 40;
  as.startPos = 1800;
  // Under the first wheel point (the entry point, 48 px in from the left) from the first injection on.
  as.deadAfterInjection = 0;
  as.deadX = 0, as.deadY = 0, as.deadW = 120, as.deadH = H;
  SessionOptions so = options(true);
  so.fromTop = true;
  h.run(doc.img, as, so);
  const std::string wrong = h.exact(doc.img, 0);
  const bool fromTheTop = h.stats.topReached && wrong.empty() && h.stats.top == 0 && h.stats.bottom == doc.img.h;
  report("to the top first over a strip that takes no wheel: the image from the page's top, or topReached false",
         h.end == SessionEnd::PageEnd && (fromTheTop || !h.stats.topReached),
         (wrong.empty() ? counts(h) : wrong) + fmt(", top %s, %d steps up", h.stats.topReached ? "proved" : h.stats.toTopGaveUp.c_str(), h.stats.toTopSteps));
}

// Hints counted twice (#1543's review): a frame the tracker calls still
// without proving it (nothing matched, most of it the same) keeps the sum of
// hints since its last placed frame; the session took its share back as
// owed and handed it out again, so the tracker's expected offset grew past
// what the step owed (a sum past the frame height was seen). Steps whose
// first frames show the page still (an application that answers late), with
// part of the frame changing: the tracker's working sum, rebuilt from the
// hints and the results, never exceeds what the step owes.
void testHintsCountedOnce() {
  std::printf("Hints after frames that show the page still\n");
  const Document doc = page(95);
  for (const int restless : {0, 24}) {
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    as.reactionDelayMs = 450;
    as.restlessRows = restless;
    SessionOptions so = options(true);
    int notches = 0, kept = 0;
    double worst = -1e9;
    so.onStep = [&](int n, RunPoint) { notches = n; };
    so.onFrame = [&](const TrackResult& r, const TrackerHint&, int64_t) {
      if (r.hinted && notches != 0) worst = std::max(worst, std::abs(r.expected) - std::abs(notches) * as.pxPerNotch * 1.05);
      kept += r.status == TrackStatus::NoMovement && r.hintsKept;
    };
    h.script = [](Harness& x, int64_t t) { if (t >= 120000) x.stop.store(true); };
    h.run(doc.img, as, so);
    // Rows that change in every frame are in the image as one frame showed them.
    const std::string wrong = restless ? std::string() : h.exact(doc.img, 0);
    report(fmt("the tracker's expected offset stays within what a step owes (restless rows %d)", restless),
           worst <= 4 && h.end == SessionEnd::PageEnd && wrong.empty(),
           (wrong.empty() ? counts(h) : wrong) + fmt(", most past the step %.0f px, %d frames kept the hints", worst, kept));
  }
}

// A blank stretch under a pinned header and footer (#1543's review): the
// header and footer are no content of the page there; the blank check looks
// at the part that scrolls, so it is not called the end of the page.
void testBlankUnderPinnedParts() {
  std::printf("A blank stretch under a pinned header and footer\n");
  DocSpec ds;
  ds.seed = 99;
  ds.width = W;
  ds.viewport = H;
  ds.flatFactor = 0;
  ds.tableFactor = 0;
  ds.noiseFactor = 0;
  ds.gradientFactor = 0;
  ds.blankFactor = 2.5;
  ds.paragraphsPerGap = 6;
  const Document doc = makeDocument(ds);
  Harness h;
  h.headerH = 48;
  h.footerH = 32;
  FakeAppSpec as;
  as.pxPerNotch = 40;
  SessionOptions so = options(true);
  if (std::getenv("PUTGD_SCROLL_TRACE"))
    so.onFrame = [&](const TrackResult& r, const TrackerHint&, int64_t t) {
      std::printf("    t=%lld app=%d %s %s off=%d pos=%lld textured=%d\n", (long long)t, h.app->framePositions.back(), toString(r.status), toString(r.reason), r.offset,
                  r.position, r.texturedRows);
    };
  h.script = [](Harness& x, int64_t t) { if (t >= 120000) x.stop.store(true); };
  h.run(doc.img, as, so);
  report("auto-scroll at a blank stretch under a pinned header and footer: stops, not called the end",
         h.end == SessionEnd::PageEnd && h.stats.endBlank && !h.stats.reachedEnd,
         counts(h) + fmt(", blank end %d, reached end %d, %d pinned", int(h.stats.endBlank), int(h.stats.reachedEnd), h.stats.pinnedRegions));
}

// The end of a page whose last content rows sit above a blank tail, under a
// pinned header and footer (#1543's review: seed 51, no blank stretches, a
// tail one region tall; the tracker reviewer saw it go lost with no_match
// under auto-scroll and end idle, so the person would read "Auto-scroll lost
// the page" on a page that simply ended). Auto-scroll stops at the end.
void testEndAboveABlankTail() {
  std::printf("The end of a page with a blank tail\n");
  const Document d = page(51, 0);
  Image doc(W, d.img.h + H, kBg);
  std::memcpy(doc.px.data(), d.img.px.data(), d.img.px.size() * 4);
  Harness h;
  h.headerH = h.footerH = 48;
  FakeAppSpec as;
  as.pxPerNotch = 60;
  SessionOptions so = options(true);
  so.idleLimitMs = 60000;
  if (std::getenv("PUTGD_SCROLL_TRACE"))
    so.onFrame = [&](const TrackResult& r, const TrackerHint& hint, int64_t t) {
      std::printf("    t=%lld app=%d %s %s off=%d pos=%lld hint=%d/%d/%d state=%s\n", (long long)t, h.app->framePositions.back(), toString(r.status),
                  toString(r.reason), r.offset, r.position, int(hint.known), hint.offset, hint.uncertainty, toString(r.state));
    };
  h.script = [](Harness& x, int64_t t) { if (t >= 300000) x.stop.store(true); };
  h.run(doc, as, so);
  const long long full = h.headerH + doc.h + h.footerH;
  report("auto-scroll at the end of a page with a blank tail: ends there, not lost, the image no taller than the page",
         h.end == SessionEnd::PageEnd && !h.saw(SessionState::Paused, HandBack::Lost) && !h.saw(SessionState::Lost) && h.stats.bottom - h.stats.top <= full &&
             h.stats.bottom - h.stats.top >= h.headerH + d.img.h,
         counts(h) + fmt(", page with header and footer %lld rows, content to %d, image %lld, reachedEnd %d endBlank %d%s", full, h.headerH + d.img.h,
                         h.stats.bottom - h.stats.top, int(h.stats.reachedEnd), int(h.stats.endBlank), h.saw(SessionState::Lost) ? ", went lost" : ""));
}

// CodeRabbit on #1536. Auto-scroll turned off and on again before the loop
// read either request: the main thread took the watch and the rest down with
// the off, and the loop, seeing only the latest "on" while it still ran,
// never asked for them again. It takes the jump as off, then on, and asks.
void testOffAndOnBackToBack() {
  std::printf("Auto-scroll off and on before the loop reads either\n");
  const Document doc = page(96);
  Harness h;
  FakeAppSpec as;
  as.pxPerNotch = 40;
  SessionOptions so = options(true);
  int prepared = 0;
  so.beforeAuto = [&prepared]() -> std::optional<RunPoint> {
    ++prepared;
    return RunPoint{-16, H / 3};
  };
  bool toggled = false;
  h.script = [&toggled](Harness& x, int64_t t) {
    if (!toggled && t >= 3000 && x.last() == SessionState::AutoScrolling) {
      toggled = true;
      x.setAuto(false);
      x.setAuto(true);
    }
    if (t >= 120000) x.stop.store(true);
  };
  h.run(doc.img, as, so);
  const std::string wrong = h.exact(doc.img, 0);
  report("off then on back to back: the run asks for its surfaces again, and goes on to the end", toggled && prepared == 2 && h.end == SessionEnd::PageEnd && wrong.empty(),
         (wrong.empty() ? counts(h) : wrong) + fmt(", asked for its surfaces %d times", prepared));
}

// CodeRabbit on #1536. Done on the way to the top, while the stream sends its
// last picture again (KWin's region stream went stale while Chrome moved):
// the image is the screen as it is at Done, from a fresh frame.
void testDoneOnTheWayUpStale() {
  std::printf("Done on the way to the top on a stale stream\n");
  const Document doc = page(97);
  Harness h;
  FakeAppSpec as;
  as.pxPerNotch = 10;  // a way up of many steps
  as.startPos = 2400;
  as.staleFromMs = 2500;
  SessionOptions so = options(true);
  so.fromTop = true;
  bool doneOnWay = false;
  int atStale = -1;
  h.script = [&doneOnWay, &atStale](Harness& x, int64_t t) {
    if (atStale < 0 && t >= 2500) atStale = x.app->positionNow();
    if (t >= 3300 && !x.stop.load()) {
      doneOnWay = x.last() == SessionState::ScrollingToTop;
      x.stop.store(true);
    }
  };
  h.run(doc.img, as, so);
  const int now = h.app->positionNow();
  const std::string wrong = h.exact(doc.img, now);
  report("Done on the way up while the stream is stale: the image is the screen at Done",
         doneOnWay && atStale != now && h.end == SessionEnd::Finished && wrong.empty() && h.stats.bottom - h.stats.top == H,
         (wrong.empty() ? counts(h) : wrong) + fmt(", page at %d when the stream went stale, at %d at Done", atStale, now));
}

// CodeRabbit on #1536. The spool's disk fills while the person scrolls: the
// rows are given their blocks before they are written, so the write fails
// with a reason instead of SIGBUS in the copy, and the session ends at once.
void testSpoolDiskFull() {
  std::printf("The spool's disk fills\n");
  const Document doc = page(98);
  Harness h;
  h.spoolSpace = size_t(1200) * W * 4;  // about 1,200 rows of the image
  std::vector<FakeApp::UserScroll> script;
  for (int i = 0; i < (doc.img.h - H) / 120 + 4; ++i) script.push_back({1500 + 150 * i, 120, 100});
  h.script = [script](Harness& x, int64_t t) {
    if (x.app->script.empty()) x.play(script);
    if (t >= 60000) x.stop.store(true);
  };
  h.run(doc.img, FakeAppSpec{}, options(false));
  report("a full disk under the spool ends the capture at once, with a reason", h.end == SessionEnd::CanvasFailed && h.spool->canvas.failed() &&
                                                                               h.spool->canvas.error() == ENOSPC && h.stats.elapsedMs < 30000,
         fmt("%s after %lld ms, spool error %d", toString(h.end), (long long)h.stats.elapsedMs, h.spool->canvas.error()));
}

// CodeRabbit on #1536 (dropOwed). A step whose only frame is skipped (a
// foreign picture: a notification over the region), the page's own frames
// of the step never delivered (the stream says nothing until the page has
// come to rest and the next step moves it): where the page went since the
// tracker's last placed frame is not known. On content that repeats every
// step, forgetting what the step's hints said let the next frame be placed
// at the alias one step short, and the image lost a period. The hints stay
// with the tracker. The opposite case: the step at the page's end moves
// nothing, and its hints must not take a later frame a period on.
// The fixture itself: on a stream that sends a frame only when the picture
// changed, a foreign picture waiting to be shown is a change, at rest too.
void testFixtureForeignOnDamageOnly() {
  std::printf("The fixture: a foreign picture on a damage-only stream at rest\n");
  const Document doc = page(98);
  FakeClock clock;
  FakeAppSpec as;
  as.damageOnly = true;
  FakeApp app(doc.img, ViewportSpec{W, H, 0, 0, 7}, as, clock);
  const auto first = app.nextFrame(100);
  std::vector<uint8_t> row0;
  if (first) row0.assign(first->view.row(0), first->view.row(0) + size_t(W) * 4);
  const auto still = app.nextFrame(200);  // nothing changed: nothing sent
  app.foreignThenQuiet(500);
  const auto foreign = app.nextFrame(200);
  const bool differs = foreign && !row0.empty() && std::memcmp(foreign->view.row(0), row0.data(), row0.size()) != 0;
  report("a foreign picture is sent on a damage-only stream at rest", first && !still && foreign && differs,
         fmt("first %d, at rest %d, foreign %d, differs %d", bool(first), bool(still), bool(foreign), differs));
}

void testSkippedStepOnRepeatingContent() {
  std::printf("A step whose only frame is skipped, on content that repeats every step\n");
  const Document unique = page(99, 0);
  const int period = 160, repeats = 12, head = 1200, tail = 1200;
  // moved: the step moves the page inside the repeating part; end: the page
  // ends in the repeating part and the step there moves nothing; unique: a
  // page that does not repeat.
  // Not here: the same step's frame coming late, after the step's whole
  // distance was handed out. Its next frames show the page exactly one period
  // on, the same picture as the tracker's reference, which the tracker takes
  // for a page that did not move: a period is lost whatever the hints say.
  for (const char* kind : {"moved", "end", "end, late", "unique"}) {
    const bool repeating = std::strncmp(kind, "unique", 6) != 0, atEnd = std::strncmp(kind, "end", 3) == 0;
    const int delay = std::strstr(kind, "late") ? 400 : 0;
    Image doc(W, !repeating ? unique.img.h : head + repeats * period + (atEnd ? 0 : tail), kBg);
    if (repeating) {
      std::memcpy(doc.px.data(), unique.img.px.data(), size_t(head) * W * 4);
      for (int k = 0; k < repeats; ++k) {
        Rng ink(5);
        const int y0 = head + k * period;
        for (int y = 12; y < period - 20; y += 26) drawTextLine(doc, y0 + y, 24, W - 24, ink);
        fillRows(doc, y0 + period - 8, y0 + period - 4, 0x3060A0);
      }
      if (!atEnd) std::memcpy(&doc.px[size_t(head + repeats * period) * W], &unique.img.px[size_t(head) * W], size_t(tail) * W * 4);
    } else {
      doc = unique.img;
    }
    const int last = doc.h - H;
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;  // a step of 4 notches is 160 rows: the period
    bool fired = false;
    int firedAt = -1;
    h.script = [&fired, &firedAt, atEnd, last, delay](Harness& x, int64_t t) {
      const auto& e = x.app->events;
      const int at = x.app->positionNow();
      if (!fired && !e.empty() && e.back().kind == 's' && (atEnd ? at == last : e.back().a == 4 && at > 1300 && at < 2400)) {
        fired = true;
        firedAt = at;
        x.app->foreignThenQuiet(700, delay);
      }
      if (t >= 120000) x.stop.store(true);
    };
    SessionOptions so = options(true);
    if (std::getenv("PUTGD_SCROLL_TRACE")) {
      so.onFrame = [&](const TrackResult& r, const TrackerHint& hint, int64_t t) {
        std::printf("    t=%lld app=%d %s %s off=%d pos=%lld hint=%d/%d/%d expected=%d state=%s\n", (long long)t, h.app->framePositions.back(), toString(r.status),
                    toString(r.reason), r.offset, r.position, int(hint.known), hint.offset, hint.uncertainty, r.expected, toString(r.state));
      };
      so.onStep = [](int n, RunPoint) { std::printf("    step %d\n", n); };
    }
    h.run(doc, as, so);
    const std::string wrong = h.exact(doc, 0);
    report(fmt("one skipped frame in a step (%s): the image is exact as far as it goes", kind), fired && wrong.empty(),
           (wrong.empty() ? counts(h) : wrong) + fmt(", the step at %d, page %d rows, image to %lld", firedAt, doc.h, h.stats.bottom));
  }
}

// A blank stretch taller than the region under auto-scroll: no frame shows
// anything move while it fills the region, the tracker reports such frames
// still under any hint, and two still steps there look like the end. The
// session does not call that the end of the page (end_of_content would claim
// the whole page).
void testBlankStretch() {
  std::printf("A blank stretch taller than the region\n");
  DocSpec ds;
  ds.seed = 99;
  ds.width = W;
  ds.viewport = H;
  ds.flatFactor = 0;
  ds.tableFactor = 0;
  ds.noiseFactor = 0;
  ds.gradientFactor = 0;
  ds.blankFactor = 2.5;
  ds.paragraphsPerGap = 6;
  const Document doc = makeDocument(ds);
  // Also with put.gd's bar inside the region: its pixels change in every
  // frame, and the blank check leaves them out as the tracker does.
  const TrackerRect bar{W - 260, H - 60, 250, 50};
  for (const bool withBar : {false, true}) {
    Harness h;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    SessionOptions so = options(true);
    if (withBar) {
      as.barX = bar.x, as.barY = bar.y, as.barW = bar.w, as.barH = bar.h;
      so.excluded = bar;
    }
    h.run(doc.img, as, so);
    // It cannot be followed (frames that show nothing show no movement), so it
    // stops there; it is not called the end of the page. What it has is exact
    // (the bar's place left out).
    const std::string wrong = withBar ? std::string() : h.exact(doc.img, 0);
    report(fmt("auto-scroll at a blank stretch taller than the region%s: stops, not called the end", withBar ? ", the bar inside" : ""),
           h.end == SessionEnd::PageEnd && h.stats.endBlank && !h.stats.reachedEnd && wrong.empty() && h.stats.bottom < doc.img.h,
           (wrong.empty() ? counts(h) : wrong) + fmt(", page %d rows, blank end %d", doc.img.h, int(h.stats.endBlank)));
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
    report("a capture nobody touches after scrolling ends by itself, the image kept", h.end == SessionEnd::Idle && h.stats.elapsedMs < 8000 && wrong.empty(),
           wrong.empty() ? fmt("%s after %lld ms", toString(h.end), (long long)h.stats.elapsedMs) : wrong);
  }
  // Forgotten (#1536's review): nothing ever scrolled, by the person or by
  // auto-scroll, so the one screen is not saved: ended like Cancel. Also when
  // auto-scroll was on and found nothing that scrolls, and when the way to the
  // top moved the page but nothing after it did (not the image's movement).
  struct Forgotten { const char* name; bool autoScroll, fromTop; int startPos; };
  for (const Forgotten& c : {Forgotten{"nobody scrolls", false, false, 0}, Forgotten{"the way to the top only, then paused", true, true, 1200}}) {
    Harness h;
    SessionOptions so = options(c.autoScroll);
    so.idleLimitMs = 5000;
    so.fromTop = c.fromTop;
    FakeAppSpec as;
    as.pxPerNotch = 40;
    as.startPos = c.startPos;
    // The second: the mouse pauses auto-scroll right after the way to the top.
    h.script = [c](Harness& x, int64_t t) {
      if (c.fromTop && x.saw(SessionState::ScrollingToTop) && x.last() != SessionState::ScrollingToTop) x.paused = true;
      if (t >= 30000) x.stop.store(true);  // the bound must come first
    };
    h.run(doc.img, as, so);
    report(fmt("a capture nobody ever scrolls ends by itself like Cancel, no image (%s)", c.name),
           h.end == SessionEnd::Forgotten && h.stats.movedByUser + h.stats.movedByPutgd == 0 && (!c.fromTop || h.stats.toTopSteps > 0),
           fmt("%s after %lld ms, moved by user %d by put.gd %d, %d steps up", toString(h.end), (long long)h.stats.elapsedMs, h.stats.movedByUser, h.stats.movedByPutgd, h.stats.toTopSteps));
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
  testWheelTakenByAPart();
  testBarInside();
  testSwitchAndPause();
  testLostAndFound();
  testAutoJump();
  testReviewCases();
  testStaleStreamAtDone();
  testStaleStreamUnderAuto();
  testTopUnderADeadPoint();
  testOneFrameOutOfPlace();
  testHintsCountedOnce();
  testBlankUnderPinnedParts();
  testEndAboveABlankTail();
  testOffAndOnBackToBack();
  testDoneOnTheWayUpStale();
  testSpoolDiskFull();
  testSkippedStepOnRepeatingContent();
  testFixtureForeignOnDamageOnly();
  testBlankStretch();
  testCapAndIdle();
  testDoneAndCancel();
  std::printf("\n%d passed, %d failed\n", g_pass, g_fail);
  return g_fail == 0 ? 0 : 1;
}
