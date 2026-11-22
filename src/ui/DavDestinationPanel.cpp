#include "DavDestinationPanel.h"
#include "PreferenceRows.h"
#include "Ui.h"
#include "storage/dav/DavCommon.h"
#include <QCheckBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QVBoxLayout>
namespace putgd {
using Ui::space;
using Ui::Space;
DavDestinationPanel::DavDestinationPanel(QWidget* parent) : QWidget(parent) { setObjectName("davDestinationEditor"); }
void DavDestinationPanel::finished(const QString& field) {
    // An edit answers the field's earlier refusal; a new one comes back with the save.
    if (problems_.contains(field)) showProblem(field, {});
    sync();
    if (!loading_) emit edited(field);
}
void DavDestinationPanel::build(const RowBuilder& row) {
    if (built_) return;
    built_ = true;
    auto* page = new QVBoxLayout(this);
    page->setContentsMargins(0, 0, 0, 0);
    page->setSpacing(space(Space::xxl));
    const auto line = [this](const char* name, const QString& placeholder) {
        auto* field = new QLineEdit(this);
        field->setObjectName(name);
        field->setPlaceholderText(placeholder);
        Ui::setMono(field);
        return field;
    };
    const auto insecure = [this](const char* name) {
        auto* box = new QCheckBox("Allow plain HTTP for this destination", this);
        box->setObjectName(name);
        connect(box, &QCheckBox::clicked, this, [this] { finished("allowInsecureHttp"); });
        return box;
    };
    const QString plainHint = QStringLiteral("Plain HTTP sends files and the password unencrypted. Use it only on a network you trust.");

    // Nextcloud: the server and the account.
    nextcloudForm_ = PreferenceRows::card(page, "Nextcloud", "Each upload gets a public share link: anyone with the link can view the file.");
    nextcloudForm_->parentWidget()->setObjectName("nextcloudServerSection");
    server_ = line("nextcloudServer", "https://cloud.example.com");
    row(nextcloudForm_, "Server", server_, "The address you open Nextcloud at. A pasted Files or share link works too.");
    addProblem(nextcloudForm_, "server");
    connect(server_, &QLineEdit::editingFinished, this, [this] {
        // A pasted Files or share link becomes the install's base address.
        const auto base = dav::nextcloudServer(server_->text());
        if (!base.isEmpty() && base != server_->text()) { const QSignalBlocker block(server_); server_->setText(base); }
        finished("server");
    });
    connect(server_, &QLineEdit::textChanged, this, [this] { sync(); });
    nextcloudInsecure_ = insecure("nextcloudAllowHttp");
    nextcloudInsecureRow_ = row(nextcloudForm_, "Plain HTTP", nextcloudInsecure_, plainHint);

    auto* accountBox = new QWidget(this);
    accountBox->setObjectName("nextcloudAccount");
    auto* accountColumn = new QVBoxLayout(accountBox);
    accountColumn->setContentsMargins(0, 0, 0, 0);
    accountColumn->setSpacing(space(Space::s));
    account_ = new QLabel("Signed out", accountBox);
    account_->setObjectName("nextcloudAccountStatus");
    account_->setWordWrap(true);
    account_->setAccessibleName("Nextcloud account");
    account_->setProperty("uiStatus", true);
    Ui::role(account_, "secondary");
    accountColumn->addWidget(account_);
    auto* buttons = new QHBoxLayout;
    buttons->setSpacing(space(Space::s));
    signInButton_ = new QPushButton(QString::fromUtf8("Sign in…"), accountBox);
    signInButton_->setObjectName("nextcloudSignIn");
    Ui::role(signInButton_, "primary");
    cancel_ = new QPushButton("Cancel sign-in", accountBox);
    cancel_->setObjectName("nextcloudCancelSignIn");
    retry_ = new QPushButton("Try opening browser again", accountBox);
    retry_->setObjectName("nextcloudRetryBrowser");
    signOut_ = new QPushButton("Sign out", accountBox);
    signOut_->setObjectName("nextcloudSignOut");
    for (auto* button : {signInButton_, cancel_, retry_, signOut_}) { button->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed); buttons->addWidget(button); }
    buttons->addStretch(1);
    accountColumn->addLayout(buttons);
    appPasswordButton_ = new QPushButton("Use an app password instead", accountBox);
    appPasswordButton_->setObjectName("nextcloudUseAppPassword");
    appPasswordButton_->setFlat(true);
    appPasswordButton_->setCheckable(true);
    appPasswordButton_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    accountColumn->addWidget(appPasswordButton_, 0, Qt::AlignLeft);
    row(nextcloudForm_, "Account", accountBox, "Sign in opens Nextcloud in your browser, where you allow put.gd. It gets an app password of its own, kept in the desktop keychain or for this session.");
    connect(signInButton_, &QPushButton::clicked, this, &DavDestinationPanel::signInRequested);
    connect(cancel_, &QPushButton::clicked, this, &DavDestinationPanel::cancelSignInRequested);
    connect(retry_, &QPushButton::clicked, this, &DavDestinationPanel::browserRequested);
    connect(signOut_, &QPushButton::clicked, this, &DavDestinationPanel::signOutRequested);
    connect(appPasswordButton_, &QPushButton::toggled, this, [this](bool shown) { appPassword_ = shown; sync(); emit appPasswordToggled(shown); });

