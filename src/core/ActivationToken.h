#pragma once
#include <QString>

namespace putgd {
// An xdg-activation token from a tray host, launcher or the resident (#657):
// compositors issue short printable ASCII strings, so anything else is refused.
inline bool validActivationToken(const QString& token)
{
    if (token.isEmpty() || token.size() > 512) return false;
    for (const QChar c : token)
        if (c.unicode() < 0x21 || c.unicode() > 0x7e) return false;
    return true;
}
}
