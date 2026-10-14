#pragma once
#include <QColor>
#include <QIcon>
#include <QList>
#include <QString>
class QHeaderView;class QComboBox;class QLabel;class QApplication;class QWidget;class QFrame;class QFormLayout;class QAbstractItemView;class QToolButton;
namespace putgd::VisualStyle {
void apply(QApplication&);
void configureColumns(QHeaderView* header);
QComboBox* choiceBox(QWidget* parent);
QIcon icon(const QString& name,const QColor& color=QColor("#9bbcff"));
QWidget* header(const QString& title,const QString& subtitle,QWidget* parent);
QFrame* card(const QString& title,const QString& subtitle,QWidget* parent);
void styleForm(QFormLayout*);
void decorateView(QAbstractItemView*,const QList<int>& statusColumns);
void role(QWidget*,const char*);
void feedback(QLabel*,const QString&,bool error=false);
void success(QLabel*,const QString&);
QToolButton* helpButton(const QString& title,const QString& description,QWidget* control,QWidget* parent);
QWidget* settingLabel(const QString& title,const QString& description,QWidget* control,QWidget* parent);
QWidget* settingRow(const QString& title,const QString& description,QWidget* control,QWidget* parent);
}
