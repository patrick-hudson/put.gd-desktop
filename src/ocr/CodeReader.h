#pragma once
#include "OcrImage.h"
#include <QList>

namespace putgd::ocr {
// QR Code, Micro QR Code, Data Matrix and Aztec from the whole grey image, in
// order top to bottom, then left to right, by the centre of their corners.
// Linear barcodes and PDF417 are not read. A code that fails to decode is left
// out. Text content is sanitized; the contract's encoder applies the bounds.
QList<Code> readCodes(const GreyView& image);
}
