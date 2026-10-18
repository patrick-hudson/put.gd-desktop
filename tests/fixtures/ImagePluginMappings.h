#pragma once
#include <QByteArray>
#include <QColor>
#include <QFile>
#include <QImage>
#include <QStringList>
namespace putgd::test {
// Image plugins whose code is mapped into this process; dlopen maps an
// executable segment. Tests using this never encode or decode with Qt image
// codecs themselves, so any plugin present was loaded by the code under test.
inline QStringList loadedImagePlugins(){
 QFile maps("/proc/self/maps");if(!maps.open(QIODevice::ReadOnly))return {"/proc/self/maps unreadable"};
 QStringList loaded;
 for(const auto& line:maps.readAll().split('\n')){
  const auto fields=line.simplified().split(' ');
  if(fields.size()>=6&&fields[1].contains('x')&&fields[5].contains("/imageformats/")){const auto path=QString::fromLocal8Bit(fields[5]);if(!loaded.contains(path))loaded.append(path);}
 }
 return loaded;
}
// 3x2 RGB images: red, green, blue over yellow, cyan, magenta. Encoded by
// hand so a test process never asks Qt for an image writer.
inline const QByteArray syntheticPng=QByteArray::fromHex("89504e470d0a1a0a0000000d49484452000000030000000208020000001216f14d000000154944415478da63f8cfc0c000c1ff81d47f20f11f004ac908f85fb37d750000000049454e44ae426082");
inline const QByteArray syntheticBmp=QByteArray::fromHex("424d4e0000000000000036000000280000000300000002000000010018000000000018000000130b0000130b0000000000000000000000ffffffff00ff00ff0000000000ff00ff00ff0000000000");
inline bool syntheticPixels(const QImage& image){
 return image.size()==QSize(3,2)&&image.pixelColor(0,0)==QColor(Qt::red)&&image.pixelColor(1,0)==QColor(Qt::green)&&image.pixelColor(2,0)==QColor(Qt::blue)
  &&image.pixelColor(0,1)==QColor(Qt::yellow)&&image.pixelColor(1,1)==QColor(Qt::cyan)&&image.pixelColor(2,1)==QColor(Qt::magenta);
}
}
