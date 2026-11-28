#pragma once
// The wheel as a desktop sends it, over every closed value control under a window (#26, #2117).
// The event is spontaneous and goes through QApplication::notify to the widget under the
// pointer, so one a control ignores travels on to the pane under it; sendEvent would clear the
// marker and stop it at the first widget.
#include <QAbstractScrollArea>
#include <QAbstractSlider>
#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QScrollBar>
#include <QStringList>
#include <QTabBar>
#include <QWheelEvent>
#include <QtTest/qtestspontaneevent.h>

namespace putgd::test {
inline void wheelOver(QWidget* control,int delta){
 const QPoint centre=control->rect().center();QWidget* under=control->childAt(centre);if(!under)under=control;
 const QPointF at=under->mapFrom(control,centre);
 QWheelEvent event(at,under->mapToGlobal(at),{},QPoint(0,delta),Qt::NoButton,Qt::NoModifier,Qt::NoScrollPhase,false);
 QSpontaneKeyEvent::setSpontaneous(&event);QApplication::instance()->notify(under,&event);
}
inline QString controlValue(QWidget* w){
 if(auto* c=qobject_cast<QComboBox*>(w))return QString::number(c->currentIndex())+":"+c->currentText();
 if(auto* s=qobject_cast<QAbstractSpinBox*>(w))return s->text();
 if(auto* s=qobject_cast<QAbstractSlider*>(w))return QString::number(s->value());
 if(auto* t=qobject_cast<QTabBar*>(w))return QString::number(t->currentIndex());
 return {};
}
struct WheelSweep{QStringList changed,checked,scrolled;};
// Every dropdown, spin box, slider and tab bar under root, shown or not, gets one wheel step
// each way. A shown one in a pane that can scroll must scroll that pane.
inline WheelSweep sweepClosedControls(QWidget* root){
 WheelSweep sweep;
 for(auto* control:root->findChildren<QWidget*>()){
  if(controlValue(control).isNull()||qobject_cast<QScrollBar*>(control))continue;
  auto name=control->objectName();if(name.isEmpty())name=control->accessibleName();if(name.isEmpty())name=control->metaObject()->className();
  QScrollBar* bar=nullptr;
  if(control->isVisible())for(auto* p=control->parentWidget();p&&!bar;p=p->parentWidget())if(auto* area=qobject_cast<QAbstractScrollArea*>(p);area&&area->verticalScrollBar()->maximum()>0)bar=area->verticalScrollBar();
  if(bar)bar->setValue(bar->minimum());
  const auto before=controlValue(control);
  wheelOver(control,-120);
  if(bar&&bar->value()>bar->minimum())sweep.scrolled.append(name);
  wheelOver(control,120);
  if(controlValue(control)!=before)sweep.changed.append(name+" ("+before+" -> "+controlValue(control)+")");
  sweep.checked.append(name);
 }
 return sweep;
}
}
