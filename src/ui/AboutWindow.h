#pragma once
#include <QJsonObject>
#include <QDialog>

namespace putgd {
class AboutWindow final : public QDialog {
public:
 explicit AboutWindow(const QJsonObject& residentBuild, QWidget* parent = nullptr);
 int maximumFitHeight()const;
protected:
 void showEvent(QShowEvent*)override;
 bool eventFilter(QObject*,QEvent*)override;
private:
 void fitHeight();
 QString details_;
};
}
