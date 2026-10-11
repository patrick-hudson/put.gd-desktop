#pragma once
#include <QApplication>
#include <QCloseEvent>
#include <QHideEvent>
#include <QShowEvent>
#include <QTimer>
#include <QWidget>
#include <QWindow>

namespace putgd {
// Keep the logical view and its draft/selection, releasing only hidden native resources.
class RetainedWindow:public QWidget {
public:
 using QWidget::QWidget;
protected:
 void showEvent(QShowEvent* event) override {
  closed_=false;
  ++visibilityGeneration_;
  // A recreated QWidgetWindow starts with no native state; keep Qt and the compositor aligned.
  if(auto* native=windowHandle())native->setWindowStates(windowState()&~Qt::WindowActive);
  QWidget::showEvent(event);
 }
 void closeEvent(QCloseEvent* event) override {
  QWidget::closeEvent(event);
  if(event->isAccepted()) {
   closed_=true;
   scheduleNativeRelease(); // An already-hidden minimized window receives no new hide event.
  }
 }
 void hideEvent(QHideEvent* event) override {
  QWidget::hideEvent(event);
  if(event->spontaneous())++visibilityGeneration_;
  else scheduleNativeRelease();
 }
private:
 void scheduleNativeRelease() {
  const auto generation=++visibilityGeneration_;
  // Run after hide/close dispatch; the context cancels this work on widget destruction.
  QTimer::singleShot(0,this,[this,generation] {
   if(generation!=visibilityGeneration_||!isWindow()||!isHidden()||isVisible()||(isMinimized()&&!closed_))return;
   // QWindow::close may already clear winId while leaving the QWidget backing store.
   if(!internalWinId()&&!backingStore())return;
   if(QApplication::activeModalWidget()||QGuiApplication::modalWindow())return;
   for(auto* child:findChildren<QWidget*>())
    if(child->isWindow()&&child->isVisible())return;
   if(auto* native=windowHandle()) {
    for(auto* other:QGuiApplication::allWindows()) {
     if(!other->isVisible())continue;
     for(auto* parent=other->transientParent();parent;parent=parent->transientParent())
      if(parent==native)return;
    }
   }
   destroy(true,true);
  });
 }
 quint64 visibilityGeneration_=0;
 bool closed_=false;
};
}
