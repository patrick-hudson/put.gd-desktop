#pragma once
#include <QHash>
#include <QJsonObject>
#include <QUrl>
#include <QWidget>
#include <functional>
class QCheckBox;class QFormLayout;class QLabel;class QLineEdit;class QPushButton;
namespace putgd {
// Settings → Destinations → Nextcloud or WebDAV (#1872). Nextcloud: the server, the
// account (Sign in through the browser, or an app password in the Credentials card
// below), and the folder. WebDAV: the folder address and an optional link address; its
// user and password are in the Credentials card. Settings builds the rows with its own
// row style; this class owns the fields and the sign-in display, never a credential.
class DavDestinationPanel final : public QWidget {
    Q_OBJECT
public:
    using RowBuilder = std::function<QWidget*(QFormLayout*, const QString&, QWidget*, const QString&)>;
    explicit DavDestinationPanel(QWidget* parent = nullptr);
    void build(const RowBuilder&);
    void setProvider(const QString&);
    QString provider() const { return provider_; }
    void setSettings(const QJsonObject&);
    QJsonObject settings() const;
    // Sign-in display. Waiting: a Login Flow v2 job runs; `url` is its browser page once known.
    enum class SignIn { Idle, Starting, Waiting, BrowserFailed };
    void setSignIn(SignIn, const QUrl& url = {});
    SignIn signIn() const { return signIn_; }
    QUrl signInUrl() const { return signInUrl_; }
    // Whether the app password fields are asked for (Nextcloud's fallback).
    bool appPasswordShown() const { return appPassword_; }
    void setAppPasswordShown(bool);
    // While setup runs, only Cancel and Try opening browser again stay usable.
    void setBusy(bool);
    // The last Test said the WebDAV folder is missing: offer Create folder.
    void setFolderMissing(bool);
    void setNotice(const QString& text, bool success = false);
    // A refused save: the message under the field it names (server, folder, address, linkBase), or
    // cleared with an empty message; an empty field means every field.
    void showProblem(const QString& field, const QString& message);
    QJsonObject problems() const;
    // Fields, sign-in state and offers, never a credential; for agents.
    QJsonObject state() const;
signals:
    // A setting changed: its key in the destination's settings, saved on its own.
    void edited(const QString& field);
    void signInRequested();
    void cancelSignInRequested();
    void browserRequested();
    void signOutRequested();
    void createFolderRequested();
    void appPasswordToggled(bool shown);
private:
    void finished(const QString& field);
    void addProblem(QFormLayout*, const QString& field);
    QHash<QString, QLabel*> problems_;
    QHash<QString, QFormLayout*> forms_;
    void sync();
    bool loading_ = false, built_ = false, appPassword_ = false, busy_ = false, folderMissing_ = false;
    QString provider_ = QStringLiteral("nextcloud");
    QString accountName_;
    SignIn signIn_ = SignIn::Idle;
    QUrl signInUrl_;
    QFormLayout *nextcloudForm_ = nullptr, *folderForm_ = nullptr, *webdavForm_ = nullptr;
    QLineEdit *server_ = nullptr, *folder_ = nullptr, *address_ = nullptr, *linkBase_ = nullptr;
    QCheckBox *nextcloudInsecure_ = nullptr, *webdavInsecure_ = nullptr;
    QWidget *nextcloudInsecureRow_ = nullptr, *webdavInsecureRow_ = nullptr, *createFolderRow_ = nullptr;
    QLabel *account_ = nullptr, *notice_ = nullptr;
    QPushButton *signInButton_ = nullptr, *cancel_ = nullptr, *retry_ = nullptr, *signOut_ = nullptr, *appPasswordButton_ = nullptr, *createFolder_ = nullptr;
};
}
