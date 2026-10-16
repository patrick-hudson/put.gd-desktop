#pragma once
#include <QDBusConnection>
#include <QObject>
#include <QColor>
#include <QUrl>
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
 void setActivity(const QColor& color, const QString& description, int percent = -1);
 void notify(const QString& title, const QString& message, bool warning = false, const QUrl& link = {});
signals:
 void notificationActivated(const QUrl& link);
 void actionActivated(const QString& action);
 void availabilityChanged(bool available);
 void availabilityResolved();
private:
 friend class NativeTrayPrivate;
 std::unique_ptr<NativeTrayPrivate> d_;
};
}
