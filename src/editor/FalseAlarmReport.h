#pragma once
#include "QuickRedact.h"
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QRect>
#include <QSize>
#include <QStringList>

// #2134: the part of a false-alarm report the sensitive text check's helper writes: what recognition read
// and what the check matched, with the recognized text of each match, its words, boxes, confidences and the
// rule that fired. The resident adds the capture, settings, build and environment (src/app/FalseAlarmReports.h).
// A deliberate exception to "diagnostics hold no pixels or recognized text" (docs/design/diagnostics.md):
// it holds the screen's text, never a saved Quick redact word or pattern (a list match names its rule by
// kind and number only), the account name looked up for name, or a decoded code's content.
namespace putgd::editor::falseAlarm {
// The Quick redact run the check made, as the editor keeps it.
struct Run {
 QList<RecognizedWord> candidates;
 QList<quickRedact::Match> matches;
 QList<QList<QRect>> marks; // by match, the marks it placed
 QList<QPair<QString,QRect>> codes; // QR codes covered with Keys and tokens: kind (otp, wifi, key) and box
 QJsonObject recognition; // how the result was read (EditorTextRecognition::details), no text
 QStringList categories;
 bool valid=false;
};
struct Input {
 Run run;
 QString reportedFrom; // prompt or editor
 QSize image; // the checked image's size in pixels
 QJsonObject findings; // the prompt's {found, complete, reason?}
 QJsonObject covered; // what the check covered by itself, by kind
 QJsonObject codeKinds; // #1941: covered QR codes by kind
 QStringList categories,cover; // the kinds checked, and those covered without asking
};
// The most words a report lists. Past it, or past maximumBytes, only the words on a match's lines and the
// two lines either side are kept, and `words.omitted` counts the rest.
inline constexpr int maximumWords=50000;
inline constexpr qint64 maximumBytes=8LL*1024*1024;
// {reportedFrom, image, check, recognition, findings, words, codes}; see docs/design/diagnostics.md.
QJsonObject build(const Input&);
}
