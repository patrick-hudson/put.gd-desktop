// Settings → Destinations for Nextcloud and WebDAV (#1872): the sign-in flow around the
// panel, the results of its setup jobs, and ui.davDestination. Kept out of
// SettingsWindow.cpp; the panel itself is DavDestinationPanel.
#include "SettingsWindow.h"
#include "DavDestinationPanel.h"
#include "Ui.h"
#include "platform/HostOpen.h"
#include "storage/dav/DavCommon.h"
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QJsonArray>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPointer>
#include <QPushButton>
#include <QTimer>
namespace putgd {
void SettingsWindow::davSignInEvent(const QString& setupId, const QUrl& url) {
    if (setupId != davSignInSetup_ || !dav_ || dav_->signIn() == DavDestinationPanel::SignIn::Waiting || dav_->signIn() == DavDestinationPanel::SignIn::BrowserFailed) return;
    // Only a page of the server this destination names is opened.
    const QUrl server(dav_->settings()["server"].toString());
    if (!QStringList{"http", "https"}.contains(url.scheme()) || url.host().compare(server.host(), Qt::CaseInsensitive) != 0) return;
    dav_->setSignIn(DavDestinationPanel::SignIn::Waiting, url);
    openDavBrowser();
}
void SettingsWindow::openDavBrowser() {
    if (!dav_ || !dav_->signInUrl().isValid()) return;
    const auto id = davSignInSetup_;
    host::openLink(dav_->signInUrl(), [guard = QPointer<SettingsWindow>(this), id](const QJsonObject& result) {
        if (!guard || guard->davSignInSetup_ != id || guard->dav_->signIn() == DavDestinationPanel::SignIn::Idle) return;
        guard->dav_->setSignIn(result["opened"].toBool() ? DavDestinationPanel::SignIn::Waiting : DavDestinationPanel::SignIn::BrowserFailed);
        emit guard->viewStateChanged();
    });
}
void SettingsWindow::cancelDavSignIn() {
    if (davSignInSetup_.isEmpty()) { dav_->setSignIn(DavDestinationPanel::SignIn::Idle); emit viewStateChanged(); return; }
    settings_.request("setup.cancel", {{"id", davSignInSetup_}}, this, [this](Error e, QJsonObject) { if (e) status(e); });
}
bool SettingsWindow::davSetupFinished(const QJsonObject& job, const QJsonObject& data, const Error& error, bool currentTest) {
    const auto profileId = job["profileId"].toString();
    const auto kind = job["kind"].toString();
    auto profiles = draft_["profiles"].toArray();
    int index = -1;
    for (int i = 0; i < profiles.size(); ++i) if (profiles[i].toObject()["id"] == profileId) index = i;
    const auto provider = index >= 0 ? profiles[index].toObject()["provider"].toString() : QString();
    if (!dav::isDav(provider)) return false;
    const bool shown = profileId == profileId_->text() && dav_->provider() == provider;
    const auto result = data["result"].toObject();
    // The account name Settings shows as "Signed in as …": saved with the destination, never a credential.
    const auto applyAccount = [&](const QString& name) -> bool {
        if (!shown || conflict_) return false;
        auto settings = dav_->settings();
        if (settings["accountName"] == name) return true;
        settings["accountName"] = name;
        dav_->setSettings(settings);
        dirty();
        queueEdit(dav_, "/profiles/{profile}/settings");
        refreshDestinationList();
        return true;
    };
    if (kind == "connectAccount") {
        if (data["setupId"] == davSignInSetup_ || job["setupId"] == davSignInSetup_) davSignInSetup_.clear();
        if (shown) dav_->setSignIn(DavDestinationPanel::SignIn::Idle);
        if (error.code == "nextcloud_signin_cancelled") { setupFeedback("Sign-in cancelled."); return true; }
        if (error) { status(error); return true; }
        const auto name = result["accountName"].toString(result["userId"].toString());
        if (!applyAccount(name)) { setupFeedback("Signed in for the original destination. Select it again to see its account."); return true; }
        refreshCredentialHint();
        setupFeedback(result["sharePasswordEnforced"].toBool()
            ? "Signed in as " + name + ". This server requires a password on every public link, which put.gd doesn't set, so uploads there won't get a link."
            : result["sharing"].isBool() && !result["sharing"].toBool() ? "Signed in as " + name + ". Public links are turned off on this server, so uploads would get no link."
            : "Signed in as " + name + ".", true);
        return true;
    }
    if (kind == "inspectAccount") {
        if (error) { status(error); return true; }
        if (provider == dav::nextcloudId) {
            const auto name = result["accountName"].toString(result["userId"].toString());
            applyAccount(name);
            setupFeedback("Signed in as " + name + ".", true);
        } else setupFeedback("The folder is there.", true);
        return true;
    }
    if (kind == "signOut") {
        if (error) { status(error); return true; }
        applyAccount({});
        refreshCredentialHint();
        const auto revokeError = result["revokeError"].toObject();
        setupFeedback(result["revoked"].toBool() ? QStringLiteral("Signed out. put.gd's app password was revoked on the server.")
                      : !revokeError.isEmpty() ? "Signed out here, but put.gd couldn't revoke its app password on the server (" + revokeError["message"].toString() +
                                                     "). Revoke it in Nextcloud under Personal settings → Security."
                                               : QStringLiteral("Signed out."), true);
        return true;
    }
    if (kind == "createFolder") {
        if (error) { status(error); return true; }
        if (shown) dav_->setFolderMissing(false);
        setupFeedback(result["created"].toBool() ? QStringLiteral("Folder created. Test again to check it.") : QStringLiteral("The folder already exists."), true);
        return true;
    }
    if (kind == "storeCredentials" && !error && provider == dav::nextcloudId && shown) {
        // An app password names no account: ask Nextcloud whose it is, for "Signed in as …".
        QTimer::singleShot(0, this, [this] { if (setupBlockedReason().isEmpty()) beginSetup("inspectAccount"); });
        return false;
    }
    if (kind == "testDestination" && shown && currentTest) {
        dav_->setFolderMissing(error.code == "webdav_folder_missing" && error.details.value("createFolder").toBool());
        QStringList warnings;
        for (const auto& value : result["warnings"].toArray()) warnings.append(value.toObject()["message"].toString());
        dav_->setNotice(warnings.join("\n"));
    }
    return false;
}

// ui.davDestination (#1872): the open Nextcloud or WebDAV form, driven as a user drives it. The
// reply is the form's state; it never carries a credential. Sign in presses Sign in…: the
// browser page put.gd opens is in the state as signInUrl, where an agent completes it.
void SettingsWindow::davCommand(const QJsonObject& params, std::function<void(Error, QJsonObject)> reply) {
    const auto action = params["action"].toString();
    const auto shown = [this](QWidget* widget) { return widget && widget->isVisibleTo(destinationPage_); };
    const auto usable = [&shown](QWidget* widget) { return shown(widget) && widget->isEnabled(); };
    const Error unavailable{"control_unavailable", "That control isn't shown or can't be used for this destination right now.", {}};
    if (!destinationPage_ || !destinationPage_->isVisible()) { reply({"ui_view_closed", "Open Destinations before driving it.", {}}, {}); return; }
    if (QApplication::activeModalWidget()) { reply(unavailable, {}); return; }
    if (action == "add") {
        const auto provider = params["provider"].toString();
        if (!usable(addDestination_)) { reply(unavailable, {}); return; }
        const auto count = profile_->count();
        addS3Profile(provider == dav::nextcloudId ? "Nextcloud" : "WebDAV", params["label"].toString(), provider);
        if (profile_->count() == count) { reply({"profile_limit", "The destination could not be added.", {}}, {}); return; }
        queueEdit(profileLabel_);
    } else {
        const auto requested = params["profileId"].toString();
        if (!requested.isEmpty() && requested != profileId_->text()) {
            const int index = profile_->findData(requested);
            if (index < 0) { reply({"profile_not_found", "No destination has this ID.", {}}, {}); return; }
            if (!usable(destinationList_)) { reply(unavailable, {}); return; }
            profile_->setCurrentIndex(index);
        }
    }
    if (profileIndex_ < 0 || !dav::isDav(providerType_->currentData().toString())) {
        reply({"dav_not_selected", "Select a Nextcloud or WebDAV destination in Destinations first, or name it with profileId.", {}}, {});
        return;
    }
    const auto press = [&](const char* name) -> bool {
        auto* button = findControl<QPushButton*>(name);
        if (!usable(button)) return false;
        button->click();
        return true;
    };
    if (action == "set") {
        const auto field = params["field"].toString(), value = params["value"].toString();
        const bool nextcloud = dav_->provider() == dav::nextcloudId;
        if (field == "allowInsecureHttp") {
            auto* box = findControl<QCheckBox*>(nextcloud ? "nextcloudAllowHttp" : "webdavAllowHttp");
            if (value != "true" && value != "false") { reply({"invalid_request", "Allow plain HTTP is true or false.", {}}, {}); return; }
            if (!usable(box)) { reply(unavailable, {}); return; }
            if (box->isChecked() != (value == "true")) box->click();
        } else {
            static const QHash<QString, QString> nextcloudFields{{"server", "nextcloudServer"}, {"folder", "nextcloudFolder"}};
            static const QHash<QString, QString> webdavFields{{"address", "webdavAddress"}, {"linkBase", "webdavLinkBase"}};
            const auto name = (nextcloud ? nextcloudFields : webdavFields).value(field);
            auto* edit = name.isEmpty() ? nullptr : findControl<QLineEdit*>(name);
            if (!usable(edit)) { reply(unavailable, {}); return; }
            edit->setFocus();
            edit->setText(value);
            edit->setModified(true);
            emit edit->editingFinished();
        }
    } else if (action == "appPassword") {
        auto* button = findControl<QPushButton*>("nextcloudUseAppPassword");
        if (!usable(button)) { reply(unavailable, {}); return; }
        if (button->isChecked() != (params["value"].toString() == "show")) button->click();
    } else {
        static const QHash<QString, const char*> buttons{{"signIn", "nextcloudSignIn"}, {"cancelSignIn", "nextcloudCancelSignIn"}, {"openBrowser", "nextcloudRetryBrowser"},
                                                         {"signOut", "nextcloudSignOut"}, {"createFolder", "webdavCreateFolder"}, {"test", "testDestination"}, {"unlock", "unlockCredentials"}};
        if (buttons.contains(action) && !press(buttons.value(action))) { reply(unavailable, {}); return; }
    }
    auto state = dav_->state();
    state["profileId"] = profileId_->text();
    state["label"] = profileLabel_->text();
    state["saved"] = !selectedDestinationChanged() && !savePending();
    state["status"] = destinationStatus_ ? destinationStatus_->text() : QString();
    state["storedHint"] = credentialHint_->text();
    state["storeEnabled"] = usable(findControl<QPushButton*>("storeCredentials"));
    state["testEnabled"] = usable(findControl<QPushButton*>("testDestination"));
    state["setupRunning"] = !setupJobs_.isEmpty();
    reply({}, state);
}
}
