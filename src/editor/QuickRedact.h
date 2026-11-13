#pragma once
#include "TextLayer.h"
#include <QList>
#include <QRect>
#include <QStringList>

// Quick redact (#1428, decision 5): suggested Redact marks over recognized
// words of five kinds. It only suggests: recognition can miss text, so a page
// with no match can still hold some.
//
// Matching runs over every recognized word (the unfiltered candidates) on lines
// as they appear (words grouped by height and ordered left to right), after
// whitespace and Unicode digits are normalized. Emails, phone numbers, card
// numbers and addresses are matched across adjacent words and wrapped lines.
// Keys and tokens are matched per line on the words of the token alphabet
// joined without spaces, since recognition splits a long run into pieces: a
// known prefix at the start of a word, a JSON web token, a PEM block between
// its BEGIN and END lines, or a hex or base64 run of 32 characters or more
// with the shape of random data. Where a kind's shape fixes the alphabet the
// usual recognition confusions are allowed: O for 0, l and I for 1, S for 5
// and B for 8 inside numbers, O, l, I and S inside IPv6 groups, l, i, O, S
// and t inside hex, and a misread character or two inside a token.
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
// The Redact marks for a match: its word boxes, one box per line, padded by a
// quarter of the line height above and below and by one and a half characters
// either side, clamped to `bounds` (the image).
QList<QRect> marks(const QList<RecognizedWord>& candidates,const Match&,const QRect& bounds);
// Luhn's check over the digits of `number`.
bool luhn(const QString& digits);
}
