#include "WindowActivation.h"

#include <QElapsedTimer>
#include <QTimer>
#include <QWidget>
#include <memory>

namespace putgd {
namespace {
constexpr auto tokenVariable = "XDG_ACTIVATION_TOKEN";
}

void activateWindow(QWidget* window, const QString& token)
{
    if (!window) return;
    const bool withToken = validActivationToken(token);
    // Never pass on a stale token from the environment this process started with.
    qunsetenv(tokenVariable);
    if (withToken) qputenv(tokenVariable, token.toLatin1());
    if (window->isMinimized()) window->showNormal();
    else window->show();
    window->raise();
    // Qt 6.8+ already activates a newly shown window, using and clearing the
    // token; ask again only when it is still there, or when there is none.
    if (!withToken || qEnvironmentVariableIsSet(tokenVariable)) window->activateWindow();
    qunsetenv(tokenVariable);
}

void whenActivated(QWidget* window, int timeoutMs, std::function<void(bool)> done)
{
    if (!window) { done(false); return; }
    if (window->isActiveWindow() && !window->isMinimized()) { done(true); return; }
    // The window owns the timer, so a window destroyed first never answers.
    auto* timer = new QTimer(window);
    timer->setInterval(25);
    auto clock = std::make_shared<QElapsedTimer>();
    clock->start();
    QObject::connect(timer, &QTimer::timeout, timer, [timer, window, timeoutMs, clock, done] {
        const bool active = window->isActiveWindow() && !window->isMinimized();
        if (!active && clock->elapsed() < timeoutMs) return;
        timer->stop();
        timer->deleteLater();
        done(active);
    });
    timer->start();
}
}
