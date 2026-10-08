#pragma once
#include <QColor>
#include <QIcon>
#include <QList>
#include <QString>
class QLabel;class QApplication;class QWidget;class QFrame;class QFormLayout;class QAbstractItemView;
namespace putgd::VisualStyle {
void apply(QApplication&);
QIcon icon(const QString& name,const QColor& color=QColor("#a9f1cb"));
QWidget* header(const QString& title,const QString& subtitle,QWidget* parent);
QFrame* card(const QString& title,const QString& subtitle,QWidget* parent);
void styleForm(QFormLayout*);
void decorateView(QAbstractItemView*,const QList<int>& statusColumns);
void role(QWidget*,const char*);
void feedback(QLabel*,const QString&,bool error=false);
}
