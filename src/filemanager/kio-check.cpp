// Loads a built KIO plugin the way KFileItemActions does and checks it (#2136):
// its metadata (MimeTypes, so KIO offers it for files and folders), the
// instantiated KAbstractFileItemActionPlugin, its one action for a selection,
// and no action while the resident's marker exists (a private XDG_DATA_HOME).
// Not installed. Usage: putgd-kio-check <putgd.so> [--activate]
// --activate also triggers the action, which starts /usr/bin/putgd-launcher.
#include "encrypted-entry.h"
#include "kio-entry.h"
#include <KAbstractFileItemActionPlugin>
#include <KFileItem>
#include <KFileItemListProperties>
#include <KPluginFactory>
#include <KPluginMetaData>
#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QTimer>
#include <QWidget>
#include <cstdio>

static int fail(const QString &message) { std::fprintf(stderr, "FAIL: %s\n", qPrintable(message)); return 1; }

static bool write(const QString &path, const QByteArray &bytes) {
  QFile file(path);
  return QDir().mkpath(QFileInfo(path).path()) && file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

int main(int argc, char **argv) {
  const bool activate=argc == 3 && !qstrcmp(argv[2], "--activate");
  if(argc != 2 && !activate) return fail(QStringLiteral("usage: putgd-kio-check <putgd.so> [--activate]"));
  QTemporaryDir dataHome;
  if(!dataHome.isValid()) return fail(QStringLiteral("no temporary folder"));
  qputenv("XDG_DATA_HOME", QFile::encodeName(dataHome.path()));
  // Offscreen for the package audit; --activate runs on the desktop, so the launcher it starts does too.
  if(!activate && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM")) qputenv("QT_QPA_PLATFORM", "offscreen");
  QApplication app(argc, argv);
  const KPluginMetaData metadata(QFileInfo(QString::fromLocal8Bit(argv[1])).absoluteFilePath());
  if(!metadata.isValid()) return fail(QStringLiteral("no plugin metadata in ") + QString::fromLocal8Bit(argv[1]));
  if(metadata.mimeTypes() != QStringList{QStringLiteral("application/octet-stream"), QStringLiteral("inode/directory")})
    return fail(QStringLiteral("MimeTypes are ") + metadata.mimeTypes().join(QLatin1Char(',')));
  const auto loaded=KPluginFactory::instantiatePlugin<KAbstractFileItemActionPlugin>(metadata);
  if(!loaded) return fail(QStringLiteral("instantiatePlugin: ") + loaded.errorString);
  // Two local files, as Dolphin passes a selection.
  const QString first=dataHome.filePath(QStringLiteral("selection/a b.txt")), second=dataHome.filePath(QStringLiteral("selection/c.png"));
  if(!write(first, "a") || !write(second, "c")) return fail(QStringLiteral("cannot write the selection"));
  const KFileItemListProperties selection(KFileItemList{KFileItem(QUrl::fromLocalFile(first)), KFileItem(QUrl::fromLocalFile(second))});
  QWidget window;
  const QList<QAction *> actions=loaded.plugin->actions(selection, &window);
  if(actions.size() != 1 || actions.first()->text() != QStringLiteral(PUTGD_KIO_ENCRYPTED_NAME))
    return fail(QStringLiteral("expected one %1 action, got %2").arg(QStringLiteral(PUTGD_KIO_ENCRYPTED_NAME)).arg(actions.size()));
  if(!loaded.plugin->actions(KFileItemListProperties(KFileItemList{}), &window).isEmpty()) return fail(QStringLiteral("an empty selection offered an action"));
  // With the resident's marker present, no action.
  const QString marker=dataHome.filePath(QStringLiteral(PUTGD_FM_ENCRYPTED_HIDDEN));
  if(!write(marker, "proton\n")) return fail(QStringLiteral("cannot write ") + marker);
  if(const auto hidden=loaded.plugin->actions(selection, &window); !hidden.isEmpty()) return fail(QStringLiteral("offered %1 actions with the marker").arg(hidden.size()));
  if(!QFile::remove(marker)) return fail(QStringLiteral("cannot remove ") + marker);
  if(activate) {
    actions.first()->trigger();
    // The launcher job runs on this event loop; KIO starts the process and returns.
    QTimer::singleShot(3000, &app, &QCoreApplication::quit);
    app.exec();
  }
  const QJsonObject result{{QStringLiteral("id"), metadata.pluginId()}, {QStringLiteral("name"), metadata.name()},
                           {QStringLiteral("mimeTypes"), QJsonArray::fromStringList(metadata.mimeTypes())},
                           {QStringLiteral("actions"), QJsonArray{actions.first()->text()}}, {QStringLiteral("exec"), QStringLiteral(PUTGD_KIO_ENCRYPTED_EXEC)},
                           {QStringLiteral("hidesEncrypted"), true}, {QStringLiteral("activated"), activate}, {QStringLiteral("qt"), QStringLiteral(QT_VERSION_STR)}};
  std::printf("%s\n", QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
  return 0;
}
