#pragma once
#include "Error.h"
#include <QByteArray>
#include <QJsonArray>
#include <QJsonObject>
#include <QList>
#include <QPoint>
#include <QRect>
#include <QString>
#include <QStringList>
// The text-job contract (#1428): what the parent (TextJob, in the resident and
// in the editor) and the temporary putgd-ocr helper exchange, the bounds of a
// result, both error vocabularies, the rules for what may be copied or shown,
// and the language data table. Qt Core only: the helper links this library and
// never putgd_core, which brings Qt GUI and Qt SQL.
// schemas/text-recognition.schema.json states the same shapes as data, with
// fixtures under tests/fixtures/text/.
namespace putgd {
struct TextWord {
 QString text;
 QRect rect; // source pixels
 int confidence=0; // 0 to 100
 // #1911: from the second read of an uncertain token's line, a candidate only.
 // Quick redact matches these words apart from the first read's.
 bool second=false;
};
struct TextCode {
 QString format; // one of textjob::codeFormats()
 bool binary=false; // content that is not valid text: its length only, never copied or shown
 QString text; // empty when binary
 qint64 length=0; // bytes of content
 QList<QPoint> quad; // four corners in source pixels: top left, top right, bottom right, bottom left
 QPoint centre() const;
};
struct TextResult {
 // The text for copying (filtered words, line breaks kept), without code content.
 QString text;
 // The top edge of each line of `text` in source pixels, one per line. A code's
 // line goes before the first text line whose top edge is below its centre.
 QList<int> lineTops;
 QList<TextWord> words; // the filtered words of `text`; only when asked for
 QList<TextWord> candidates; // every recognized word, unfiltered; the editor only
 QList<TextCode> codes; // top to bottom, then left to right, by centre
 bool truncated=false,banded=false;
 QStringList languagesUsed,languagesSkipped;
 // #1981: the script check of a job that named no languages. Empty outcome:
 // it did not run (nothing garbled enough, or languages were chosen). script
 // is Tesseract's OSD name (Han, Cyrillic, ...), language the one it maps to;
 // outcome is one of textjob::detectionOutcomes().
 QString detectedScript,detectedLanguage,detectionOutcome;
 // Timed by the helper; the parent adds spawnMs (start to ready) and totalMs.
 qint64 engineStartMs=0,recognizeMs=0,spawnMs=0,totalMs=0;
 // #2134: the engine's version (Tesseract's own string) and the largest scale a panel was read at
 // (1 is the image's size, 2 twice it), with the number of bands: what a false-alarm report records,
 // since the boxes come back in source pixels. Empty, 0 and 0 from an engine that doesn't say.
 QString engineVersion;double engineScale=0;int engineBands=0;
 // Codes whose content is text: what a notification counts and a copy carries.
 int textCodes() const;
};
namespace textjob {
inline constexpr char role[]="text-job";
inline constexpr char generation[]="1";
// The two memfds arrive in the child at these descriptor numbers.
inline constexpr int inputDescriptor=3,outputDescriptor=4;
// The parent's deadline for a whole job, fixed before it prepares the input.
inline constexpr int maximumDeadlineMs=20000;
// Admission: above this the input is refused with input_excessive.
inline constexpr qint64 maximumInputPixels=24000000;
inline constexpr int maximumEdge=32768;
// Result bounds. The output cap governs them together: candidates are cut
// first, then words, then codes, then text; `truncated` says anything was cut.
inline constexpr qint64 maximumOutputBytes=8*1024*1024;
inline constexpr qint64 maximumTextBytes=1024*1024;
inline constexpr int maximumWords=20000,maximumWordBytes=256;
inline constexpr int maximumCodes=64,maximumCodeBytes=8*1024;
inline constexpr int maximumLanguages=3;
inline constexpr qint64 maximumLanguageDataBytes=64*1024*1024;
// The child's environment is cleared, then holds only these (decision 2).
// The data path is a colon-separated list of directories in lookup order.
inline constexpr char dataPathVariable[]="PUTGD_TEXT_DATA";
// "1": Unhinged mode (#1937), every CPU and the memory free allows.
inline constexpr char unhingedVariable[]="PUTGD_TEXT_UNHINGED";
// The input memfd: a 32-byte little-endian header (magic, version, width,
// height, stride, colour stride, two reserved zero words), then stride*height
// bytes of 8-bit grey, then colourStride*height bytes of 8-bit RGB, both
// flattened onto white (#1748: the colour finds emoji). Rows are packed: the
// stride is the width and the colour stride three times it. Sealed against write,
// shrink and grow before the child starts.
inline constexpr char inputMagic[4]={'P','G','T','J'};
inline constexpr quint32 inputVersion=2;
inline constexpr qsizetype inputHeaderBytes=32;
inline constexpr int requiredSeals=0x0002|0x0004|0x0008; // F_SEAL_SHRINK|F_SEAL_GROW|F_SEAL_WRITE
struct InputHeader {int width=0,height=0,stride=0,colourStride=0;};
QByteArray encodeInputHeader(const InputHeader&);
// False unless the header is well formed, admitted and `bytes` is exactly its size.
bool decodeInputHeader(const QByteArray& header,qint64 bytes,InputHeader&);
bool admitted(int width,int height,int stride,int colourStride);
// The whole memfd: header, grey plane and colour plane.
qint64 inputBytes(const InputHeader&);
qint64 colourOffset(const InputHeader&);

struct Options {
 bool words=false; // word boxes of the copied text
 bool candidates=false; // unfiltered candidates (Quick redact)
 QStringList languages; // 1 to 3 valid identifiers
 // Named by a caller: one that cannot load fails the job. From Settings: one
 // that cannot load is skipped when another loads.
 bool languagesRequired=false;
 // #1981: languages nobody chose (no parameter, no saved preference): lines
 // the first pass read badly may have their script detected and be read again
 // in its language. Off when a person or caller picked the languages.
 bool detectScript=false;
 // Unhinged mode (#1937): the parent tells the helper in its environment
 // (unhingedVariable), before the helper confines itself; never in the bootstrap.
 bool allCores=false;
 bool operator==(const Options&) const=default;
};
// Decision 1: the code kinds read. Linear barcodes and PDF417 are not.
QStringList codeFormats(); // QRCode, MicroQRCode, DataMatrix, Aztec
// Decision 5: the Quick redact categories of editor.apply's redactText, in
// the plan's order: email addresses, phone numbers, IPv4 and IPv6 addresses,
// card numbers, keys and tokens.
QStringList redactCategories(); // email, phone, ip, card, key, name, list
// #1618: limits and checks for Quick redact terms and patterns, shared by
// Settings (preferences.quickRedactTerms / quickRedactPatterns) and redactText.
// The messages never repeat the value.
inline constexpr int maxRedactTerms=200,maxRedactTermLength=100;
inline constexpr int maxRedactPatterns=50,maxRedactPatternLength=500;
QString redactTermError(const QString& term); // empty when valid
QString redactPatternError(const QString& pattern); // empty when valid; compile errors give the offset
QStringList childErrorCodes(); // decision 2, from the helper
QStringList parentErrorCodes(); // decision 2, from the parent
// The fixed message of a code; no paths or payload. language is named only for
// language_unavailable.
Error failure(const QString& code,const QString& language={});

// Control frames, on the image worker's UiControl transport.
QJsonObject envelope(const QString& nonce);
bool validEnvelope(const QJsonObject& payload,const QString& nonce);
// bootstrap: role, nonce, generation, build, task, input, options, limits.
QJsonObject bootstrap(const QString& nonce,const QJsonObject& build,const InputHeader&,const Options&);
bool decodeBootstrap(const QJsonObject& payload,const QJsonObject& build,QString& nonce,InputHeader&,Options&);
// ready: the envelope and the child's build.
QJsonObject ready(const QString& nonce,const QJsonObject& build);
bool validReady(const QJsonObject& payload,const QString& nonce,const QJsonObject& build);
// checkpoint: the envelope, ok, and bytes and sha256 of the output memfd, or code
// (and language for language_unavailable).
QJsonObject checkpoint(const QString& nonce,const QByteArray& output);
QJsonObject checkpointFailure(const QString& nonce,const QString& code,const QString& language={});
// True for a well-formed checkpoint; error is the child's failure, else bytes and sha256.
bool decodeCheckpoint(const QJsonObject& payload,const QString& nonce,Error& error,qint64& bytes,QString& sha256);

// The output memfd's JSON. encodeResult applies every bound and the cut order,
// and sets `truncated`; it never returns more than maximumOutputBytes.
QByteArray encodeResult(TextResult,const Options&);
// Strict: any field outside the contract or the bounds, or geometry outside the
// image, is refused (text_incompatible). What it returns has passed sanitize().
bool decodeResult(const QByteArray& json,const InputHeader&,const Options&,TextResult&);
// Words and codes as text.inspect and editor.inspect return them.
QJsonArray wordsJson(const QList<TextWord>&);
QJsonArray codesJson(const QList<TextCode>&);

// Decision 4a. Recognized text and code content are screen data from an
// untrusted source: before any copy, display or agent reply, C0 controls
// except tab and newline, C1 controls and every format character (bidirectional
// overrides, embeddings and isolates, zero-width, invisible and tag characters)
// are removed, U+2028 and U+2029 become newlines and the text is put in NFC.
// Recognized text keeps U+200C and U+200D, and the tags of the three flags that
// are tag sequences (England, Scotland, Wales); code content keeps no format
// character.
QString sanitizeText(const QString&);
QString sanitizeCodeContent(const QString&);
// Characters as a person counts them (grapheme clusters): an emoji with a skin
// tone, a flag or a family is one (#1748). What notifications, characterCount
// and the editor's copy report.
qsizetype characters(const QString&);
// The clipboard text of the action and of Copy all text: the text with each
// text code's content on a line of its own, before the first text line whose
// top edge is below the code's centre. Binary codes are left out.
QString copyText(const TextResult&);
// The notification of the Copy text action (behaviour 1.4): never recognized
// text, a host name or code content. copyStatus is copied, suppressed or failed.
QString notification(const TextResult&,const QString& copyStatus,const QStringList& missingLanguageNames={});
inline constexpr char busyNotification[]="Text recognition is still running";

// #1981: what the script check did. kept: the lines were read again in the
// detected language and that reading was kept; discarded: read again, kept
// the first reading; language_missing: that language's data is not installed;
// detector_missing: no osd data; unknown_script: no script, Latin, or one
// without a language; no_time: the time budget left no room for it;
// no_memory: the memory budget left no room for it.
QStringList detectionOutcomes();
// An OSD script name: 1 to 32 ASCII letters or underscores.
bool validScriptName(const QString& name);
// {"script","language","outcome"} of a result whose check ran; null otherwise.
// What the helper's result, text.recognize's record and editor.inspect carry.
QJsonValue detectionJson(const TextResult&);
// Decision 6. Identifiers match ^[a-z]{3,4}(_[a-z0-9]+){0,2}$; osd and equ are
// data files, not languages.
bool validLanguage(const QString& identifier);
bool validLanguages(const QStringList& identifiers); // 1 to 3, each valid, none twice
struct DataDirectory {QString path,kind;}; // kind: user, packaged or system
// The fixed directories in lookup order: the user's, the build's own (the
// AppImage's), then each distribution's.
QList<DataDirectory> dataDirectories(const QString& home,const QString& applicationDirectory);
struct Language {QString id,kind,path;qint64 bytes=0;};
// Names and sizes of *.traineddata in those directories; the first directory
// that holds a language wins. Loads no engine and starts no helper.
QList<Language> installedLanguages(const QList<DataDirectory>&);
// The Tesseract identifier for a locale name such as de_DE, or empty.
QString languageForLocale(const QString& locale);
// The languages of a job that names none: the saved preference, else the
// locale's language when installed, then English; installed ones only.
QStringList defaultLanguages(const QStringList& preference,const QString& locale,const QList<Language>& installed);
}
}
