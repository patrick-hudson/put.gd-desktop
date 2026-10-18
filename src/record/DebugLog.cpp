#include "DebugLog.h"
#include <QDateTime>
#include <QFile>
#include <QProcessEnvironment>
namespace putgd::record {
void debugLog(const QString& line) {
    static const QString path = QProcessEnvironment::systemEnvironment().value("PUTGD_RECORD_DEBUG_LOG");
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::Append | QIODevice::Text)) return;
    file.write((QDateTime::currentDateTime().toString(Qt::ISODateWithMs) + " " + line + "\n").toUtf8());
}
}
