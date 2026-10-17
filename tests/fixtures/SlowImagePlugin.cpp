#include <QImageIOPlugin>
#include <QThread>

// Loading this plugin takes three seconds. On the plugin path it holds image
// type discovery inside the transient image job, so a test can cancel while
// that child is running.
namespace {
struct Hold {Hold(){QThread::msleep(3000);}} hold;
}
class SlowImagePlugin final : public QImageIOPlugin {
 Q_OBJECT
 Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "slow-image-plugin.json")
public:
 Capabilities capabilities(QIODevice*,const QByteArray&)const override {return {};}
 QImageIOHandler* create(QIODevice*,const QByteArray&)const override {return nullptr;}
};
#include "SlowImagePlugin.moc"
