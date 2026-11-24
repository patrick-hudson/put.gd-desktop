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
// #1941: `found` with the QR codes Keys and tokens covered (`codes`, by kind: otp, wifi, key) named as
// codes of their own, "a 2FA QR code", "a Wi-Fi QR code", "a QR code with a key", and taken out of the
// key count. Only kinds and counts: never a code's content.
QList<QPair<QString,int>> withCodes(QList<QPair<QString,int>> found,const QJsonObject& codes);
// #1820: the notification's line for an upload the check covered, "Covered 1 card number."; empty for none.
QString coveredSentence(const QJsonObject& covered);
}
