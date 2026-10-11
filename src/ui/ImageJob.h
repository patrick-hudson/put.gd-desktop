#pragma once
class QCoreApplication;
class QIODevice;
namespace putgd {
// Private no-window role of putgd-ui. QApplication is already initialized so
// image codecs, including SVG/font rendering, never enter a Core resident.
int runImageJob(QCoreApplication&,QIODevice&);
}
