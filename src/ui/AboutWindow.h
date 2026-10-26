#pragma once
#include <QJsonObject>
#include <QMessageBox>

namespace putgd {
class AboutWindow final : public QMessageBox {
public:
 explicit AboutWindow(const QJsonObject& residentBuild, QWidget* parent = nullptr);
protected:
 void showEvent(QShowEvent*)override;
};
}
