// Loads a built Peony plugin the way Peony's PluginManager does and checks it
// (#1105): metadata version and IID, instance(), the dynamic_casts Peony uses,
// and the action it returns per menu type. Built against the same distro Qt as
// the plugin. Not installed. Usage: putgd-peony-check <plugin.so> [--activate]
// --activate also triggers the folder-view action, which starts /usr/bin/putgd-launcher.
#include <menu-plugin-iface.h>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPluginLoader>
#include <cstdio>

static int fail(const QString &message) { std::fprintf(stderr, "FAIL: %s\n", qPrintable(message)); return 1; }

int main(int argc, char **argv) {
  const bool activate=argc == 3 && !qstrcmp(argv[2], "--activate");
  if(argc != 2 && !activate) return fail(QStringLiteral("usage: putgd-peony-check <plugin.so> [--activate]"));
  if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  QPluginLoader loader(QString::fromLocal8Bit(argv[1]));
  const QJsonObject metadata=loader.metaData();
  if(metadata.value(QStringLiteral("IID")).toString() != QStringLiteral(MenuPluginInterface_iid)) return fail(QStringLiteral("IID is ") + metadata.value(QStringLiteral("IID")).toString());
  if(metadata.value(QStringLiteral("MetaData")).toObject().value(QStringLiteral("version")).toString() != QStringLiteral("3.2.2")) return fail(QStringLiteral("metadata version is not 3.2.2"));
  QObject *instance=loader.instance();
  if(!instance) return fail(QStringLiteral("instance(): ") + loader.errorString());
  auto *plugin=dynamic_cast<Peony::PluginInterface *>(instance);
  if(!plugin || plugin->pluginType() != Peony::PluginInterface::MenuPlugin) return fail(QStringLiteral("not a Peony menu plugin"));
  auto *menu=dynamic_cast<Peony::MenuPluginInterface *>(plugin);
  if(!menu || qobject_cast<Peony::MenuPluginInterface *>(instance) != menu) return fail(QStringLiteral("dynamic_cast to MenuPluginInterface failed"));
  if(menu->name().isEmpty()) return fail(QStringLiteral("empty name"));
  const QStringList selection{QStringLiteral("file:///tmp/a%20b.txt"), QStringLiteral("smb://host/share/dir")};
  const auto offered=[&](Peony::MenuPluginInterface::Types types, const QStringList &uris) {
    const QList<QAction *> actions=menu->menuActions(types, QStringLiteral("file:///tmp"), uris);
    QString text=actions.size() == 1 ? actions.first()->text() : QString::number(actions.size()) + QStringLiteral(" actions");
    if(activate && types == Peony::MenuPluginInterface::DirectoryView && actions.size() == 1) actions.first()->trigger();
    qDeleteAll(actions);
    return text;
  };
  for(auto type : {Peony::MenuPluginInterface::DirectoryView, Peony::MenuPluginInterface::DesktopWindow})
    if(const QString text=offered(type, selection); text != QStringLiteral("Upload with put.gd")) return fail(QStringLiteral("menu type %1 offered %2").arg(type).arg(text));
  if(offered(Peony::MenuPluginInterface::SideBar, selection) != QStringLiteral("0 actions") || offered(Peony::MenuPluginInterface::DirectoryView, {}) != QStringLiteral("0 actions"))
    return fail(QStringLiteral("offered an action for the side bar or an empty selection"));
  QJsonObject result{{QStringLiteral("iid"), metadata.value(QStringLiteral("IID"))}, {QStringLiteral("version"), QStringLiteral("3.2.2")},
                     {QStringLiteral("name"), menu->name()}, {QStringLiteral("qt"), QStringLiteral(QT_VERSION_STR)},
                     {QStringLiteral("activated"), activate}};
  std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
  return 0;
}
