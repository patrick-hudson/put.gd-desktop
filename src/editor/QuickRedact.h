#pragma once
#include "TextLayer.h"
#include <QImage>
#include <QJsonArray>
#include <QList>
#include <QRect>
#include <QStringList>
#include <atomic>
#include <functional>

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
 QList<int> skippedPatterns; // indexes into Sources::patterns that did not finish (time, limits, not compiling)
 bool nameUnchecked=false; // `name` was chosen but the account lookup did not answer in time
 bool unfinished=false; // the run was cut at its deadline: categories may not all have been matched
};
// Decision 6, round 3 of #1631: user patterns. A pattern runs exactly as typed
// with ordinary PCRE2 search (QRegularExpression, Unicode properties, case as
// written) over subjects:
//  - each recognized line on its own: its words joined with one space;
//  - each pair of consecutive lines in reading order, joined with "\n", so
//    \s (or \n) matches across a wrapped line; a literal space does not. A
//    pair's hit counts only where it crosses the line break: one inside a line
//    is found in that line, once. Nothing spans three lines.
// Each subject is searched once, from its start, as global matching does; a
// hit marks every word it touches, and an empty hit marks nothing.
struct PatternHit {int subject=0;qsizetype start=0,length=0;bool operator==(const PatternHit&) const=default;};
// One pattern's result. `finished` false: it did not compile, or a search hit
// PCRE2's limits; it is then reported as skipped.
struct PatternOutcome {int pattern=0;bool finished=false;QList<PatternHit> hits;};
// Runs patterns over subjects and returns the outcome of each pattern that
// ended; one it never got to, or gave up on, is missing (and skipped). The
// editor's runs one in a child process it kills at the deadline
// (src/ui/QuickRedactRunner.h), since nothing bounds a regular expression's
// time in process.
// `lineBreaks` holds, for each subject, the offset of a pair's line break, or
// -1 for a single line.
using PatternMatcher=std::function<QList<PatternOutcome>(const QStringList& subjects,const QList<qsizetype>& lineBreaks,const QStringList& patterns)>;
// The matching itself, in this process, calling `done` after each pattern in
// order: what the child runs. Not bounded in time: the editor never calls it on
// its own threads.
// With `lineBreaks`, a pair subject's hits that do not cross its break are left
// out. After a hit, the next search starts on a whole character, even after \C
// matched half of one.
void matchPatterns(const QStringList& subjects,const QStringList& patterns,const std::function<void(PatternOutcome)>& done,const QList<qsizetype>& lineBreaks={});
// Every match of the chosen categories. A word belongs to one match at most;
// where kinds overlap the more specific one wins (key, email, card, ip, phone,
// then name and list).
QList<Match> find(const QList<RecognizedWord>& candidates,const QStringList& categories);
// The same with the name and list sources. Without a matcher, patterns run in
// this process with matchPatterns, `budgetMs` checked between patterns (tests);
// a budget of 0 or less skips every pattern. Blocking: call it off the UI
// thread (src/ui/QuickRedactRunner.h).
Result find(const QList<RecognizedWord>& candidates,const QStringList& categories,const Sources&,int budgetMs=2000);
// The same, calling `progress` (on the calling thread) after the name and list
// terms and after the patterns, with everything found so far, the patterns not
// yet finished counted as skipped. `cancel`, when set, is read between stages.
// `matcher`, when set, runs the patterns.
Result find(const QList<RecognizedWord>& candidates,const QStringList& categories,const Sources&,int budgetMs,const std::function<void(const Result&)>& progress,const std::atomic_bool* cancel=nullptr,const PatternMatcher& matcher={});
// The Redact marks for a match: its word boxes, one box per visual row, padded by a
// quarter of the line height above and below and by one and a half of the edge
// word's characters either side, clamped to `bounds` (the image). Towards another
// recognized word on the row the side margin ends at the far side of the blank
// space between them in `image` (the recognized picture), so that word stays
// readable; with no such space, or where `image` is transparent, it is kept whole,
// and without `image` it stops short of that word's box. A word joined to the
// match by ink between their boxes that neither holds, such as the dot of an
// address recognition dropped, is covered with the match. It never cuts into
// the match's own boxes.
QList<QRect> marks(const QList<RecognizedWord>& candidates,const Match&,const QRect& bounds,const QImage& image={});
// The same marks, each with the recognized words' own box it was padded from
// (what a deleted suggestion is remembered by). One per visual row of the
// match, in reading order.
struct MarkBox {QRect mark;QRect words;};
QList<MarkBox> markBoxes(const QList<RecognizedWord>& candidates,const Match&,const QRect& bounds,const QImage& image={});
// #1941: whether a decoded code's content holds a secret Quick redact's Keys and tokens covers, and of
// which kind: "otp" (an otpauth:// 2FA enrolment), "wifi" (a Wi-Fi network's password, WIFI:…;P:…;),
// "key" (anything the key rules find in it, read as lines of words), or empty. The content is read
// only to decide; nothing of it is kept or returned.
QString codeSecret(const QString& content);
// #1941: codes as text.inspect and editor.inspect return them (textjob::codesJson), with each one whose content
// holds a secret withheld: its "kind" becomes "withheld", "withheld" names the secret's kind (otp, wifi, key) and
// it has no "text". What a person shows or copies on screen is not this; agent replies are.
QJsonArray withholdSecretCodes(QJsonArray codes);
// #1941: what a copy an agent asked for carries in place of such a code's content: "[2FA QR code withheld]",
// "[Wi-Fi QR code withheld]" or "[QR code with a key withheld]", for the kinds codeSecret names.
QString withheldText(const QString& kind);
// Luhn's check over the digits of `number`.
bool luhn(const QString& digits);
}
