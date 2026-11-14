// Replay tool for the scroll tracker (#1515): feeds a recorded sequence of
// frames through ScrollTracker (or, for a before and after, through today's
// ScrollStitcher) and writes the image and every frame's result as JSON. Lab
// input, never desktop proof; tools/lab/scroll_tracker_score.py compares the
// output with the browser's own full-page picture and the true scroll position.
//
//   test_scroll_tracker_replay --list FILE --out DIR [--engine tracker|stitcher]
//       [--repeat N] [--max-height N]
//
// FILE has one frame per line: a PNG path, optionally followed by a hint
// (expected offset since the line before, and its uncertainty).
#include "core/ScrollStitcher.h"
#include "core/ScrollTracker.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QFile>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <vector>

using namespace putgd::scroll;

namespace {

struct Line {
  QString path;
  TrackerHint hint;
};

QImage load(const QString& path) {
  QImage image(path);
  if (image.isNull()) return image;
  return image.convertToFormat(QImage::Format_RGB32);
}

FrameView viewOf(const QImage& image) {
  return {image.constBits(), image.width(), image.height(), std::ptrdiff_t(image.bytesPerLine())};
}

bool save(const std::vector<uint8_t>& pixels, int width, long long rows, const QString& path) {
  if (rows <= 0) return false;
  QImage out(reinterpret_cast<const uchar*>(pixels.data()), width, int(rows), width * 4, QImage::Format_RGB32);
  return out.copy().save(path, "PNG");
}

// The memory canvas, timed: what the engine itself costs is a frame's time
// less the time its writes and reads take here (the helper's spool file has
// costs of its own).
class TimedCanvas final : public TrackerCanvas {
 public:
  explicit TimedCanvas(int width) : inner(width) {}
  void writeSpan(long long row, int x0, int x1, const uint8_t* bgrx) override {
    const auto t0 = std::chrono::steady_clock::now();
    inner.writeSpan(row, x0, x1, bgrx);
    ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
  }
  bool readRow(long long row, uint8_t* bgrx) const override {
    const auto t0 = std::chrono::steady_clock::now();
    const bool ok = inner.readRow(row, bgrx);
    ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    return ok;
  }
  long long top() const override { return inner.top(); }
  long long bottom() const override { return inner.bottom(); }
  MemoryCanvas inner;
  mutable double ms = 0;
};

double percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0;
  std::sort(v.begin(), v.end());
  return v[std::min(v.size() - 1, size_t(q * double(v.size() - 1) + 0.5))];
}

}  // namespace

