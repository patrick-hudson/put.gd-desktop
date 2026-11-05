// #418: decode one file as put.gd's History detail does (HistoryWindow.cpp:240-241):
// QImageReader on the path with format auto-detection, size(), then a scaled read().
#include <QGuiApplication>
#include <QImageReader>
#include <QImage>
#include <cstdio>
int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  QImageReader reader(QString::fromLocal8Bit(argv[1]));
  const QSize size = reader.size();
  std::printf("format=%s size=%dx%d", reader.format().constData(), size.width(), size.height());
  if (size.isValid() && qint64(size.width()) * size.height() <= 16000000) reader.setScaledSize(size.scaled(900, 540, Qt::KeepAspectRatio));
  const QImage image = reader.read();
  std::printf(" read=%s error=\"%s\"\n", image.isNull() ? "null" : "ok", qPrintable(reader.errorString()));
  return 0;
}
