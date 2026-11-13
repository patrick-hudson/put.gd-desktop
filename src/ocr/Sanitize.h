#pragma once
#include <QByteArray>
#include <QString>

// Recognized text and code content come from whatever was on screen, so they
// are untrusted (#1428 decision 4a). Everything the helper returns passes here.
namespace putgd::ocr {
enum class Untrusted { RecognizedText, CodeContent };
// Removes C0 controls except tab and newline, DEL, C1 controls and every
// format character (category Cf, which includes the bidirectional overrides,
// embeddings and isolates, zero-width and tag characters); U+2028 and U+2029
// become newlines; the result is in NFC. Recognized text keeps U+200C and
// U+200D, which Persian and Indic scripts need; code content keeps neither.
QString sanitizeUntrusted(const QString& text, Untrusted kind);
// Strict UTF-8: false for any malformed sequence, overlong form or surrogate.
bool decodeUtf8(const QByteArray& bytes, QString& text);
// The longest prefix of text whose UTF-8 form fits in maximumBytes, cut on a
// code point boundary. cut is set when anything was removed.
QString clampUtf8(const QString& text, qsizetype maximumBytes, bool& cut);
}
