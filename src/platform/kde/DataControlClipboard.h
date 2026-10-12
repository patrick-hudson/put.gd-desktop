#pragma once
#include "app/OperationService.h"
#include <QObject>
#include <memory>
struct xkb_keymap;
namespace putgd {
class DataControlClipboard final:public QObject {
 Q_OBJECT
public:
 explicit DataControlClipboard(QObject* parent=nullptr);
 ~DataControlClipboard() override;
 xkb_keymap* keyboardMap()const; // Borrowed, main-thread only; invalidated by keyboardChanged.
 bool available()const;quint64 revision()const;QJsonObject status()const;
 void acquire(const ResourceLimits&,std::function<void(PayloadResult)>,quint64 expectedRevision);
 void copy(const CopyPayload&,std::function<void(Error)>);
 void cancel();
 void invalidateCopyEligibility();
signals:
 void changed();
 void keyboardChanged();
private:
 struct Impl;std::unique_ptr<Impl> d_;
};
}
