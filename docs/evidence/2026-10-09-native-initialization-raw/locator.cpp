#include <QCoreApplication>
#include <QDBusConnection>
#include <QTimer>
#if defined(LOCATOR_gui)
#include <QGuiApplication>
using Application = QGuiApplication;
constexpr const char* applicationType = "QGuiApplication";
#elif defined(LOCATOR_widgets)
#include <QApplication>
using Application = QApplication;
constexpr const char* applicationType = "QApplication";
#elif defined(LOCATOR_core)
using Application = QCoreApplication;
constexpr const char* applicationType = "QCoreApplication";
#else
#error Select one locator application type
#endif
#include <charconv>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <system_error>

int main(int argc, char** argv) {
  int idleSeconds = 5;
  bool connectSessionBus = false;
  bool durationSeen = false;
  for (int index = 1; index < argc; ++index) {
    const std::string_view argument(argv[index]);
    if (argument == "--session-bus" && !connectSessionBus) {
      connectSessionBus = true;
    } else if (argument == "--idle-seconds" && !durationSeen && index + 1 < argc) {
      durationSeen = true;
      const std::string_view value(argv[++index]);
      const auto parsed = std::from_chars(value.data(), value.data() + value.size(), idleSeconds);
      if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
          idleSeconds < 2 || idleSeconds > 60) {
        std::fputs("invalid_idle_seconds: expected an integer from 2 to 60\n", stderr);
        return 2;
      }
    } else {
      std::fputs("usage: init-{core,gui,widgets} [--idle-seconds 5] [--session-bus]\n", stderr);
      return 2;
    }
  }

  const auto started = std::chrono::steady_clock::now();
  Application app(argc, argv);
  const auto emitStage = [&](const char* stage) {
    const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - started).count();
    std::printf("{\"schemaVersion\":1,\"applicationType\":\"%s\",\"stage\":\"%s\","
                "\"elapsedMicroseconds\":%lld,\"idleSeconds\":%d,\"sessionBusRequested\":%s,"
                "\"qtCompileVersion\":\"%s\",\"qtRuntimeVersion\":\"%s\"}\n",
                applicationType, stage, static_cast<long long>(elapsed), idleSeconds,
                connectSessionBus ? "true" : "false", QT_VERSION_STR, qVersion());
    std::fflush(stdout);
  };
  emitStage("constructed");
  if (connectSessionBus) {
    // Delay the optional connection so an external observer can record the
    // constructor phase first. No service name, object, menu or icon is exported.
    QTimer::singleShot(1000, &app, [&] {
      const auto bus = QDBusConnection::sessionBus();
      if (!bus.isConnected()) {
        emitStage("session-bus-unavailable");
        app.exit(3);
        return;
      }
      emitStage("session-bus-connected");
    });
  }
  QTimer::singleShot(idleSeconds * 1000, &app, [&] {
    emitStage("idle-finished");
    app.quit();
  });
  return app.exec();
}
