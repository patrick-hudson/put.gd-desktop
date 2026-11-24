#pragma once
#include "core/Settings.h"
#include "platform/wayland/AsyncFdReader.h"
#include <QObject>
#include <QImage>
#include <QVariantMap>
namespace putgd {
struct KWinImageLayout {int width=0,height=0,stride=0;QImage::Format format=QImage::Format_Invalid;qreal scale=1;qint64 bytes=0;};
KWinImageLayout validateKWinMetadata(const QVariantMap&,const ResourceLimits&,Error&);
QImage decodeKWinImage(const QVariantMap&,const QByteArray&,const ResourceLimits&,Error&);
}
