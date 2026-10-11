#pragma once
#include <QDBusConnection>
#include <QObject>
#include <memory>

namespace putgd {
class NativeTrayPrivate;
// StatusNotifier and dbusmenu transport; no windowing application is required.
// Rendering to a QImage is software-only. Native desktop acceptance is separate
// from tests of these wire methods.
class NativeTray final : public QObject {
 Q_OBJECT
public:
 explicit NativeTray(QObject* parent = nullptr);
 NativeTray(const QDBusConnection& bus, QObject* parent = nullptr);
 ~NativeTray() override;
 bool available() const;
 bool availabilityKnown() const;
 void notify(const QString& title, const QString& message, bool warning = false);
signals:
 void actionActivated(const QString& action);
 void availabilityChanged(bool available);
 void availabilityResolved();
private:
 friend class NativeTrayPrivate;
 std::unique_ptr<NativeTrayPrivate> d_;
};
}