    folderForm_ = PreferenceRows::card(page, "Where files go");
    folderForm_->parentWidget()->setObjectName("nextcloudFolderSection");
    folder_ = line("nextcloudFolder", "put.gd");
    row(folderForm_, "Folder", folder_, "A folder in your Nextcloud files, such as put.gd or Screenshots/put.gd. put.gd creates it on the first upload.");
    addProblem(folderForm_, "folder");
    connect(folder_, &QLineEdit::editingFinished, this, [this] {
        const auto clean = dav::cleanFolder(folder_->text());
        if (clean != folder_->text()) { const QSignalBlocker block(folder_); folder_->setText(clean); }
        finished("folder");
    });

    // Plain WebDAV: the folder address and where its files are served.
    webdavForm_ = PreferenceRows::card(page, "WebDAV", "Enter the server's user and password under Credentials.");
    webdavForm_->parentWidget()->setObjectName("webdavServerSection");
    address_ = line("webdavAddress", "https://dav.example.com/uploads/");
    row(webdavForm_, "Folder address", address_, "The WebDAV address of the folder uploads go to.");
    addProblem(webdavForm_, "address");
    connect(address_, &QLineEdit::editingFinished, this, [this] {
        const auto clean = dav::collectionAddress(address_->text());
        if (!clean.isEmpty() && clean != address_->text()) { const QSignalBlocker block(address_); address_->setText(clean); }
        finished("address");
    });
    connect(address_, &QLineEdit::textChanged, this, [this] { sync(); });
    linkBase_ = line("webdavLinkBase", "https://files.example.com/uploads/");
    row(webdavForm_, "Link address", linkBase_, "Optional. Where these files are served publicly; links are this address and the file name. Leave empty to hand out the folder address itself.");
    addProblem(webdavForm_, "linkBase");
    connect(linkBase_, &QLineEdit::textChanged, this, [this] { sync(); });
    connect(linkBase_, &QLineEdit::editingFinished, this, [this] {
        const auto clean = dav::collectionAddress(linkBase_->text());
        if (clean != linkBase_->text() && !(clean.isEmpty() && linkBase_->text().trimmed().isEmpty())) { const QSignalBlocker block(linkBase_); linkBase_->setText(clean.isEmpty() ? linkBase_->text().trimmed() : clean); }
        finished("linkBase");
    });
    webdavInsecure_ = insecure("webdavAllowHttp");
    webdavInsecureRow_ = row(webdavForm_, "Plain HTTP", webdavInsecure_, plainHint);
    createFolder_ = new QPushButton("Create folder", this);
    createFolder_->setObjectName("webdavCreateFolder");
    createFolder_->setSizePolicy(QSizePolicy::Maximum, QSizePolicy::Fixed);
    createFolderRow_ = PreferenceRows::action(webdavForm_, "Missing folder", "The test found no folder at this address. Create it on the server.", createFolder_);
    connect(createFolder_, &QPushButton::clicked, this, &DavDestinationPanel::createFolderRequested);

