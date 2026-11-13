#pragma once
#include "TextLayer.h"
#include <QList>
#include <QRect>
#include <QStringList>

// Quick redact (#1428, decision 5): suggested Redact marks over recognized
// words of five kinds. It only suggests: recognition can miss text, so a page
// with no match can still hold some.
//
// Matching runs over every recognized word (the unfiltered candidates) in
// reading order, joined across adjacent words and wrapped lines, after
// whitespace and Unicode digits are normalized. Where a kind's shape fixes the
// alphabet the usual recognition confusions are allowed: O for 0, l and I for
// 1, S for 5 and B for 8 inside numbers, and O, l, I and S inside IPv6 groups.
namespace putgd::editor::quickRedact {
// email, phone, ip, card, key: the categories of editor.apply's redactText.
QStringList categories();
// Keys and tokens, as the tool's help lists them.
QStringList keyShapes();
struct Match {
 QString category;
 QList<int> words; // indexes into the candidates, in reading order
 bool operator==(const Match&) const=default;
};
// Every match of the chosen categories. A word belongs to one match at most;
// where kinds overlap the more specific one wins (key, email, card, ip, phone).
QList<Match> find(const QList<RecognizedWord>& candidates,const QStringList& categories);
// The Redact marks for a match: its word boxes padded by a quarter of their
// line height, one box per line, clamped to `bounds` (the image).
QList<QRect> marks(const QList<RecognizedWord>& candidates,const Match&,const QRect& bounds);
// Luhn's check over the digits of `number`.
bool luhn(const QString& digits);
}
