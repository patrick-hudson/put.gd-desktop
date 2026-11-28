// "Upload encrypted with put.gd" in Dolphin and Konqueror on KF6 (#2136), as a
// KFileItemAction plugin: KF6 service menus have no condition that could hide it.
// KIO asks actions() at every context menu, and the action is left out while the
// resident's marker exists (encrypted-entry.h), one access() per menu. Upload with
// put.gd stays in the KIO service menu, which this build installs without its
// encrypted action. KIO offers plugins only for a selection with one common type
// (all files or all folders), so a mixed selection shows no encrypted entry.
// The action's name, icon and Exec come from that service menu's
// putgdUploadEncrypted action (kio-entry.h, generated at configure time); KIO's
// ApplicationLauncherJob runs the Exec as it runs service menus, so %F gets
// KIOFuse or kioexec paths for remote items. Linked to the distro's KF6 KIO and
// Qt like any Dolphin plugin; the packages still depend on no KDE library
// (packaging/linux), so where KIO is missing the file is simply unused.
#include "encrypted-entry.h"
#include "kio-entry.h"
#include <KAbstractFileItemActionPlugin>
#include <KFileItem>
#include <KFileItemListProperties>
#include <KIO/ApplicationLauncherJob>
#include <KIO/JobUiDelegateFactory>
#include <KPluginFactory>
#include <KService>
#include <QAction>
#include <QIcon>
#include <QPointer>
#include <QWidget>
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

class PutgdFileItemAction : public KAbstractFileItemActionPlugin {
public:
  PutgdFileItemAction(QObject *parent, const QVariantList &) : KAbstractFileItemActionPlugin(parent) {}
  QList<QAction *> actions(const KFileItemListProperties &items, QWidget *parentWidget) override {
    if(items.items().isEmpty() || encryptedHidden()) return {};
    auto *action=new QAction(QIcon::fromTheme(QStringLiteral(PUTGD_KIO_ENCRYPTED_ICON)), QStringLiteral(PUTGD_KIO_ENCRYPTED_NAME), parentWidget);
    const QList<QUrl> urls=items.urlList();
    const QPointer<QWidget> window=parentWidget;
    connect(action, &QAction::triggered, this, [urls, window] {
      const KService::Ptr service(new KService(QStringLiteral(PUTGD_KIO_ENCRYPTED_NAME), QStringLiteral(PUTGD_KIO_ENCRYPTED_EXEC), QStringLiteral(PUTGD_KIO_ENCRYPTED_ICON)));
      auto *job=new KIO::ApplicationLauncherJob(service);
      job->setUrls(urls);
      job->setUiDelegate(KIO::createDefaultJobUiDelegate(KJobUiDelegate::AutoHandlingEnabled, window));
      job->start();
    });
    return {action};
  }
};

}

K_PLUGIN_CLASS_WITH_JSON(PutgdFileItemAction, "putgd-kio.json")

#include "kio-plugin.moc"
