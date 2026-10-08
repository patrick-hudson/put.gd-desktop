#include "ui/VisualStyle.h"
#include <QAction>
#include <QApplication>
#include <QMenu>
#include <QSocketNotifier>
#include <QStringList>
#include <QSystemTrayIcon>
#include <QTimer>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unistd.h>
#ifdef __GLIBC__
#include <malloc.h>
#endif

int main(int argc, char** argv) {
 QApplication app(argc, argv);
 app.setQuitOnLastWindowClosed(false);
 app.setDesktopFileName("putgd");
 QCoreApplication::setOrganizationName("putgd-native-cold-probe");
 QCoreApplication::setApplicationName("putgd-native-cold-probe");
 const int settleMs = argc == 2 ? std::atoi(argv[1]) : 5000;
 if(settleMs < 1000 || settleMs > 30000) return 2;
 // This executable creates only its own icon/menu; it has no product services.
 QIcon brand;
 std::unique_ptr<QSystemTrayIcon> tray;
 std::unique_ptr<QMenu> menu;
 const char* stages[] = {"native-empty", "visual-style", "brand-icon", "tray-icon",
                         "empty-tray-menu", "populated-tray-menu", "after-one-trim"};
 int stage = 0;
 bool waiting = false;
 bool trimSupported = false;
 int trimReturned = -1;
 long long trimUsec = 0;
 const auto snapshot = [&] {
  unsigned long long arena=0, allocated=0, freeArena=0, mmapBytes=0;
#ifdef __GLIBC__
  const auto m = mallinfo2();
  arena=m.arena; allocated=m.uordblks; freeArena=m.fordblks; mmapBytes=m.hblkhd;
#endif
  std::printf("{\"stage\":\"%s\",\"pid\":%ld,\"qtVersion\":\"%s\","
              "\"platform\":\"%s\","
              "\"styleCppSha256\":\"%s\",\"styleHeaderSha256\":\"%s\","
              "\"probeCppSha256\":\"%s\",\"arenaBytes\":%llu,"
              "\"allocatedBytes\":%llu,\"freeArenaBytes\":%llu,\"mmapBytes\":%llu,"
              "\"trayAvailable\":%s,\"trimSupported\":%s,\"trimReturned\":%d,\"trimUsec\":%lld}\n",
              stages[stage], static_cast<long>(getpid()), qVersion(),
              QGuiApplication::platformName().toUtf8().constData(), STYLE_CPP_SHA,
              STYLE_H_SHA, PROBE_CPP_SHA, arena, allocated, freeArena, mmapBytes,
              tray && tray->isSystemTrayAvailable() ? "true" : "false",
              trimSupported ? "true" : "false", trimReturned, trimUsec);
  std::fflush(stdout);
  waiting = true;
 };
 QSocketNotifier input(STDIN_FILENO, QSocketNotifier::Read, &app);
 QObject::connect(&input, &QSocketNotifier::activated, &app, [&] {
  char byte;
  if(::read(STDIN_FILENO, &byte, 1) != 1) { app.quit(); return; }
  if(byte != 'n' || !waiting) return;
  waiting = false;
  ++stage;
  switch(stage) {
   case 1: putgd::VisualStyle::apply(app); break;
   case 2: brand=putgd::VisualStyle::icon("brand"); app.setWindowIcon(brand); break;
   case 3:
    tray=std::make_unique<QSystemTrayIcon>(brand);
    tray->setToolTip("put.gd — Share and local capture");
    if(tray->isSystemTrayAvailable()) tray->show();
    break;
   case 4: menu=std::make_unique<QMenu>(); tray->setContextMenu(menu.get()); break;
   case 5:
    // Exact labels/icons/order from TrayController; capture entries have no effects.
    for(const auto& action:QStringList{"region","window","desktop","clipboard"})
     menu->addAction(putgd::VisualStyle::icon(action), "Capture "+action);
    menu->addSeparator();
    menu->addAction(putgd::VisualStyle::icon("preferences"), "Settings and setup");
    menu->addAction(putgd::VisualStyle::icon("history"), "Upload history");
    menu->addAction(putgd::VisualStyle::icon("activity"), "Recent activity");
    menu->addSeparator();
    QObject::connect(menu->addAction("Quit"), &QAction::triggered, &app, &QApplication::quit);
    break;
   case 6: {
    const auto started=std::chrono::steady_clock::now();
#ifdef __GLIBC__
    trimSupported=true; trimReturned=malloc_trim(0);
#endif
    trimUsec=std::chrono::duration_cast<std::chrono::microseconds>(
     std::chrono::steady_clock::now()-started).count();
    break;
   }
   default: tray->hide(); tray->setContextMenu(nullptr); app.quit(); return;
  }
  QTimer::singleShot(settleMs, &app, snapshot);
 });
 QTimer::singleShot(settleMs, &app, snapshot);
 return app.exec();
}
