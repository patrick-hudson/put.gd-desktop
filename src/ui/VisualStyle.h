#pragma once
#include "Theme.h"
#include <QColor>
#include <QIcon>
#include <QList>
#include <QString>
class QHeaderView;class QComboBox;class QLabel;class QApplication;class QWidget;class QFrame;class QFormLayout;class QAbstractItemView;class QToolButton;
namespace putgd::VisualStyle {
void apply(QApplication&);
void configureColumns(QHeaderView* header);
QComboBox* choiceBox(QWidget* parent);
// A glyph in the active scheme's icon colours. QIcon users repaint it in the
// new colours after a scheme switch (#60); a pixmap taken from it does not.
QIcon icon(const QString& name);
// A glyph in a fixed colour: tray tints, glyphs drawn on the accent.
QIcon icon(const QString& name,const QColor& color,int percent=-1);
// Folder of the check, radio and chevron indicator images painted from the
// scheme's tokens (1x and @2x) in a private temporary folder for this process,
// for stylesheets' image urls; empty when it cannot be written.
QString indicators(Theme::Scheme);
QWidget* header(const QString& title,const QString& subtitle,QWidget* parent);
QFrame* card(const QString& title,const QString& subtitle,QWidget* parent);
void styleForm(QFormLayout*);
void decorateView(QAbstractItemView*,const QList<int>& statusColumns);
void role(QWidget*,const char*);
void feedback(QLabel*,const QString&,bool error=false);
void success(QLabel*,const QString&);
QToolButton* helpButton(const QString& title,const QString& description,QWidget* control,QWidget* parent);
QWidget* settingLabel(const QString& title,const QString& description,QWidget* control,QWidget* parent);
QWidget* settingRow(const QString& title,const QString& description,QWidget* control,QWidget* parent);
}
