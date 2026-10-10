#pragma once
#include "Error.h"
#include "Settings.h"
#include <QByteArray>
#include <QImage>
#include <QList>
#include <QString>
#include <atomic>
#include <memory>

namespace putgd {
// Synchronous, bounded child work. Call from an acquisition/background worker,
// never the resident event loop. No GUI or codec discovery runs in this class.
class ImageJob final {
public:
 using Cancel=std::shared_ptr<std::atomic_bool>;
 explicit ImageJob(QString helperPath={},int timeoutMs=10000);
 QList<QByteArray> discover(Error&,Cancel={}) const;
 QImage decode(const QByteArray&,const ResourceLimits&,Error&,Cancel={}) const;
 QByteArray preview(const QString& file,qint64 maximumPixels,qint64 budget,Error&,Cancel={}) const;
private:
 QByteArray run(const QString& task,const QByteArray* input,const QString& file,
                qint64 maximumPixels,qint64 imageBytes,qint64 outputBytes,Error&,const Cancel&) const;
 QString helperPath_;
 int timeoutMs_;
};
}
