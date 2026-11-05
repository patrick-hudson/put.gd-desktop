#pragma once
#include <QLabel>

namespace putgd::test {
// Qt versions/styles differ in whether QMessageBox text takes keyboard focus.
// Keep that distinction while always inventorying its question/consequences.
inline QString catalogLabelKind(const QLabel& label){
 if((label.textInteractionFlags()&Qt::LinksAccessibleByMouse)&&label.text().contains("<a "))return "link";
 if(!label.objectName().isEmpty()&&label.focusPolicy()!=Qt::NoFocus)return "focusable";
 if(label.objectName()=="qt_msgbox_label"||label.objectName()=="qt_msgbox_informativelabel")return "label";
 return {};
}
}
