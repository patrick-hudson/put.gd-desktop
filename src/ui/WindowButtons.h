#pragma once
#include <QFileSystemWatcher>
#include <QObject>
#include <QProcess>
#include <QStringList>

namespace putgd {
struct WindowButtonLayout {
    QStringList left;
    QStringList right{QStringLiteral("minimize"), QStringLiteral("maximize"), QStringLiteral("close")};
    bool operator==(const WindowButtonLayout&) const = default;
};

// One cached, asynchronous desktop preference source for all custom headers.
class WindowButtons final : public QObject {
    Q_OBJECT
public:
    static WindowButtons& instance();
    explicit WindowButtons(const QString& desktop, const QString& configDirectory, QObject* parent = nullptr);
    ~WindowButtons() override;
    const WindowButtonLayout& layout() const { return layout_; }
    static WindowButtonLayout fromGSettings(QString value);
    static WindowButtonLayout fromKWin(const QString& path);
signals:
    void changed();
private:
    void setLayout(const WindowButtonLayout&);
    void readKWin();
    void monitorSettings();
    WindowButtonLayout layout_;
    QString kwinPath_, schema_;
    QFileSystemWatcher watcher_;
    QProcess query_, monitor_;
    QByteArray monitorOutput_;
    bool monitorChanged_ = false;
};
}
