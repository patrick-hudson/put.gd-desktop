#include <KService>
#include <KSycoca>
#include <QCoreApplication>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <cstdio>

// Read the session's actual application cache. Do not enumerate window titles,
// desktop contents, service Exec arguments, or any credential material.
int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    KSycoca::disableAutoRebuild();
    int count = 0;
    bool putgd = false, dolphin = false;
    for (const auto& service : KService::allServices()) {
        if (!service->isApplication()) continue;
        ++count;
        const auto name = service->desktopEntryName();
        putgd = putgd || name == QStringLiteral("putgd");
        dolphin = dolphin || name == QStringLiteral("org.kde.dolphin") || name == QStringLiteral("dolphin");
    }
    const auto json = QJsonDocument(QJsonObject{{"applicationCount", count}, {"putgd", putgd}, {"dolphin", dolphin},
        {"cachePath", KSycoca::absoluteFilePath()}, {"cacheExists", QFileInfo::exists(KSycoca::absoluteFilePath())}}).toJson(QJsonDocument::Compact);
    std::puts(json.constData());
}
