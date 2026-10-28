#pragma once
#include <QString>
class QFormLayout;
class QVBoxLayout;
class QLabel;
class QRadioButton;
class QWidget;
namespace putgd::PreferenceRows {
// Preserve each field and its agent identity while composing card rows. Text
// fields stack unless preferenceInline is set; inline values share a measured
// width within their page family.
QLabel* caption(const QString&);
QFormLayout* card(QVBoxLayout*,const QString& heading,const QString& description={},bool headingInside=false);
QWidget* sectionFor(QFormLayout*);
QWidget* rowFor(QWidget*);
QWidget* labelFor(QFormLayout*,QWidget* row);
QWidget* action(QFormLayout*,const QString& heading,const QString& hint,QWidget* control);
QWidget* radio(QRadioButton*,const QString& explanation,const QString& detail);
// preferenceStacked keeps a picker full width; preferenceResponsive lets a
// choice move below its label when the measured family width no longer fits.
void box(QFormLayout*);
void alignChoices(QWidget* family);
}