int main(int argc, char** argv) {
  QCoreApplication app(argc, argv);
  QCommandLineParser parser;
  parser.addOptions({{"list", "frames, one PNG path per line [hint uncertainty]", "file"},
                     {"out", "an existing directory for image.png and results.json", "dir"},
                     {"engine", "tracker or stitcher", "name", "tracker"},
                     {"repeat", "run the sequence this many times for timing (the image is the first run's)", "n", "1"},
                     {"max-height", "height cap", "rows", "32000"}});
  parser.process(app);
  if (!parser.isSet("list") || !parser.isSet("out")) {
    std::fprintf(stderr, "usage: --list FILE --out DIR [--engine tracker|stitcher]\n");
    return 2;
  }
  QFile listFile(parser.value("list"));
  if (!listFile.open(QIODevice::ReadOnly | QIODevice::Text)) return 2;
  std::vector<Line> lines;
  QTextStream in(&listFile);
  while (!in.atEnd()) {
    const QStringList parts = in.readLine().split(' ', Qt::SkipEmptyParts);
    if (parts.isEmpty()) continue;
    Line l;
    l.path = parts[0];
    if (parts.size() >= 3) l.hint = {true, parts[1].toInt(), parts[2].toInt()};
    lines.push_back(l);
  }
  std::vector<QImage> frames;
  for (const Line& l : lines) {
    frames.push_back(load(l.path));
    if (frames.back().isNull()) {
      std::fprintf(stderr, "cannot read %s\n", qPrintable(l.path));
      return 3;
    }
  }
  const QString out = parser.value("out");
  const bool stitcher = parser.value("engine") == "stitcher";
  const int repeat = std::max(1, parser.value("repeat").toInt());
  QJsonArray results;
  QJsonObject summary;
  std::vector<double> ms, engineMs;
  using Clock = std::chrono::steady_clock;

  if (!stitcher) {
    for (int run = 0; run < repeat; ++run) {
      TimedCanvas canvas(frames[0].width());
      TrackerOptions opts;
      opts.maxOutputRows = parser.value("max-height").toLongLong();
      ScrollTracker tracker(canvas, opts);
      for (size_t i = 0; i < frames.size(); ++i) {
        const double canvasBefore = canvas.ms;
        const auto t0 = Clock::now();
        const TrackResult r = tracker.addFrame(viewOf(frames[i]), lines[i].hint);
        const double t = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        ms.push_back(t);
        engineMs.push_back(t - (canvas.ms - canvasBefore));
        if (run > 0) continue;
        results.append(QJsonObject{{"file", lines[i].path},
                                   {"status", toString(r.status)},
                                   {"reason", toString(r.reason)},
                                   {"offset", r.offset},
                                   {"position", double(r.position)},
                                   {"addedTop", r.addedTop},
                                   {"addedBottom", r.addedBottom},
                                   {"confidence", r.confidence},
                                   {"byExpectation", r.byExpectation},
                                   {"onHint", r.onHint},
                                   {"recovered", r.recovered},
                                   {"state", toString(r.state)},
                                   {"ms", t}});
      }
      const auto t0 = Clock::now();
      const TrackTotals tot = tracker.finish();
      const double finishMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      if (run > 0) continue;
      int cols = 0, rowsOut = 0;
      const std::vector<uint8_t> mask = tracker.pinnedMask(cols, rowsOut);
      QJsonArray maskRows;
      for (int r = 0; r < rowsOut; ++r) {
        QString s;
        for (int c = 0; c < cols; ++c) s += mask[size_t(r) * cols + c] ? '#' : '.';
        maskRows.append(s);
      }
      summary = QJsonObject{{"engine", "tracker"},
                            {"frames", tot.frames},
                            {"placed", tot.placed},
                            {"noMovement", tot.noMovement},
                            {"skipped", tot.skipped},
                            {"invalid", tot.invalid},
                            {"longestSkipRun", tot.longestSkipRun},
                            {"recovered", tot.recovered},
                            {"byExpectation", tot.byExpectation},
                            {"onHint", tot.onHint},
                            {"pinnedRegions", tot.pinnedRegions},
                            {"capReached", tot.capReached},
                            {"top", double(tot.top)},
                            {"bottom", double(tot.bottom)},
                            {"finishMs", finishMs},
                            {"cellPx", tracker.options().cellPx},
                            {"blockRows", tracker.options().blockRows},
                            {"pinnedMask", maskRows}};
      if (!save(canvas.inner.image(tot.top, tot.bottom), canvas.inner.width(), tot.bottom - tot.top, out + "/image.png")) {
        std::fprintf(stderr, "cannot write the image\n");
        return 4;
      }
    }
  } else {
    // Today's stitcher, fed as the control loop would: the hint is the last
    // proved step (the loop's notches times its measured pixels per notch),
    // and the capture ends at the first step it cannot match, as the loop
    // does after its retry and its way back show the same picture.
    StitchOptions so;
    so.maxOutputRows = parser.value("max-height").toInt();
    ScrollStitcher st(so);
    int refPos = 0, lastStep = kNoHint;
    QString ended;
    for (size_t i = 0; i < frames.size(); ++i) {
      const auto t0 = Clock::now();
      const StepResult r = st.addFrame(viewOf(frames[i]), lastStep);
      const double t = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
      ms.push_back(t);
      const bool moved = r.status == StepStatus::Appended || r.status == StepStatus::Reversed || r.status == StepStatus::LimitReached;
      if (moved) {
        refPos += r.offset;
        if (r.offset > 0) lastStep = r.offset;
      }
      const char* status = r.status == StepStatus::First || moved ? "placed"
                           : r.status == StepStatus::NoMovement   ? "no_movement"
                                                                  : "skipped";
      results.append(QJsonObject{{"file", lines[i].path},
                                 {"status", status},
                                 {"reason", toString(r.status)},
                                 {"offset", r.offset},
                                 {"position", refPos},
                                 {"addedBottom", r.appendedRows},
                                 {"confidence", r.score},
                                 {"ms", t}});
      if (r.status == StepStatus::NoMatch || r.status == StepStatus::Ambiguous || r.status == StepStatus::LayoutChanged ||
          r.status == StepStatus::LimitReached) {
        ended = toString(r.status);
        break;
      }
    }
    st.finish();
    const int h = st.outputHeight();
    summary = QJsonObject{{"engine", "stitcher"}, {"frames", int(frames.size())}, {"fed", int(results.size())}, {"endedOn", ended},
                          {"top", 0}, {"bottom", h}};
    if (!save(st.takeImage(), frames[0].width(), h, out + "/image.png")) {
      std::fprintf(stderr, "cannot write the image\n");
      return 4;
    }
  }
  double sum = 0;
  for (double v : ms) sum += v;
  summary["msMean"] = ms.empty() ? 0 : sum / double(ms.size());
  summary["msP50"] = percentile(ms, 0.5);
  summary["msP95"] = percentile(ms, 0.95);
  summary["msMax"] = ms.empty() ? 0 : *std::max_element(ms.begin(), ms.end());
  if (!engineMs.empty()) {
    double e = 0;
    for (double v : engineMs) e += v;
    summary["engineMsMean"] = e / double(engineMs.size());
    summary["engineMsP50"] = percentile(engineMs, 0.5);
    summary["engineMsP95"] = percentile(engineMs, 0.95);
    summary["engineMsMax"] = *std::max_element(engineMs.begin(), engineMs.end());
  }
  summary["width"] = frames[0].width();
  summary["height"] = frames[0].height();
  QFile f(out + "/results.json");
  if (!f.open(QIODevice::WriteOnly)) return 4;
  f.write(QJsonDocument(QJsonObject{{"summary", summary}, {"frames", results}}).toJson(QJsonDocument::Indented));
  std::printf("%s\n", QJsonDocument(summary).toJson(QJsonDocument::Compact).constData());
  return 0;
}
