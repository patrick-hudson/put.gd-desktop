#include <QImageIOPlugin>

class GnomeImageMimePlugin final : public QImageIOPlugin {
 Q_OBJECT
 Q_PLUGIN_METADATA(IID "org.qt-project.Qt.QImageIOHandlerFactoryInterface" FILE "gnome-image-mime-plugin.json")
public:
 Capabilities capabilities(QIODevice*,const QByteArray& format)const override {
  return (format=="putgd-gnome-image"||format=="putgd-gnome-application") ? CanRead : Capabilities{};
 }
 QImageIOHandler* create(QIODevice*,const QByteArray&)const override {return nullptr;}
};
#include "GnomeImageMimePlugin.moc"
