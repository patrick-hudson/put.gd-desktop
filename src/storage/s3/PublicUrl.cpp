#include "PublicUrl.h"
#include <QUrl>
namespace putgd {
bool s3VirtualAddressing(const QJsonObject& settings) {
    const auto mode = settings["addressing"].toString();
    if (mode == "virtual") return true;
    if (mode == "path") return false;
    const auto host = QUrl(settings["endpoint"].toString()).host();
    return host.endsWith("amazonaws.com") && !settings["bucket"].toString().contains('.');
}
QString publicObjectUrl(const QJsonObject& settings, const QString& key) {
    QUrl base(settings["publicUrlBase"].isString() ? settings["publicUrlBase"].toString()
                                                    : settings["endpoint"].toString());
    QString encoded = base.toString(QUrl::FullyEncoded);
    while (encoded.endsWith('/')) encoded.chop(1);
    if (!settings["publicUrlBase"].isString()) {
        if (s3VirtualAddressing(settings)) {
            base.setHost(settings["bucket"].toString() + "." + base.host());
            encoded = base.toString(QUrl::FullyEncoded);
            while (encoded.endsWith('/')) encoded.chop(1);
        } else {
            encoded += "/" + QString::fromLatin1(QUrl::toPercentEncoding(settings["bucket"].toString()));
        }
    }
    QStringList segments;
    for (const auto& segment : key.split('/'))
        segments.append(QString::fromLatin1(QUrl::toPercentEncoding(segment)));
    return encoded + "/" + segments.join('/');
}
}
