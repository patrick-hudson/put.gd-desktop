#pragma once
class QFormLayout;
class QWidget;
namespace putgd::PreferenceRows {
// Preserve each field and its agent identity while composing card rows. Text
// fields stack; choices share a measured width within their page family.
void box(QFormLayout*);
void alignChoices(QWidget* family);
}
