#pragma once
#include <QByteArray>
#include <QHash>
#include <QString>
namespace putgd {
// Starts the detached putgd-clipboard-keeper with these exact types and bytes.
// Wlroots compositors drop a selection when its owner disconnects, so without
// a clipboard manager a copied link would die with the resident. The content
// stays in memory; a result over the keeper's size limit is not kept.
// helperPath is for tests.
bool startClipboardKeeper(const QHash<QString,QByteArray>& data,QString helperPath={});
}
