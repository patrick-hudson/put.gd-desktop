#include "storage/sftp/SftpCommon.h"
#include "storage/sftp/SftpSsh.h"
#include "storage/sftp/SftpWire.h"
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QtEndian>
using namespace putgd;
// SFTP destination (#1871). The Server field decides which machine ssh signs in to and the
// folder decides where files land; a paste read wrongly would send files, and the user's
// key, somewhere else without an error. The reply parser reads bytes from a server.
class SftpRulesTest : public QObject {
    Q_OBJECT
    static QJsonObject settings(const QString& server, const QString& folder = {}) {
        auto value = sftp::defaults();
        value["server"] = server;
        value["folder"] = folder;
        return value;
    }
    static QByteArray packet(quint8 type, const QByteArray& body) {
        QByteArray out(4, '\0');
        qToBigEndian(quint32(body.size() + 1), out.data());
        return out + char(type) + body;
    }
    static QByteArray u32(quint32 v) { QByteArray out(4, '\0'); qToBigEndian(v, out.data()); return out; }
private slots:
    void pastedAddressesFillTheirFields() {
        struct Case { const char* text; const char* host; int port; const char* user; const char* folder; };
        for (const auto& c : std::initializer_list<Case>{
                 {"files.example.com", "files.example.com", 0, "", ""},
                 {"files.example.com:2222", "files.example.com", 2222, "", ""},
                 {"alice@files.example.com:2222", "files.example.com", 2222, "alice", ""},
                 {"[2001:db8::7]:2200", "2001:db8::7", 2200, "", ""},
                 {"2001:db8::7", "2001:db8::7", 0, "", ""},
                 {"shots", "shots", 0, "", ""},
                 {"sftp://alice@files.example.com:2222/srv/www/shots", "files.example.com", 2222, "alice", "/srv/www/shots"},
                 {"sftp://files.example.com/~/shots", "files.example.com", 0, "", "~/shots"},
                 {"sftp://al%69ce@files.example.com/a%20b", "files.example.com", 0, "alice", "/a b"},
                 {"alice@files.example.com:/srv/shots", "files.example.com", 0, "alice", "/srv/shots"},
                 {"alice@files.example.com:shots", "files.example.com", 0, "alice", "shots"},
                 {"sftp://files.example.com:2222/", "files.example.com", 2222, "", "/"},
                 {"  files.example.com  ", "files.example.com", 0, "", ""}}) {
            QString why;
            const auto address = sftp::parseServer(c.text, &why);
            QVERIFY2(address, c.text);
            QCOMPARE(address->host, QString(c.host));
            QCOMPARE(address->port, c.port);
            QCOMPARE(address->user, QString(c.user));
            QCOMPARE(address->folder, QString(c.folder));
        }
        QCOMPARE(sftp::serverText("2001:db8::7", 2200), QString("[2001:db8::7]:2200"));
        QCOMPARE(sftp::serverText("files.example.com", 0), QString("files.example.com"));
    }
    void addressesThatAreNotOneAreRefused() {
        for (const auto* text : {"sftp://alice:secret@files.example.com/x", "alice:secret@files.example.com", "-oProxyCommand=x", "files.example.com:0",
                                 "files.example.com:70000", "files.example.com:123456", "files .example.com", "[files.example.com]:22", "[2001:db8::7", "files.example.com:", "sftp://h/x?y"}) {
            QString why;
            QVERIFY2(!sftp::parseServer(text, &why), text);
            QVERIFY(!why.isEmpty());
        }
        // A password is refused in words that say where it goes instead.
        QString why;
        sftp::parseServer("sftp://alice:secret@h/x", &why);
        QVERIFY(why.contains("Password"));
        QVERIFY(!why.contains("secret"));
    }
    void savedSettingsHoldOnlyTheHostAndPort() {
        QVERIFY(sftp::validate(settings("files.example.com:2222", "~/shots")).code.isEmpty());
        // A draft without a server saves; it can't be used yet.
        QVERIFY(sftp::validate(settings("")).code.isEmpty());
        QCOMPARE(sftp::validate(settings(""), true).details["field"].toString(), QString("server"));
        QCOMPARE(sftp::validate(settings("alice@files.example.com")).details["field"].toString(), QString("server"));
        QCOMPARE(sftp::validate(settings("h", "~bob/x")).details["field"].toString(), QString("folder"));
        auto link = settings("h");
        link["publicUrlBase"] = "http://files.example.com/shots/";
        QCOMPARE(sftp::validate(link).details["field"].toString(), QString("publicUrlBase"));
        link["publicUrlBase"] = "https://u:p@files.example.com/";
        QCOMPARE(sftp::validate(link).details["field"].toString(), QString("publicUrlBase"));
        auto extra = settings("h");
        extra["port"] = 22;
        QVERIFY(!sftp::validate(extra).code.isEmpty());
    }
    void foldersAndLinksAreWhereTheFileIs() {
        QCOMPARE(sftp::remoteFolder("~"), QString());
        QCOMPARE(sftp::remoteFolder("~/shots/"), QString("shots"));
        QCOMPARE(sftp::remoteFolder("/srv/shots//"), QString("/srv/shots"));
        QCOMPARE(sftp::remoteFolder("/"), QString("/"));
        auto s = settings("[2001:db8::7]:2200");
        QCOMPARE(sftp::linkFor(s, "a b.png"), QString());
        s["publicUrlBase"] = "https://files.example.com/shots";
        QCOMPARE(sftp::linkFor(s, "a b#?.png"), QString("https://files.example.com/shots/a%20b%23%3F.png"));
        QCOMPARE(sftp::location(s, "/srv/shots/a.png"), QString("[2001:db8::7]:/srv/shots/a.png"));
    }
    void aPassphraseIsNeverAPasswordAndBothAreOptional() {
        const QJsonObject profile{{"provider", "sftp"}};
        QStringList names;
        QVERIFY(sftp::namedCredentials(profile, {}, &names).code.isEmpty());
        QCOMPARE(names, QStringList({"password", "passphrase"}));
        QVERIFY(sftp::namedCredentials(profile, {{"password", "x"}}, nullptr).code.isEmpty());
        // An empty value clears that one secret.
        QVERIFY(sftp::namedCredentials(profile, {{"passphrase", ""}}, nullptr).code.isEmpty());
        QCOMPARE(sftp::namedCredentials(profile, {{"password", "a\nb"}}, nullptr).code, QString("credential_invalid"));
        QCOMPARE(sftp::namedCredentials(profile, {{"token", "x"}}, nullptr).code, QString("credential_invalid"));
    }
    // The password answers only ssh's own prompt for this target, the passphrase only its prompt
    // for a configured key: a jump host's prompt or a server's keyboard-interactive text gets nothing.
    void askpassAnswersOnlyExactLocalPrompts() {
        using sftp::PromptKind;
        const QStringList hosts{"files.example.com"}, keys{"/home/alice/.ssh/id_ed25519"};
        const auto kind = [&](const QString& askpass, const QByteArray& prompt) { return sftp::matchPrompt(askpass, prompt, "alice", hosts, keys).kind; };
        QCOMPARE(kind({}, "alice@files.example.com's password: "), PromptKind::Password);
        QCOMPARE(kind({}, "Enter passphrase for key '/home/alice/.ssh/id_ed25519': "), PromptKind::Passphrase);
        QCOMPARE(sftp::matchPrompt({}, "Enter passphrase for key '/home/alice/.ssh/id_ed25519': ", "alice", hosts, keys).keyFile, keys.first());
        for (const auto* other : {"alice@jump.example.com's password: ", "bob@files.example.com's password: ", "Password: ", "(alice@files.example.com) Password: ",
                                  "alice@files.example.com's password:", " alice@files.example.com's password: ", "Enter passphrase for key '/home/alice/.ssh/id_ed25519':",
                                  "Enter passphrase for key '/home/alice/.ssh/other': ", "Verification code: ", "Enter passphrase: "})
            QCOMPARE(kind({}, other), PromptKind::Other);
        QCOMPARE(kind("confirm", "alice@files.example.com's password: "), PromptKind::Other);
        // ssh prints a key path cut to 100 bytes; two keys sharing those bytes can't be told apart.
        const QString longKey = "/home/alice/" + QString(100, 'k') + "/id_ed25519", otherLong = "/home/alice/" + QString(100, 'k') + "/id_rsa";
        const auto cut = "Enter passphrase for key '" + longKey.toUtf8().left(100) + "': ";
        QCOMPARE(sftp::matchPrompt({}, cut, "alice", hosts, {longKey}).kind, PromptKind::Passphrase);
        QCOMPARE(sftp::matchPrompt({}, cut, "alice", hosts, {longKey, otherLong}).kind, PromptKind::Ambiguous);
        QCOMPARE(sftp::matchPrompt({}, "Enter passphrase for key '" + longKey.toUtf8() + "': ", "alice", hosts, {longKey}).kind, PromptKind::Other);
        QCOMPARE(kind("none", "Confirm user presence for key ED25519-SK SHA256:x"), PromptKind::Touch);
        QCOMPARE(sftp::hostToken("files.example.com", 2222, {}), QString("[files.example.com]:2222"));
        QCOMPARE(sftp::hostToken("files.example.com", 22, "shots"), QString("shots"));
        QVERIFY(sftp::literalHostToken("[files.example.com]:2222"));
        for (const auto* pattern : {"*.example.com", "a,b", "!a", "|1|abc", ""}) QVERIFY2(!sftp::literalHostToken(pattern), pattern);
    }
    // #1892: the SSH key file is passed to ssh on its own, as one argument, and ssh then offers that
    // key rather than every agent key first; without one, nothing changes.
    void aChosenKeyIsPassedToSshAlone() {
        auto s = settings("files.example.com:2222");
        s["user"] = "alice";
        QVERIFY(sftp::validate(s).code.isEmpty());  // a destination saved without keyFile
        QVERIFY(sftp::identityArguments(s).isEmpty());
        QVERIFY(!sftp::connectionArguments(s, "none").contains("-i"));
        s["keyFile"] = QJsonValue::Null;
        QVERIFY(sftp::validate(s).code.isEmpty());
        s["keyFile"] = "/home/alice/keys/deploy key";
        QVERIFY(sftp::validate(s).code.isEmpty());
        const auto args = sftp::connectionArguments(s, "none");
        const auto i = args.indexOf("-i");
        QVERIFY(i >= 0);
        QCOMPARE(args[i + 1], QString("/home/alice/keys/deploy key"));
        QCOMPARE(args.mid(i + 2, 2), QStringList({"-o", "IdentitiesOnly=yes"}));
        // Before the subsystem and the destination, so ssh reads it as an option.
        QVERIFY(i < args.indexOf("-s") && i < args.indexOf("--"));
        for (const auto* refused : {"keys/id_ed25519", "", "/home/alice/%h_key", "/home/${USER}/key", "/home/alice/a\nb"}) {
            s["keyFile"] = QString::fromUtf8(refused);
            QCOMPARE(sftp::validate(s).details.value("field").toString(), QString("keyFile"));
        }
        s["keyFile"] = 7;
        QCOMPARE(sftp::validate(s).details.value("field").toString(), QString("keyFile"));
        sftp::Resolved r;
        r.host = "files.example.com";
        r.port = 2222;
        r.user = "alice";
        r.agent = true;
        r.identityFiles = {"/home/alice/keys/deploy_ed25519", "/home/alice/.ssh/id_rsa"};
        r.chosenKey = "/home/alice/keys/deploy_ed25519";
        QCOMPARE(sftp::resolvedLine(r, false), QString("Connects to files.example.com:2222 as alice with key deploy_ed25519"));
    }
    // A key file ssh would skip says why, in words that say how to fix it, before ssh runs.
    void keyFilesSshCannotUseSayWhy() {
        QTemporaryDir dir;
        const auto write = [&](const QString& name, const QByteArray& content, QFile::Permissions mode) {
            QFile file(dir.filePath(name));
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write(content);
            file.close();
            QVERIFY(file.setPermissions(mode));
        };
        const auto owner = QFile::ReadOwner | QFile::WriteOwner;
        // A private key's first line, put together here so no key-shaped text sits in the source.
        const QByteArray privateHead = QByteArray("-----BEGIN OPENSSH ") + "PRIVATE " + "KEY-----\n";
        write("id_ed25519", privateHead, owner);
        write("id_ed25519.pub", "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIA alice@laptop\n", owner | QFile::ReadOther | QFile::ReadGroup);
        write("lonely.pub", "ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIA alice@laptop\n", owner);
        write("pasted", "ssh-rsa AAAAB3NzaC1yc2EAAAADAQABAAABAQ alice@laptop\n", owner);
        write("tabbed", "ssh-ed25519\tAAAAC3NzaC1lZDI1NTE5AAAAIA alice@laptop\n", owner);
        write("work.ppk", "PuTTY-User-Key-File-3: ssh-ed25519\n", owner);
        write("open", privateHead, owner | QFile::ReadGroup);
        QVERIFY(sftp::keyFileProblem(dir.filePath("id_ed25519")).code.isEmpty());
        // A .pub picked by mistake names the private key next to it.
        QString privateKey;
        QCOMPARE(sftp::keyFileProblem(dir.filePath("id_ed25519.pub"), &privateKey).code, QString("ssh_key_public"));
        QCOMPARE(privateKey, dir.filePath("id_ed25519"));
        QCOMPARE(sftp::keyFileProblem(dir.filePath("lonely.pub"), &privateKey).code, QString("ssh_key_public"));
        QVERIFY(privateKey.isEmpty());
        QCOMPARE(sftp::keyFileProblem(dir.filePath("pasted")).code, QString("ssh_key_public"));
        QCOMPARE(sftp::keyFileProblem(dir.filePath("tabbed")).code, QString("ssh_key_public"));
        QCOMPARE(sftp::keyFileProblem(dir.filePath("work.ppk")).code, QString("ssh_key_putty"));
        // ssh refuses a private key of yours that others can read; the message says how to fix it.
        const auto open = sftp::keyFileProblem(dir.filePath("open"));
        QCOMPARE(open.code, QString("ssh_key_permissions"));
        QVERIFY(open.message.contains("chmod 600") && !open.message.contains(dir.path()));
        QCOMPARE(sftp::keyFileProblem(dir.filePath("gone")).code, QString("ssh_key_missing"));
        QCOMPARE(sftp::keyFileProblem(dir.path()).code, QString("ssh_key_unreadable"));
        // ssh's own refusal of a configured key that is too open reads the same way.
        sftp::FailureContext context;
        context.resolved.user = "alice";
        const auto refused = sftp::classify("Permissions 0644 for '/home/alice/.ssh/id_rsa' are too open.\nIt is required that your private key files are NOT accessible by others.\n"
                                            "alice@files.example.com: Permission denied (publickey).\n", context);
        QCOMPARE(refused.code, QString("ssh_key_permissions"));
        QVERIFY(refused.message.contains("id_rsa") && !refused.message.contains("/home/alice"));
        context.resolved.chosenKey = "/home/alice/keys/deploy_ed25519";
        QVERIFY(sftp::classify("alice@files.example.com: Permission denied (publickey).\n", context).message.contains("with the key deploy_ed25519"));
    }
    // A password typed or pasted into an address never stays in it.
    void passwordsAreLeftOutOfAddresses() {
        bool had = false;
        QCOMPARE(sftp::withoutPassword("sftp://alice:pw@files.example.com:2222/srv", &had), QString("sftp://alice@files.example.com:2222/srv"));
        QVERIFY(had);
        QCOMPARE(sftp::withoutPassword("alice:p@ss@files.example.com:/srv", &had), QString("alice@files.example.com:/srv"));
        QCOMPARE(sftp::withoutPassword("alice@files.example.com:/srv", &had), QString("alice@files.example.com:/srv"));
        QVERIFY(!had);
        // A percent-encoded separator counts as one, in either case.
        QCOMPARE(sftp::withoutPassword("sftp://alice%3Apw@files.example.com/x", &had), QString("sftp://alice@files.example.com/x"));
        QVERIFY(had);
        QCOMPARE(sftp::withoutPassword("alice%3apw@files.example.com:/srv", &had), QString("alice@files.example.com:/srv"));
        QVERIFY(had);
        QCOMPARE(sftp::withoutPassword("sftp://alice@files.example.com/a%3Ab", &had), QString("sftp://alice@files.example.com/a%3Ab"));
        QVERIFY(!had);
        QCOMPARE(sftp::withoutUserInfo("https://u:p@files.example.com/shots/", &had), QString("https://files.example.com/shots/"));
        QVERIFY(had);
    }
    void hostKeysReadAsSshKeygenShowsThem() {
        // The fingerprint below is what ssh-keygen -lf prints for this public line.
        const QString line = "[127.0.0.1]:18380 ssh-ed25519 AAAAC3NzaC1lZDI1NTE5AAAAIFIE1eeuTiZD8O0K7jUXHFl1IBU5HCpFTlDsB6zPqGo0";
        QCOMPARE(sftp::fingerprintOfLine(line), QString("ED25519 SHA256:lKD1cde7y8vs6nPz/4s3tCa5I1qSUf7Et8Vb93KG/90"));
        QCOMPARE(sftp::hostOfLine(line), QString("[127.0.0.1]:18380"));
        QVERIFY(sftp::fingerprintValid(sftp::fingerprintOfLine(line)));
        QVERIFY(sftp::fingerprintOfLine("h ssh-ed25519 !!!").isEmpty());
    }
    void serverRepliesAreBounded() {
        using namespace sftp::wire;
        QString error;
        // A length beyond the bound is refused before anything is buffered for it.
        Framer huge;
        huge.append(u32(maximumPacket + 1) + "x");
        QVERIFY(!huge.next(&error));
        QVERIFY(!error.isEmpty());
        // Shell output before SFTP starts reads as an absurd length, not as a packet.
        Framer noise;
        error.clear();
        noise.append("Welcome to the server\n");
        QVERIFY(!noise.next(&error));
        QVERIFY(!error.isEmpty());
        // A name list may not claim more entries than the bound, nor strings longer than the packet.
        Framer names;
        names.append(packet(Name, u32(7) + u32(maximumNames + 1)));
        const auto body = names.next(&error);
        QVERIFY(body);
        QVERIFY(!parse(*body, &error));
        QVERIFY(!parse(QByteArray(1, char(Handle)) + u32(1) + u32(4000), &error));
        // A status may end after its code, as version 3 servers send it.
        const auto status = parse(QByteArray(1, char(Status)) + u32(9) + u32(Failure), &error);
        QVERIFY(status);
        QCOMPARE(status->code, quint32(Failure));
        // Two packets in one read come out one at a time.
        Framer two;
        two.append(packet(Status, u32(1) + u32(Ok)) + packet(Status, u32(2) + u32(Eof)));
        QCOMPARE(parse(*two.next(&error), &error)->id, quint32(1));
        QCOMPARE(parse(*two.next(&error), &error)->code, quint32(Eof));
        QVERIFY(!two.next(&error));
    }
};
QTEST_APPLESS_MAIN(SftpRulesTest)
#include "test_sftp_rules.moc"
