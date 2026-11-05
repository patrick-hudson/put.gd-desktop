// Prints the pipewiresrc properties the recorder sets, from the same
// pipewireSourceDescription() the recorder builds its pipeline with, so
// ci_check.sh links the AppImage's plugin with exactly those (#144).
#include "record/WaylandAcquisition.h"
#include <QStringList>
#include <cstdio>

int main() {
    const QString description = putgd::record::pipewireSourceDescription(QStringLiteral("pipewiresrc"), 1, -1, false);
    QStringList properties;
    // The element, its name and the node: run_buffers.sh supplies its own.
    for (const QString& token : description.split(QLatin1Char(' '), Qt::SkipEmptyParts).mid(1))
        if (!token.startsWith(QLatin1String("name=")) && !token.startsWith(QLatin1String("path="))) properties << token;
    std::printf("%s\n", qPrintable(properties.join(QLatin1Char(' '))));
    return properties.isEmpty() ? 1 : 0;
}
