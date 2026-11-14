#pragma once
#include <QRect>
#include <QString>
#include <QWidget>

class QLabel;
class QPushButton;

namespace putgd::record {
// The scroll capture's bar (#1515): what the capture is doing in words, the
// height of the image so far, a line that says what to do next, and Auto-scroll
// (a toggle: put.gd turns the page, or you do), Done (keeps the image, from the
// first frame on) and Cancel. It carries the recording bar's window title so the
// GNOME Shell extension moves it the same way. It goes beside the rectangle on
// its own screen where it can (BarPlacement), else inside the rectangle's
// bottom-right corner, where its rows are kept out of the image (#1519). It
// never takes the keyboard. One size for every state, so its buttons never move.
class ScrollBar final : public QWidget {
    Q_OBJECT
public:
    explicit ScrollBar(QWidget* parent = nullptr);
    // Finds a spot outside `captured` (X11 root pixels, or logical desktop
    // pixels on Wayland) and moves there; false when there is none.
    bool place(const QRect& captured);
    // Inside the rectangle's bottom-right corner; false when it does not fit.
    bool placeInside(const QRect& captured);
    bool outside() const { return outside_; }
    // ready | waiting | following | lost | scrollingToTop | autoScrolling | paused | resuming | finishing
    void setState(const QString& state);
    // paused, and why: "pointer" (the user moved the mouse), "scrolled" (the
    // user scrolled), "no_movement" (nothing scrolled under the wheel).
    void setPaused(const QString& by);
    QString state() const { return state_; }
    void setHeight(int rows);
    // Auto-scroll's toggle, as the session has it.
    void setAutoScroll(bool on);
    // Done keeps something: the first frame is in.
    void setPicture(bool picture);
    // The key that finishes the capture here ("Esc", a hotkey), named in the hint; empty: none.
    void setFinishKey(const QString& key);
    // The height as the bar writes it: "12,340 px".
    static QString heightText(int rows);
    // The words the bar shows for a state: {label, hint}.
    static QStringList words(const QString& state, const QString& pausedBy, const QString& finishKey);
signals:
    void autoScrollRequested(bool on);
    void doneRequested();
    void cancelRequested();
protected:
    void paintEvent(QPaintEvent*) override;
private:
    void refresh();
    void fitSize();
    QString state_ = QStringLiteral("ready"), pausedBy_, finishKey_;
    int rows_ = 0;
    bool outside_ = false, auto_ = false, picture_ = false;
    QLabel *dot_ = nullptr, *label_ = nullptr, *height_ = nullptr, *hint_ = nullptr;
    QPushButton *autoScroll_ = nullptr, *done_ = nullptr, *cancel_ = nullptr;
};
}
