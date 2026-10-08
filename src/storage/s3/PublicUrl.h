#pragma once
#include <QJsonObject>
#include <QString>
namespace putgd {
QString publicObjectUrl(const QJsonObject& settings, const QString& key);
bool s3VirtualAddressing(const QJsonObject& settings);
}
