#pragma once
#include <QJsonObject>
#include <QList>
#include <QPair>
#include <QString>

// #1804, #1820: how the sensitive text check names what it found or covered, by kind and count, never the text.
// Shared by the prompt (the editor helper) and the upload notification (the resident's tray).
namespace putgd {
// "a card number and 2 keys or tokens"; with `counted`, "1 card number and 2 keys or tokens".
QString describeSensitive(const QList<QPair<QString,int>>& found,bool counted=false);
// #1820: the notification's line for an upload the check covered, "Covered 1 card number."; empty for none.
QString coveredSentence(const QJsonObject& covered);
}
