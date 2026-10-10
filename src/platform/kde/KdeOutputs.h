#pragma once
#include "platform/Geometry.h"
#include <QObject>
#include <QJsonObject>
#include <memory>

namespace putgd {
// An independent, Core-compatible Wayland connection. Logical rectangles and
// names come from xdg-output; the KWin screenshot response owns pixel density.
class KdeOutputs final:public QObject {
 Q_OBJECT
public:
 struct Metadata {
  QString name;
  QRect logical;
  QSize mode;
  int transform=0;
  int scale=1;
 };
 explicit KdeOutputs(QObject* parent=nullptr);
 ~KdeOutputs() override;
 QList<CaptureFrame> layout() const;
 quint64 revision() const;
 QJsonObject status() const;
 // Shared normalization of completed native metadata, also used by contract
 // tests. Partial protocol batches never reach this function in production.
 static QList<CaptureFrame> framesFromMetadata(const QList<Metadata>&,QString* error=nullptr);
signals:
 void changed();
private:
 struct Impl;
 std::unique_ptr<Impl> d_;
};
}
