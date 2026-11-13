#pragma once
#include "EditDocument.h"
#include <QByteArray>
#include <QList>
#include <QPolygon>
#include <QRect>
#include <QString>

namespace putgd::editor {
// What the Copy text tool (#1428) holds over the canvas: the words and codes of
// one recognition result, in source pixels, and a selection in reading order.
// Nothing here reads pixels or starts a job; src/ui/EditorText runs the helper.
struct RecognizedWord {
 QString text;
 QRect rect;
 int confidence=0;
 int line=0; // which line of the result it is on, in reading order
};
struct RecognizedCode {
 QString format;
 bool binary=false; // its length only: never copied, shown or opened
 QString text;
 qint64 length=0;
 QPolygon quad;
};

class TextLayer {
public:
 // The recognition revision: what recognition reads changes only with a
 // Redact mark (added, moved, resized, removed) or the crop, by any route,
 // undo and redo included. Arrows, text and other marks leave it alone.
 static QByteArray inputSignature(const EditDocument&);
 // True when a box or a code touches a Redact mark (sharing an edge counts).
 // A result never keeps one: blacking the pixels is not enough, since a code
 // can still decode through its error correction.
 static bool touchesRedaction(const QRect&,const QList<QRect>& redactions);
 static bool touchesRedaction(const QPolygon&,const QList<QRect>& redactions);
 // Drops every word and code that touches a Redact mark and numbers the
 // words' lines; what is left is all a result may show, copy or return.
 static void exclude(QList<RecognizedWord>&,QList<RecognizedCode>&,const QList<QRect>& redactions);
 // Words in reading order joined as text: a space between words of a line,
 // a newline between lines. from and to are inclusive, in any order.
 static QString join(const QList<RecognizedWord>&,int from,int to);

 // A new result (identity > 0), already passed through exclude(); the
 // selection is cleared.
 void set(quint64 identity,QList<RecognizedWord> words,QList<RecognizedCode> codes);
 void clear();
 quint64 identity() const {return identity_;}
 const QList<RecognizedWord>& words() const {return words_;}
 const QList<RecognizedCode>& codes() const {return codes_;}
 // The word under a source pixel, or -1.
 int wordAt(const QPointF& image) const;
 // The selection, from anchor to focus in reading order; -1 for none.
 void select(int anchor,int focus);
 void extendTo(int focus); // Shift+click: keeps the anchor
 void selectLine(int index);
 void selectAll();
 void clearSelection();
 bool hasSelection() const {return anchor_>=0&&focus_>=0;}
 int first() const {return hasSelection()?qMin(anchor_,focus_):-1;}
 int last() const {return hasSelection()?qMax(anchor_,focus_):-1;}
 bool selected(int index) const {return hasSelection()&&index>=first()&&index<=last();}
 QString selectedText() const {return hasSelection()?join(words_,first(),last()):QString();}
private:
 quint64 identity_=0;
 QList<RecognizedWord> words_;
 QList<RecognizedCode> codes_;
 int anchor_=-1,focus_=-1;
};
}
