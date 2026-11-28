// Loads a built Peony plugin the way Peony's PluginManager does and checks it
// (#1105): metadata version and IID, instance(), the dynamic_casts Peony uses,
// and the two actions it returns per menu type (#2119). Built against the same
// distro Qt as the plugin. Not installed.
// Usage: putgd-peony-check <plugin.so> [--activate|--activate-encrypted]
// --activate also triggers the folder view's Upload with put.gd action,
// --activate-encrypted its Upload encrypted with put.gd; either starts /usr/bin/putgd-launcher.
#include "encrypted-entry.h"
#include <menu-plugin-iface.h>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QApplication>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPluginLoader>
#include <cstdio>

static int fail(const QString &message) { std::fprintf(stderr, "FAIL: %s\n", qPrintable(message)); return 1; }

int main(int argc, char **argv) {
  const int activate=argc == 3 ? (!qstrcmp(argv[2], "--activate") ? 1 : !qstrcmp(argv[2], "--activate-encrypted") ? 2 : -1) : 0;
  if(argc != 2 && activate <= 0) return fail(QStringLiteral("usage: putgd-peony-check <plugin.so> [--activate|--activate-encrypted]"));
  if(qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  // #2136: the plugin's marker under a private data home, created and removed below.
  QTemporaryDir dataHome;
  if(!dataHome.isValid()) return fail(QStringLiteral("no temporary folder"));
  qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome.path()));
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
    QStringList texts;for(auto *action : actions) texts << action->text();
    const QString text=actions.isEmpty() ? QStringLiteral("0 actions") : texts.join(QStringLiteral(" | "));
    if(activate > 0 && types == Peony::MenuPluginInterface::DirectoryView && actions.size() == 2) actions[activate - 1]->trigger();
    qDeleteAll(actions);
    return text;
  };
  for(auto type : {Peony::MenuPluginInterface::DirectoryView, Peony::MenuPluginInterface::DesktopWindow})
    if(const QString text=offered(type, selection); text != QStringLiteral("Upload with put.gd | Upload encrypted with put.gd")) return fail(QStringLiteral("menu type %1 offered %2").arg(type).arg(text));
  if(offered(Peony::MenuPluginInterface::SideBar, selection) != QStringLiteral("0 actions") || offered(Peony::MenuPluginInterface::DirectoryView, {}) != QStringLiteral("0 actions"))
    return fail(QStringLiteral("offered an action for the side bar or an empty selection"));
  // With the resident's marker present, only Upload with put.gd is offered.
  const QString marker=dataHome.filePath(QStringLiteral(PUTGD_FM_ENCRYPTED_HIDDEN));
  QFile file(marker);
  if(!QDir().mkpath(QFileInfo(marker).path()) || !file.open(QIODevice::WriteOnly) || file.write("proton\n") != 7) return fail(QStringLiteral("cannot write ") + marker);
  file.close();
  if(const QString text=offered(Peony::MenuPluginInterface::DirectoryView, selection); text != QStringLiteral("Upload with put.gd")) return fail(QStringLiteral("with the marker offered ") + text);
  if(!QFile::remove(marker)) return fail(QStringLiteral("cannot remove ") + marker);
  QJsonObject result{{QStringLiteral("iid"), metadata.value(QStringLiteral("IID"))}, {QStringLiteral("version"), QStringLiteral("3.2.2")},
                     {QStringLiteral("name"), menu->name()}, {QStringLiteral("qt"), QStringLiteral(QT_VERSION_STR)},
                     {QStringLiteral("hidesEncrypted"), true}, {QStringLiteral("activated"), activate == 1 ? QJsonValue(QStringLiteral("plain")) : activate == 2 ? QJsonValue(QStringLiteral("encrypted")) : QJsonValue()}};
  std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
  return 0;
}