    notice_ = new QLabel(this);
    notice_->setObjectName("davNotice");
    notice_->setWordWrap(true);
    notice_->setTextFormat(Qt::PlainText);
    notice_->setProperty("uiStatus", true);
    Ui::role(notice_, "secondary");
    page->addWidget(notice_);
    notice_->hide();
    setSettings(dav::nextcloudDefaults());
}
void DavDestinationPanel::setProvider(const QString& provider) {
    if (provider_ == provider) return;
    provider_ = provider;
    folderMissing_ = false;
    setSignIn(SignIn::Idle);
    sync();
}
void DavDestinationPanel::setSettings(const QJsonObject& s) {
    if (!built_) return;
    loading_ = true;
    const bool insecure = s["allowInsecureHttp"].toBool();
    server_->setText(s["server"].toString());
    folder_->setText(s.contains("folder") ? s["folder"].toString() : QStringLiteral("put.gd"));
    accountName_ = s["accountName"].toString();
    address_->setText(s["address"].toString());
    linkBase_->setText(s["linkBase"].toString());
    nextcloudInsecure_->setChecked(insecure);
    webdavInsecure_->setChecked(insecure);
    loading_ = false;
    sync();
}
QJsonObject DavDestinationPanel::settings() const {
    auto s = dav::defaults(provider_);
    if (!built_) return s;
    if (provider_ == dav::nextcloudId) {
        const auto typed = server_->text().trimmed();
        const auto base = dav::nextcloudServer(typed);
        s["server"] = typed.isEmpty() ? QString() : base.isEmpty() ? typed : base;
        s["folder"] = dav::cleanFolder(folder_->text());
        s["allowInsecureHttp"] = nextcloudInsecure_->isChecked();
        s["accountName"] = accountName_;
    } else {
        const auto typed = address_->text().trimmed();
        const auto clean = dav::collectionAddress(typed);
        s["address"] = typed.isEmpty() ? QString() : clean.isEmpty() ? typed : clean;
        const auto link = linkBase_->text().trimmed();
        const auto cleanLink = dav::collectionAddress(link);
        s["linkBase"] = link.isEmpty() ? QJsonValue(QJsonValue::Null) : QJsonValue(cleanLink.isEmpty() ? link : cleanLink);
        s["allowInsecureHttp"] = webdavInsecure_->isChecked();
    }
    return s;
}
void DavDestinationPanel::setSignIn(SignIn state, const QUrl& url) {
    signIn_ = state;
    if (state == SignIn::Idle) signInUrl_.clear();
    else if (url.isValid()) signInUrl_ = url;
    sync();
}
void DavDestinationPanel::setAppPasswordShown(bool shown) {
    if (!built_ || appPasswordButton_->isChecked() == shown) return;
    appPasswordButton_->setChecked(shown);
}
void DavDestinationPanel::setBusy(bool busy) { busy_ = busy; sync(); }
void DavDestinationPanel::setFolderMissing(bool missing) { folderMissing_ = missing; sync(); }
void DavDestinationPanel::addProblem(QFormLayout* form, const QString& field) {
    auto* label = new QLabel(this);
    label->setObjectName("davProblem_" + field);
    label->setWordWrap(true);
    label->setTextFormat(Qt::PlainText);
    form->addRow(label);
    form->setRowVisible(label, false);
    problems_.insert(field, label);
    forms_.insert(field, form);
}
void DavDestinationPanel::showProblem(const QString& field, const QString& message) {
    for (auto it = problems_.cbegin(); it != problems_.cend(); ++it) {
        if (!field.isEmpty() && it.key() != field) continue;
        if (message.isEmpty()) it.value()->clear();
        else Ui::feedback(it.value(), message, true);
        forms_.value(it.key())->setRowVisible(it.value(), !message.isEmpty());
    }
}
QJsonObject DavDestinationPanel::problems() const {
    QJsonObject out;
    for (auto it = problems_.cbegin(); it != problems_.cend(); ++it) if (!it.value()->text().isEmpty()) out[it.key()] = it.value()->text();
    return out;
}
void DavDestinationPanel::setNotice(const QString& text, bool success) {
    if (!built_) return;
    notice_->setText(text);
    notice_->setVisible(!text.isEmpty());
    if (success) Ui::success(notice_, text);
}
void DavDestinationPanel::sync() {
    if (!built_) return;
    const bool nextcloud = provider_ == dav::nextcloudId;
    PreferenceRows::sectionFor(nextcloudForm_)->setVisible(nextcloud);
    PreferenceRows::sectionFor(folderForm_)->setVisible(nextcloud);
    PreferenceRows::sectionFor(webdavForm_)->setVisible(!nextcloud);
    nextcloudForm_->setRowVisible(nextcloudInsecureRow_, nextcloudInsecure_->isChecked() || dav::plainHttpAddress(server_->text()));
    // Shown for an http:// folder address or link address on another computer: both follow the switch.
    webdavForm_->setRowVisible(webdavInsecureRow_, webdavInsecure_->isChecked() || dav::plainHttpAddress(address_->text()) || dav::plainHttpAddress(linkBase_->text()));
    webdavForm_->setRowVisible(createFolderRow_, !nextcloud && folderMissing_);
    const bool signingIn = signIn_ != SignIn::Idle;
    const bool signedIn = !accountName_.isEmpty();
    QString text;
    switch (signIn_) {
    case SignIn::Starting: text = QString::fromUtf8("Opening Nextcloud sign-in…"); break;
    case SignIn::Waiting: text = QStringLiteral("Finish signing in to Nextcloud in your browser, then choose Grant access."); break;
    case SignIn::BrowserFailed: text = QStringLiteral("Couldn't open the default browser for Nextcloud sign-in."); break;
    case SignIn::Idle: text = signedIn ? "Signed in as " + accountName_ : QStringLiteral("Signed out"); break;
    }
    account_->setText(text);
    signInButton_->setText(signedIn ? QString::fromUtf8("Sign in again…") : QString::fromUtf8("Sign in…"));
    signInButton_->setVisible(!signingIn);
    signInButton_->setEnabled(!busy_ && !server_->text().trimmed().isEmpty());
    cancel_->setVisible(signingIn);
    cancel_->setEnabled(true);
    retry_->setVisible(signIn_ == SignIn::BrowserFailed || (signIn_ == SignIn::Waiting && signInUrl_.isValid()));
    retry_->setEnabled(signInUrl_.isValid());
    if (signIn_ == SignIn::Waiting) retry_->setText(QStringLiteral("Open browser again"));
    else retry_->setText(QStringLiteral("Try opening browser again"));
    signOut_->setVisible(signedIn && !signingIn);
    signOut_->setEnabled(!busy_);
    appPasswordButton_->setVisible(!signingIn);
    appPasswordButton_->setEnabled(!busy_);
    createFolder_->setEnabled(!busy_);
}
QJsonObject DavDestinationPanel::state() const {
    auto out = settings();
    out["provider"] = provider_;
    out["signIn"] = signIn_ == SignIn::Starting ? "starting" : signIn_ == SignIn::Waiting ? "waiting" : signIn_ == SignIn::BrowserFailed ? "browserFailed" : "idle";
    // The browser page of a running Login Flow v2: an agent completes sign-in there.
    out["signInUrl"] = signInUrl_.isValid() ? QJsonValue(signInUrl_.toString(QUrl::FullyEncoded)) : QJsonValue();
    if (built_) {
        out["account"] = account_->text();
        out["appPasswordShown"] = appPassword_;
        out["plainHttpShown"] = provider_ == dav::nextcloudId ? nextcloudInsecureRow_->isVisibleTo(this) : webdavInsecureRow_->isVisibleTo(this);
        out["createFolderOffered"] = provider_ == dav::webdavId && folderMissing_;
        out["notice"] = notice_->isVisibleTo(this) ? QJsonValue(notice_->text()) : QJsonValue();
        out["errors"] = problems();
        out["actions"] = QJsonObject{{"signIn", signInButton_->isVisibleTo(this) && signInButton_->isEnabled()}, {"cancelSignIn", cancel_->isVisibleTo(this)},
                                     {"openBrowser", retry_->isVisibleTo(this) && retry_->isEnabled()}, {"signOut", signOut_->isVisibleTo(this) && signOut_->isEnabled()}};
    }
    return out;
}
}
