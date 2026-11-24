#pragma once
#include "Error.h"
#include <QByteArray>
#include <QImage>
#include <QJsonObject>
#include <QString>
// QR codes for History's Show QR code and history.qr (#1428, decision 10),
// written with the vendored Nayuki generator (src/core/qrcodegen, v1.8.0).
// The resident (copy and save) and putgd-ui (the window) share this, so both
// draw exactly the same modules: error correction exactly M with boostEcl off,
// byte mode, a 4-module quiet zone, whole pixels per module, black on white.
namespace putgd::qr {
// Version 40 at level M holds 2,331 bytes in byte mode; a longer link is
// refused with qr_too_long.
inline constexpr qsizetype maximumBytes=2331;
inline constexpr int quietZone=4;
// The window draws the code in an area of at least this many pixels a side,
// at the largest whole number of pixels per module that fits, never under 3.
inline constexpr int windowArea=560,minimumModulePixels=3;
// Copy image and Save write a PNG at the smallest whole scale reaching this edge.
inline constexpr int exportEdge=1024;
struct Matrix {
 int size=0; // modules a side, without the quiet zone (21 to 177)
 QByteArray modules; // size*size bytes, 1 for a dark module, row by row
 bool dark(int x,int y) const {return x>=0&&y>=0&&x<size&&y<size&&modules.at(qsizetype(y)*size+x)!=0;}
 int edge() const {return size+2*quietZone;} // modules a side with the quiet zone
 bool isNull() const {return size==0;}
};
// Whether History offers Show QR code for an item (a history.detail record):
// the link Copy link would copy, now or after a receipt refresh.
bool offered(const QJsonObject& detail);
// Why it does not: link_unavailable with details.reason private, revoked,
// removed, expired (and cannot be refreshed) or none, and a message saying so.
Error unavailable(const QJsonObject& detail);
// The code for these bytes (a link as Copy link copies it, in UTF-8), or
// qr_too_long over maximumBytes, or qr_empty for nothing.
Error encode(const QByteArray& bytes,Matrix&);
// Pixels per module for the window: the largest whole number that fits the
// code and its quiet zone into `area` pixels a side, never under 3.
int modulePixels(const Matrix&,int area=windowArea);
// Pixels per module of the exported PNG: the smallest whole scale whose edge
// with the quiet zone reaches exportEdge.
int exportScale(const Matrix&);
// The code at `scale` pixels per module with its quiet zone, black on white.
QImage image(const Matrix&,int scale);
// The exported PNG, a 1-bit greyscale image at exportScale with only IHDR,
// IDAT and IEND (it passes inspectSanitizedPng). Written by hand with zlib, so
// the resident loads no image plugin to make it.
QByteArray png(const Matrix&);
}
