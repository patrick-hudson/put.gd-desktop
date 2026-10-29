#pragma once
#include <QJsonObject>
#include <QDialog>

namespace putgd {
class AboutWindow final : public QDialog {
public:
 explicit AboutWindow(const QJsonObject& residentBuild, QWidget* parent = nullptr);
 QString detailedText()const{return details_;}
 int maximumFitHeight()const;
private:
 void fitHeight();
 QString details_;
};
}
