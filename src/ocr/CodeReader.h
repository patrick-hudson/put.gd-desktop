#pragma once
#include "OcrImage.h"
#include <QList>

namespace putgd::ocr {
// At most this many codes per job, each at most this many bytes of content.
inline constexpr int maximumCodes=64;
inline constexpr qsizetype maximumCodeBytes=8*1024;
// QR Code, Micro QR Code, Data Matrix and Aztec from the whole grey image, in
// order top to bottom, then left to right, by the centre of their corners.
// Linear barcodes and PDF417 are not read. A code that fails to decode is left
// out. truncated is set when a code was cut at maximumCodeBytes or codes were
// left out at maximumCodes.
QList<Code> readCodes(const GreyView& image,bool& truncated);
}
