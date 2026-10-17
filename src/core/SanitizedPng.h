#pragma once
#include "Error.h"
#include <QByteArray>
#include <QSize>
namespace putgd {
// Structural check for a finalized editor export. Accepts exactly one PNG
// stream with valid CRCs and only pixel/rendering chunks: IHDR, PLTE, tRNS,
// gAMA, cHRM, sRGB, pHYs, IDAT and a final IEND with no trailing bytes.
// Text, EXIF, time, ICC, private and unknown chunks are rejected. This does
// not decode IDAT; pixel admission remains the caller's separate check.
Error inspectSanitizedPng(const QByteArray&,QSize* size=nullptr);
}
