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
// email, phone, ip, card, key, name, list: the categories of editor.apply's
// redactText (#1618 adds name and list).
QStringList categories();
// Keys and tokens, as the tool's help lists them.
QStringList keyShapes();
struct Match {
 QString category;
 QList<int> words; // indexes into the candidates, in reading order
 bool operator==(const Match&) const=default;
};
// #1618: what the name and list categories match. The editor fills `login` and
// `realName` from the account (src/ui/AccountName.h), `terms` and `patterns`
// from Settings (preferences.quickRedactTerms / quickRedactPatterns) or from an
// agent's redactText terms/patterns. Nothing here is ever logged or stored.
struct Sources {
 QString login;
 QString realName; // the GECOS full name, before the first comma
 QStringList terms; // literal words or phrases
 QStringList patterns; // regular expressions, already validated
};
struct Result {
 QList<Match> matches;
 QList<int> skippedPatterns; // indexes into Sources::patterns that ran out of budget
};
// Every match of the chosen categories. A word belongs to one match at most;
// where kinds overlap the more specific one wins (key, email, card, ip, phone).
QList<Match> find(const QList<RecognizedWord>& candidates,const QStringList& categories);
// The same with the name and list sources. Patterns share `budgetMs` (2000 in
// the editor); a pattern that runs out of it is skipped and named in the
// result, and the matches found so far are kept. Blocking: call it off the UI
// thread (src/ui/QuickRedactRunner.h).
Result find(const QList<RecognizedWord>& candidates,const QStringList& categories,const Sources&,int budgetMs=2000);
// The Redact marks for a match: its word boxes, one box per line, padded by a
// quarter of the line height above and below and by one and a half characters
// either side, clamped to `bounds` (the image).
QList<QRect> marks(const QList<RecognizedWord>& candidates,const Match&,const QRect& bounds);
// Luhn's check over the digits of `number`.
bool luhn(const QString& digits);
}
