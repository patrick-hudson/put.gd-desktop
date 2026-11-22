#pragma once
#include <QJsonObject>
#include <QMap>
#include <QWidget>
#include <functional>
class QFormLayout;class QLabel;class QLineEdit;class QPushButton;
namespace putgd {
class FileDialog;
namespace Ui { class Banner; }
// Settings → Destinations → SFTP (#1871): Server, User, Folder on server, Link address,
// and the optional SSH key (#1892), Password and Key passphrase. A pasted sftp:// or user@host:/path
// address fills User and Folder too. The line under Folder says what ssh resolves the
// address to; a test that met an unknown or changed host key offers Trust key, and one
// that met a missing folder offers Create folder. Settings stores the fields; typed
// secrets stay in this window until Settings stores them.
class SftpPanel final : public QWidget {
    Q_OBJECT
public:
    using RowBuilder = std::function<QWidget*(QFormLayout*, const QString&, QWidget*, const QString&)>;
    static QString provider();
    static QJsonObject defaultSettings();
    explicit SftpPanel(QWidget* parent = nullptr);
    // `signIn` adds the SSH key, Password and Key passphrase rows where Settings keeps the
    // destination's sign-in (#1892); without one they get a card of their own here.
    void build(const RowBuilder&, const RowBuilder& signIn = {});
    // The rows `signIn` made, so Settings shows them only for an SFTP destination.
    const QList<QWidget*>& signInRows() const { return signInRows_; }
    // Choose… beside SSH key: the desktop's file chooser, starting in ~/.ssh (ui.fileDialog sftpKey).
    FileDialog& keyChooser() const { return *keyChooser_; }
    bool chooseKey();
    void setSettings(const QJsonObject&);
    QJsonObject settings() const;
    // Fills Server, User and Folder from an address, as a paste does; false when it isn't one.
    // An address that isn't valid as a whole changes no field: Server goes back to `previous`
    // (the text before the paste; null: as typed, with any password left out) and says why.
    bool applyAddress(const QString& text, const QString* previous = nullptr);
    // Typed values by credential-store name: only the ones typed; the others stay as stored.
    QMap<QString, QString> typedSecrets() const;
    bool hasTypedSecrets() const;
    void setTypedSecrets(const QMap<QString, QString>&);
    void clearTypedSecrets();
    // From setup.credentialHint: which of the two are stored, or unknown while locked.
    void setStored(const QStringList& stored, bool known);
    // What ssh -G resolved, or why it couldn't; empty hides the line.
    void setResolved(const QString& line, bool problem = false);
    // A test's outcome: an unknown or changed host key (with its fingerprint), a missing folder.
    void showTestError(const QString& code, const QString& message, const QJsonObject& details);
    void clearOffers();
    // The fingerprint Trust key would trust, or empty when none is offered.
    QString offeredFingerprint() const { return offeredFingerprint_; }
    // A refused save or test: the message beside the field it names. keyFile's has its own place.
    void showProblem(const QString& field, const QString& message);
    // What a chosen SSH key file needs fixed (#1892), from the file itself or from ssh -G's check.
    void setKeyProblem(const QString& message);
    // The test button's words: Test upload, or Test upload and link with a Link address.
    QString testLabel() const;
    QJsonObject state() const;
signals:
    void edited();
    void secretsEdited();
    void trustRequested(const QString& fingerprint);
    // `path` is the folder the test reported missing, as the server resolves it.
    void createFolderRequested(const QString& folder, const QString& path);
    // Clear next to a stored password or passphrase: forget that one (an empty value stored).
    void clearRequested(const QString& name);
    // Server or User changed and saved: what ssh resolves may have changed.
    void addressChanged();
private:
    void refreshProblems();
    // An edited Folder or Server is another folder than the one a test found missing.
    void hideCreateFolder();
    void finished(QLineEdit*);
    // SSH key as typed or chosen: ~/ expanded, a .pub swapped for the private key next to it.
    void takeKeyFile(const QString& path);
    void showKeyProblem();
    bool loading_ = false, built_ = false;
    QLineEdit *server_ = nullptr, *user_ = nullptr, *folder_ = nullptr, *link_ = nullptr, *password_ = nullptr, *passphrase_ = nullptr, *keyFile_ = nullptr;
    QPushButton *clearPassword_ = nullptr, *clearPassphrase_ = nullptr, *chooseKey_ = nullptr;
    QLabel* keyError_ = nullptr;
    FileDialog* keyChooser_ = nullptr;
    QList<QWidget*> signInRows_;
    // The chosen key's own problem (the file, or what ssh found), and the last pure rule's.
    QString keyFileProblem_;
    // The Folder on server a test found missing: Create folder makes only that one.
    QString offeredFolderSetting_, offeredServerSetting_;
    // A problem the panel found itself (a password left out of a paste), kept past refreshProblems.
    QString pinnedField_, pinnedProblem_;
    QLabel* resolved_ = nullptr;
    Ui::Banner *hostKey_ = nullptr, *createFolder_ = nullptr;
    QString offeredFingerprint_, offeredFolder_, hostKeyFingerprint_;
    QStringList stored_;
    bool storedKnown_ = false;
    // typedServer_: the text before the last edit, to tell a paste; savedServer_: the server last saved.
    QString typedServer_, savedServer_;
};
}
