#include "SftpPanel.h"
#include "FileDialog.h"
#include "PreferenceRows.h"
#include "Ui.h"
#include "storage/sftp/SftpCommon.h"
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
namespace putgd {
using Ui::space;
using Ui::Space;
namespace {
const QString storedPlaceholder = QString::fromUtf8("•••••••• stored");
}
QString SftpPanel::provider() { return sftp::providerId; }
QJsonObject SftpPanel::defaultSettings() { return sftp::defaults(); }
SftpPanel::SftpPanel(QWidget* parent) : QWidget(parent) { setObjectName("sftpDestinationEditor"); }
void SftpPanel::build(const RowBuilder& row, const RowBuilder& signInRow) {
    if (built_) return;
    built_ = true;
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(space(Space::xxl));
    auto* server = PreferenceRows::card(page, "Server", "put.gd connects with your own OpenSSH client, so your ~/.ssh/config, keys and agent work as they do in a terminal.");
    server->parentWidget()->setObjectName("sftpServerSection");
    const auto line = [this](const char* name, const QString& placeholder) {
        auto* field = new QLineEdit(this);
        field->setObjectName(name);
        field->setPlaceholderText(placeholder);
        connect(field, &QLineEdit::editingFinished, this, [this, field] { finished(field); });
        return field;
    };
    server_ = line("sftpServer", "files.example.com or files.example.com:2222");
    Ui::setMono(server_);
    row(server, "Server", server_, "A host name, host:port or a Host alias from ~/.ssh/config. Pasting sftp://user@host:port/path or user@host:/path fills User and Folder too.");
    // A paste (or anything typed at once) is read as an address now; typing waits for the field to be left.
    connect(server_, &QLineEdit::textEdited, this, [this](const QString& text) {
        const bool pasted = text.size() - typedServer_.size() > 1;
        const auto previous = typedServer_;
        typedServer_ = text;
        if (pasted && applyAddress(text, &previous)) return;
        if (pinnedField_ == QLatin1String("server")) { pinnedField_.clear(); pinnedProblem_.clear(); }
        refreshProblems();
    });
    user_ = line("sftpUser", "From your SSH settings");
    row(server, "User", user_, "Leave empty to use the user ssh would: the one ~/.ssh/config names for this server, else your own.");
    connect(user_, &QLineEdit::textEdited, this, [this] { refreshProblems(); });
    folder_ = line("sftpFolder", "~/shots or /srv/www/shots");
    Ui::setMono(folder_);
    row(server, "Folder on server", folder_, "Where uploads go. A path that doesn't start with / starts in the login folder.");
    connect(folder_, &QLineEdit::textEdited, this, [this] { refreshProblems(); });
    // Any change to Folder or Server, typed, pasted or set by an agent, is another folder than the
    // one a test found missing.
    connect(folder_, &QLineEdit::textChanged, this, [this](const QString& text) { if (text != offeredFolderSetting_) hideCreateFolder(); });
    connect(server_, &QLineEdit::textChanged, this, [this](const QString& text) { if (text != offeredServerSetting_) hideCreateFolder(); });
    resolved_ = new QLabel(this);
    resolved_->setObjectName("sftpResolved");
    resolved_->setAccessibleName("Resolved connection");
    resolved_->setWordWrap(true);
    resolved_->setTextFormat(Qt::PlainText);
    resolved_->setProperty("uiStatus", true);
    Ui::role(resolved_, "secondary");
    server->addRow(resolved_);
    resolved_->hide();
    hostKey_ = new Ui::Banner(Ui::Banner::Tone::Warning, {}, this);
    hostKey_->setObjectName("sftpHostKeyBanner");
    hostKey_->setAction("Trust key");
    hostKey_->actionButton()->setObjectName("sftpTrustKey");
    server->addRow(hostKey_);
    hostKey_->hide();
    connect(hostKey_->actionButton(), &QPushButton::clicked, this, [this] { if (!offeredFingerprint_.isEmpty()) emit trustRequested(offeredFingerprint_); });
    createFolder_ = new Ui::Banner(Ui::Banner::Tone::Info, {}, this);
    createFolder_->setObjectName("sftpCreateFolderBanner");
    createFolder_->setAction("Create folder");
    createFolder_->actionButton()->setObjectName("sftpCreateFolder");
    server->addRow(createFolder_);
    createFolder_->hide();
    // Only for the folder the test found missing: an edited folder hides the offer.
    connect(createFolder_->actionButton(), &QPushButton::clicked, this, [this] {
        if (!offeredFolder_.isEmpty() && folder_->text() == offeredFolderSetting_) emit createFolderRequested(folder_->text(), offeredFolder_);
        else clearOffers();
    });

    auto* links = PreferenceRows::card(page, "Links");
    links->parentWidget()->setObjectName("sftpLinksSection");
    link_ = line("sftpPublicUrlBase", "https://files.example.com/shots/");
    Ui::setMono(link_);
    row(links, "Link address", link_, "Optional: the public HTTPS address Folder on server is served at. Without one, uploads still work, the clipboard stays as it is, and History offers Copy path.");
    // A user name or password in the address is left out as it arrives, typed or pasted.
    connect(link_, &QLineEdit::textEdited, this, [this](const QString& text) {
        bool hadUser = false;
        const auto clean = sftp::withoutUserInfo(text, &hadUser);
        if (hadUser) {
            const QSignalBlocker block(link_);
            link_->setText(clean);
            pinnedField_ = QStringLiteral("publicUrlBase");
            pinnedProblem_ = QStringLiteral("A Link address doesn't take a user name or password, so it was left out.");
        }
        refreshProblems();
    });

    // Sign-in (#1892): one section with SSH key, Password, Key passphrase and where secrets are
    // kept, in the card Settings keeps the destination's sign-in in.
    const auto addSignIn = [&](const QString& label, QWidget* control, const QString& hint) {
        auto* made = signInRow(nullptr, label, control, hint);
        signInRows_.append(made);
        return made;
    };
    // SSH key: a private key file ssh gets with -i, for a key that isn't in the agent or ~/.ssh/config.
    {
        auto* box = new QWidget(this);
        box->setObjectName("sftpKeyFileBox");
        auto* layout = new QHBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(space(Space::s));
        keyFile_ = line("sftpKeyFile", "Optional");
        keyFile_->setAccessibleName("SSH key");
        Ui::setMono(keyFile_);
        layout->addWidget(keyFile_, 1);
        chooseKey_ = new QPushButton("Choose…", box);
        chooseKey_->setObjectName("sftpChooseKey");
        chooseKey_->setAccessibleName("Choose the SSH key file");
        layout->addWidget(chooseKey_);
        connect(chooseKey_, &QPushButton::clicked, this, [this] { chooseKey(); });
        auto* made = addSignIn(QStringLiteral("SSH key"), box, QStringLiteral("A private key file for ssh to use, when it isn't in your agent or ~/.ssh/config."));
        keyError_ = made->findChild<QLabel*>("sftpKeyFileBoxError");
        connect(keyFile_, &QLineEdit::textEdited, this, [this] { keyFileProblem_.clear(); showKeyProblem(); });
        keyChooser_ = new FileDialog(this);
    }
    // Each secret field with a Clear beside it, shown while that secret is stored.
    const auto secret = [&](const char* name, const QString& label, const QString& key, QPushButton*& clear) {
        auto* box = new QWidget(this);
        box->setObjectName(QString::fromLatin1(name) + "Box");
        auto* layout = new QHBoxLayout(box);
        layout->setContentsMargins(0, 0, 0, 0);
        layout->setSpacing(space(Space::s));
        auto* field = line(name, "Optional");
        field->setEchoMode(QLineEdit::Password);
        field->setAccessibleName(label);
        layout->addWidget(field, 1);
        clear = new QPushButton("Clear", box);
        clear->setObjectName(QString::fromLatin1(name) + "Clear");
        clear->setAccessibleName("Clear the stored " + label.toLower());
        clear->setToolTip("Forget the stored " + label.toLower() + "; the other one stays.");
        clear->hide();
        layout->addWidget(clear);
        connect(clear, &QPushButton::clicked, this, [this, key] { emit clearRequested(key); });
        addSignIn(label, box, {});
        return field;
    };
    password_ = secret("sftpPassword", QStringLiteral("Password"), sftp::passwordKey(), clearPassword_);
    passphrase_ = secret("sftpPassphrase", QStringLiteral("Key passphrase"), sftp::passphraseKey(), clearPassphrase_);
    // The passphrase only ever answers a key's own passphrase prompt, never a password prompt.
    passphrase_->setToolTip(QStringLiteral("Answers only a key's passphrase prompt, never a password prompt."));
    for (auto* field : {password_, passphrase_}) connect(field, &QLineEdit::textChanged, this, [this] { if (!loading_) emit secretsEdited(); });
}
bool SftpPanel::chooseKey() {
    if (!keyChooser_ || keyChooser_->isOpen()) return false;
    // Where the key is now, else ~/.ssh, else home.
    auto folder = keyFile_->text().isEmpty() ? QString() : QFileInfo(keyFile_->text()).absolutePath();
    if (folder.isEmpty() || !QFileInfo(folder).isDir()) folder = QFileInfo(QDir::home().filePath(".ssh")).isDir() ? QDir::home().filePath(".ssh") : QDir::homePath();
    return keyChooser_->open({FileDialog::Mode::Open, "Choose the SSH key file", folder, {}, {}}, [this](const FileDialog::Result& result) {
        if (result.failed()) { keyFileProblem_ = result.error; showKeyProblem(); }
        if (!result.chosen()) return;
        keyFile_->setText(result.path);
        finished(keyFile_);
    });
}
void SftpPanel::takeKeyFile(const QString& raw) {
    // Exactly as chosen or typed: a file name may end in a space.
    auto path = raw;
    if (path == QLatin1String("~") || path.startsWith(QLatin1String("~/"))) path = QDir::homePath() + path.mid(1);
    // A .pub picked by mistake: the private key next to it, when it is there.
    QString privateKey;
    auto problem = path.isEmpty() || !sftp::problem({{"keyFile", path}}, "keyFile").isEmpty() ? Error{} : sftp::keyFileProblem(path, &privateKey);
    if (!privateKey.isEmpty()) {
        path = privateKey;
        problem = sftp::keyFileProblem(path);
    }
    if (keyFile_->text() != path) {
        const QSignalBlocker block(keyFile_);
        keyFile_->setText(path);
    }
    keyFileProblem_ = problem ? problem.message : QString();
}
void SftpPanel::setKeyProblem(const QString& message) {
    keyFileProblem_ = message;
    showKeyProblem();
}
void SftpPanel::showKeyProblem() {
    if (!keyError_) return;
    // The path's own rule first, then what is wrong with the file.
    auto message = sftp::problem(settings(), "keyFile");
    if (message.isEmpty() && !keyFile_->text().isEmpty()) message = keyFileProblem_;
    keyError_->setText(message);
    keyError_->setVisible(!message.isEmpty());
    keyFile_->setProperty("invalid", !message.isEmpty());
}
void SftpPanel::hideCreateFolder() {
    offeredFolder_.clear();
    if (createFolder_) createFolder_->hide();
}
bool SftpPanel::applyAddress(const QString& text, const QString* previous) {
    // A password in the address never stays anywhere: not in the field, the draft or what agents read.
    bool hadPassword = false;
    const auto trimmed = sftp::withoutPassword(text, &hadPassword);
    const bool address = hadPassword || trimmed.contains('@') || trimmed.contains(QLatin1String("://")) || trimmed.contains(QLatin1String(":/")) || trimmed.contains(QLatin1String(":~"));
    if (!address) return false;
    const QString passwordProblem = QStringLiteral("put.gd doesn't take a password in the address, so it was left out. Enter it in Password instead.");
    QString why;
    auto parsed = sftp::parseServer(trimmed, &why);
    // The whole address must be valid before any field takes a part of it.
    if (parsed) {
        auto candidate = settings();
        candidate["server"] = sftp::serverText(parsed->host, parsed->port);
        if (parsed->hasUser) candidate["user"] = parsed->user;
        if (parsed->hasFolder) candidate["folder"] = parsed->folder;
        for (const auto& key : {QStringLiteral("server"), QStringLiteral("user"), QStringLiteral("folder")})
            if (const auto problem = sftp::problem(candidate, key); !problem.isEmpty()) { why = problem; parsed.reset(); break; }
    }
    if (!parsed) {
        // Nothing changes: a paste puts back what was there; typed text keeps only its password out.
        const QSignalBlocker block(server_);
        server_->setText(previous ? *previous : trimmed);
        typedServer_ = server_->text();
        pinnedField_ = QStringLiteral("server");
        pinnedProblem_ = hadPassword && why.isEmpty() ? passwordProblem : why;
        showProblem("server", pinnedProblem_);
        return true;
    }
    {
        const QSignalBlocker a(server_), b(user_), c(folder_);
        server_->setText(sftp::serverText(parsed->host, parsed->port));
        if (parsed->hasUser) user_->setText(parsed->user);
        if (parsed->hasFolder) folder_->setText(parsed->folder);
    }
    // The fields changed under the blocker above: the same rule as any other change.
    if (folder_->text() != offeredFolderSetting_ || server_->text() != offeredServerSetting_) hideCreateFolder();
    typedServer_ = server_->text();
    if (hadPassword) { pinnedField_ = QStringLiteral("server"); pinnedProblem_ = passwordProblem; }
    refreshProblems();
    // Saved as one edit; the normalized address is no longer a paste, so this doesn't come back here.
    if (!loading_) finished(server_);
    return true;
}
void SftpPanel::finished(QLineEdit* field) {
    if (loading_) return;
    if (field == server_ && applyAddress(server_->text())) return;
    if (field == link_) {
        bool hadUser = false;
        const auto clean = sftp::withoutUserInfo(link_->text(), &hadUser);
        if (hadUser) {
            link_->setText(clean);
            pinnedField_ = QStringLiteral("publicUrlBase");
            pinnedProblem_ = QStringLiteral("A Link address doesn't take a user name or password, so it was left out.");
        }
    }
    if (field != server_ && field != link_) { pinnedField_.clear(); pinnedProblem_.clear(); }
    if (field == keyFile_) takeKeyFile(keyFile_->text());
    refreshProblems();
    if (field == password_ || field == passphrase_) return;
    // An invalid field isn't saved: its message stays beside it until it is fixed (as R2's fields).
    // A key file ssh can't use yet is saved: its message says how to fix the file.
    for (const auto& key : {QStringLiteral("server"), QStringLiteral("user"), QStringLiteral("folder"), QStringLiteral("publicUrlBase"), QStringLiteral("keyFile")})
        if (!sftp::problem(settings(), key).isEmpty()) return;
    // The missing folder a test found is only offered while Folder on server still names it.
    if (field == folder_ && folder_->text() != offeredFolderSetting_) { offeredFolder_.clear(); createFolder_->hide(); }
    // Another server is another host key: what was trusted for the old one no longer applies here.
    if (server_->text() != savedServer_) {
        hostKeyFingerprint_.clear();
        clearOffers();
    } else if (field == user_) clearOffers();
    savedServer_ = server_->text();
    emit edited();
    // The resolved line names the key ssh signs in with.
    if (field == server_ || field == user_ || field == keyFile_) emit addressChanged();
}
void SftpPanel::setSettings(const QJsonObject& s) {
    if (!built_) return;
    loading_ = true;
    server_->setText(s["server"].toString());
    user_->setText(s["user"].toString());
    folder_->setText(s["folder"].toString());
    link_->setText(s["publicUrlBase"].toString());
    keyFile_->setText(sftp::keyFile(s));
    keyFileNull_ = s.contains("keyFile") && s["keyFile"].isNull();
    hostKeyFingerprint_ = s["hostKeyFingerprint"].toString();
    typedServer_ = savedServer_ = server_->text();
    // What the saved key file needs fixed, read from the file as the form opens.
    const auto key = sftp::keyFile(s);
    const auto problem = key.isEmpty() || !sftp::problem(s, "keyFile").isEmpty() ? Error{} : sftp::keyFileProblem(key);
    keyFileProblem_ = problem ? problem.message : QString();
    loading_ = false;
    refreshProblems();
}
QJsonObject SftpPanel::settings() const {
    if (!built_) return defaultSettings();
    // Whatever is in the fields, a password or a link's user part never leaves this panel.
    const auto link = sftp::withoutUserInfo(link_->text());
    QJsonObject s{{"server", sftp::withoutPassword(server_->text())}, {"user", user_->text().trimmed()}, {"folder", folder_->text()},
                  {"publicUrlBase", link.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(link)},
                  {"hostKeyFingerprint", hostKeyFingerprint_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(hostKeyFingerprint_)}};
    // Saved only when one is chosen (#1892), so a destination without one reads as before; a
    // destination saved with an explicit null keeps it, or it would never read as saved.
    if (!keyFile_->text().isEmpty()) s["keyFile"] = keyFile_->text();
    else if (keyFileNull_) s["keyFile"] = QJsonValue(QJsonValue::Null);
    return s;
}
QMap<QString, QString> SftpPanel::typedSecrets() const {
    if (!built_) return {};
    QMap<QString, QString> typed;
    if (!password_->text().isEmpty()) typed.insert(sftp::passwordKey(), password_->text());
    if (!passphrase_->text().isEmpty()) typed.insert(sftp::passphraseKey(), passphrase_->text());
    return typed;
}
bool SftpPanel::hasTypedSecrets() const { return built_ && (!password_->text().isEmpty() || !passphrase_->text().isEmpty()); }
void SftpPanel::setTypedSecrets(const QMap<QString, QString>& values) {
    if (!built_) return;
    loading_ = true;
    password_->setText(values.value(sftp::passwordKey()));
    passphrase_->setText(values.value(sftp::passphraseKey()));
    loading_ = false;
}
void SftpPanel::clearTypedSecrets() {
    if (!built_) return;
    loading_ = true;
    password_->clear();
    passphrase_->clear();
    loading_ = false;
}
void SftpPanel::setStored(const QStringList& stored, bool known) {
    stored_ = stored;
    storedKnown_ = known;
    if (!built_) return;
    password_->setPlaceholderText(known && stored.contains(sftp::passwordKey()) ? storedPlaceholder : QStringLiteral("Optional"));
    passphrase_->setPlaceholderText(known && stored.contains(sftp::passphraseKey()) ? storedPlaceholder : QStringLiteral("Optional"));
    clearPassword_->setVisible(known && stored.contains(sftp::passwordKey()));
    clearPassphrase_->setVisible(known && stored.contains(sftp::passphraseKey()));
}
void SftpPanel::setResolved(const QString& text, bool problem) {
    if (!resolved_) return;
    if (problem) Ui::feedback(resolved_, text, true);
    else Ui::feedback(resolved_, text);
    resolved_->setVisible(!text.isEmpty());
}
void SftpPanel::clearOffers() {
    offeredFingerprint_.clear();
    offeredFolder_.clear();
    if (hostKey_) hostKey_->hide();
    if (createFolder_) createFolder_->hide();
}
void SftpPanel::showTestError(const QString& code, const QString& message, const QJsonObject& details) {
    if (!built_) return;
    clearOffers();
    const auto fingerprint = details["fingerprint"].toString();
    if ((code == QLatin1String("ssh_host_unknown_key") || code == QLatin1String("ssh_trusted_key_changed")) && sftp::fingerprintValid(fingerprint)) {
        offeredFingerprint_ = fingerprint;
        const bool changed = code == QLatin1String("ssh_trusted_key_changed");
        hostKey_->setTone(changed ? Ui::Banner::Tone::Error : Ui::Banner::Tone::Warning);
        // The fingerprint on a line of its own, so it reads whole and compares at a glance.
        const auto previous = details["previousFingerprint"].toString();
        hostKey_->setText(changed ? "This server's host key changed since you trusted it.\n" + (previous.isEmpty() ? QString() : "Was: " + previous + "\n") + "Now: " + fingerprint +
                                        "\nTrust the new key only if your server's admin confirms it was replaced."
                                  : "put.gd doesn't know this server's host key yet:\n" + fingerprint + "\nTrust it only if it matches the key your server's admin gave you.");
        hostKey_->setToolTip(message);
        hostKey_->setAction(changed ? "Trust new key" : "Trust key");
        hostKey_->actionButton()->setObjectName("sftpTrustKey");
        hostKey_->actionButton()->setAccessibleName((changed ? "Trust the new host key " : "Trust the host key ") + fingerprint);
        hostKey_->show();
        hostKey_->announce();
    } else if (code == QLatin1String("sftp_folder_missing")) {
        offeredFolder_ = details["path"].toString();
        offeredFolderSetting_ = folder_->text();
        offeredServerSetting_ = server_->text();
        createFolder_->setText(message);
        createFolder_->show();
    }
}
void SftpPanel::showProblem(const QString& field, const QString& message) {
    if (!built_) return;
    // SSH key sits in the sign-in section, with a message of its own.
    if (field == QLatin1String("keyFile")) { setKeyProblem(message); return; }
    const QHash<QString, QWidget*> fields{{"server", server_}, {"user", user_}, {"folder", folder_}, {"publicUrlBase", link_}};
    auto* target = fields.value(field, field.isEmpty() ? nullptr : server_);
    for (auto* control : fields) {
        auto* label = findChild<QLabel*>(control->objectName() + "Error");
        if (!label) continue;
        const bool here = control == target && !message.isEmpty();
        label->setText(here ? message : QString());
        label->setVisible(here);
        control->setProperty("invalid", here);
    }
}
void SftpPanel::refreshProblems() {
    const auto s = settings();
    QString field, message;
    for (const auto& key : {QStringLiteral("server"), QStringLiteral("user"), QStringLiteral("folder"), QStringLiteral("publicUrlBase")}) {
        const auto problem = sftp::problem(s, key);
        if (!problem.isEmpty()) { field = key; message = problem; break; }
    }
    if (field.isEmpty() && !pinnedProblem_.isEmpty()) { field = pinnedField_; message = pinnedProblem_; }
    showProblem(field.isEmpty() ? QString() : field, message);
    showKeyProblem();
}
QString SftpPanel::testLabel() const {
    return built_ && !link_->text().trimmed().isEmpty() ? QStringLiteral("Test upload and link") : QStringLiteral("Test upload");
}
QJsonObject SftpPanel::state() const {
    if (!built_) return {};
    auto s = settings();
    QJsonArray typed, stored;
    if (!password_->text().isEmpty()) typed.append(sftp::passwordKey());
    if (!passphrase_->text().isEmpty()) typed.append(sftp::passphraseKey());
    for (const auto& name : stored_) stored.append(name);
    QJsonObject problems;
    for (const auto& key : {QStringLiteral("server"), QStringLiteral("user"), QStringLiteral("folder"), QStringLiteral("publicUrlBase")}) {
        const auto* control = findChild<QLineEdit*>(key == "publicUrlBase" ? QStringLiteral("sftpPublicUrlBase") : "sftp" + key.left(1).toUpper() + key.mid(1));
        const auto* label = control ? findChild<QLabel*>(control->objectName() + "Error") : nullptr;
        if (label && label->isVisible() && !label->text().isEmpty()) problems[key] = label->text();
    }
    if (keyError_ && keyError_->isVisible() && !keyError_->text().isEmpty()) problems["keyFile"] = keyError_->text();
    if (!s.contains("keyFile")) s["keyFile"] = QJsonValue(QJsonValue::Null);
    s["resolved"] = resolved_->isVisible() ? resolved_->text() : QString();
    s["typedSecrets"] = typed;
    s["storedSecrets"] = storedKnown_ ? QJsonValue(stored) : QJsonValue(QJsonValue::Null);
    s["problems"] = problems;
    s["trustOffered"] = offeredFingerprint_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(QJsonObject{{"fingerprint", offeredFingerprint_}, {"message", hostKey_->textLabel()->text()}});
    s["createFolderOffered"] = offeredFolder_.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(offeredFolder_);
    s["testLabel"] = testLabel();
    return s;
}
}
