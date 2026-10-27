#pragma once
#include "Theme.h"
#include <QColor>
#include <QIcon>
#include <QList>
#include <QString>
#include <functional>
class QHeaderView;class QComboBox;class QLabel;class QApplication;class QWidget;class QFrame;class QFormLayout;class QAbstractItemView;class QToolButton;
namespace putgd::VisualStyle {
void apply(QApplication&);
// #237: every widget carries the active scheme in its themeScheme property,
// and stylesheets hold both schemes' rules, each scoped to that mark, so a
// switch never replaces a stylesheet: Qt re-polishes every widget below one
// that changes, which took over a second with History and Settings open. A
// switch re-polishes only the visible widgets (after the hooks, which set
// palettes); a hidden one catches up just before it is shown.
QString schemeScoped(const std::function<QString(Theme::Scheme)>& render);
void followScheme(QApplication&,std::function<void()> beforeRepolish={});
void configureColumns(QHeaderView* header);
QComboBox* choiceBox(QWidget* parent);
// A glyph in the active scheme's icon colours. QIcon users repaint it in the
// new colours after a scheme switch (#60); a pixmap taken from it does not.
QIcon icon(const QString& name);
// A glyph in a fixed colour: tray tints, glyphs drawn on the accent.
QIcon icon(const QString& name,const QColor& color,int percent=-1,int recording=0);
// Folder of the check, radio, chevron and spin step images painted from the
// scheme's tokens (1x and @2x) in a private temporary folder for this process,
// for stylesheets' image urls; empty when it cannot be written.
QString indicators(Theme::Scheme);
QWidget* header(const QString& title,const QString& subtitle,QWidget* parent);
QFrame* card(const QString& title,const QString& subtitle,QWidget* parent);
void styleForm(QFormLayout*);
void alignFormValue(QLabel* caption,QLabel* value);
void decorateView(QAbstractItemView*,const QList<int>& statusColumns);
void role(QWidget*,const char*);
void feedback(QLabel*,const QString&,bool error=false);
void success(QLabel*,const QString&);
QToolButton* helpButton(const QString& title,const QString& description,QWidget* control,QWidget* parent);
// The same help dialog without a button (a menu entry opens it).
void showHelp(const QString& title,const QString& description,QWidget* parent);
QWidget* settingLabel(const QString& title,const QString& description,QWidget* control,QWidget* parent);
QWidget* settingRow(const QString& title,const QString& description,QWidget* control,QWidget* parent);
}
