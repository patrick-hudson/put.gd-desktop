// "Upload with put.gd" and "Upload encrypted with put.gd" (#2119) in Peony's
// folder views and desktop (#1105). Peony loads this plugin into its own
// process, so it only builds the two actions and, when one is chosen, starts
// the launcher with the selection's URIs, unchanged. Built
// against the libpeony interface headers (inline only) and the distro Qt's
// headers without linking either: Qt resolves from Peony, and no libpeony
// symbol is used. QT_NO_VERSION_TAGGING drops the reference a linked QtCore would satisfy.
// #2136: the encrypted action is left out while the resident's marker exists, one
// access() per menu (encrypted-entry.h).
#include "encrypted-entry.h"
#include <menu-plugin-iface.h>
#include <QAction>
#include <QIcon>
#include <QObject>
#include <QProcess>
#include <cstdlib>
#include <string>
#include <unistd.h>

namespace {

// The marker under $XDG_DATA_HOME when it is absolute, else ~/.local/share (encrypted-entry.h).
bool encryptedHidden() {
  const char *data=std::getenv("XDG_DATA_HOME"), *home=std::getenv("HOME");
  const std::string base=data && data[0] == '/' ? std::string(data) : std::string(home ? home : "") + "/.local/share";
  return access((base + "/" PUTGD_FM_ENCRYPTED_HIDDEN).c_str(), F_OK) == 0;
}

class PutgdPeonyMenu : public QObject, public Peony::MenuPluginInterface {
  Q_OBJECT
  // Peony skips a plugin whose metadata version is not its fixed VERSION 3.2.2.
  Q_PLUGIN_METADATA(IID MenuPluginInterface_iid FILE "putgd-peony.json")
  Q_INTERFACES(Peony::MenuPluginInterface)
public:
  PluginType pluginType() override { return MenuPlugin; }
  // Peony keys plugins by name and drops a duplicate.
  const QString name() override { return QStringLiteral("put.gd Upload Menu Extension"); }
  const QString description() override { return QStringLiteral("Upload the selected items with put.gd"); }
  const QIcon icon() override { return QIcon::fromTheme(QStringLiteral("putgd")); }
  void setEnable(bool enable) override { enabled_=enable; }
  bool isEnable() override { return enabled_; }
  QString testPlugin() override { return name(); }
  // Peony re-parents the returned actions to its menu.
  QList<QAction *> menuActions(Types types, const QString &, const QStringList &selection) override {
    if(selection.isEmpty() || !(types.testFlag(DirectoryView) || types.testFlag(DesktopWindow))) return {};
    const auto action=[this, &selection](const QString& label, const QStringList& options) {
      auto *item=new QAction(icon(), label, nullptr);
      QObject::connect(item, &QAction::triggered, item, [selection, options] {
        QProcess::startDetached(QStringLiteral("/usr/bin/putgd-launcher"), options + QStringList{QStringLiteral("--")} + selection);
      });
      return item;
    };
    QList<QAction *> actions{action(QStringLiteral("Upload with put.gd"), {QStringLiteral("--upload")})};
    if(!encryptedHidden()) actions << action(QStringLiteral("Upload encrypted with put.gd"), {QStringLiteral("--upload"), QStringLiteral("--encrypt")});
    return actions;
  }
private:
  bool enabled_=true;
};

}

#include "peony-menu.moc"
