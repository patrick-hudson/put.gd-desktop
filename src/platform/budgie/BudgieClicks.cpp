#include "LabwcSession.h"
#include "BudgieClicks.h"
#include "core/Log.h"
#include <QDBusConnection>
#include <QDBusConnectionInterface>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QSaveFile>
#include <QStandardPaths>
#include <QXmlStreamReader>
#include <csignal>
#include <unistd.h>

namespace putgd {
namespace {
const QString service = QStringLiteral("org.putgd.App.Budgie");
const QString objectPath = QStringLiteral("/org/putgd/Clicks");
constexpr qint64 maximumRcBytes = 4 * 1024 * 1024;
constexpr int settleMs = 80;
// How long labwc is given to read rc.xml again before the capture starts. A reconfigure shows on screen as
// the pointer turning into the default arrow for about 130 ms (measured on labwc 0.20.2) and nothing else.
constexpr int reconfigureMs = 250;

struct Button { const char* name; int number; };
constexpr Button buttons[] = {{"Left", 1}, {"Middle", 2}, {"Right", 3}};
// labwc compares a press's modifiers with a bind's exactly, so each set a
// click is commonly made with has its own bind.
constexpr const char* clientModifiers[] = {"", "S-", "C-", "A-", "W-", "C-S-"};


bool isLabwc(qint64 pid) {
    QFile comm(QStringLiteral("/proc/%1/comm").arg(pid));
    return pid > 0 && comm.open(QIODevice::ReadOnly) && comm.readAll().trimmed() == "labwc" && QFileInfo(QStringLiteral("/proc/%1").arg(pid)).ownerId() == getuid();
}
// "TitleBar" and "Titlebar", "S-C-Left" and "C-S-left", "Mod1-Left" and
// "A-Left" name the same bind to labwc.
QString bindKey(const QString& context, const QString& button) {
    static const QHash<QString, QString> aliases{{"mod1", "a"}, {"mod4", "w"}, {"mod5", "m"}, {"mod3", "h"}};
    auto parts = button.toLower().split('-');
    const QString last = parts.takeLast();
    for (auto& part : parts) part = aliases.value(part, part);
    parts.sort();
    parts.removeDuplicates();
    return context.toLower() + '|' + parts.join('-') + '|' + last;
}
struct Scan {
    QString error;
    struct Range { qsizetype begin = 0, end = 0; };
    QList<Range> ours;               // elements carrying put.gd's marker
    qsizetype mouseEnd = -1;         // where the user's <mouse> element's end tag starts
    qsizetype rootEnd = -1;          // where the root element's end tag starts
    bool defaults = false;           // that <mouse> asks for labwc's default binds
    QHash<QString, QString> press;   // bindKey -> the actions of the user's own Press bind, as written
};
// One pass over rc.xml. Positions come from the text itself: a tag starts at
// the last '<' before the place the reader stands after it.
Scan scan(const QString& rc) {
    Scan result;
    QXmlStreamReader reader(rc);
    QStringList stack;
    qsizetype oursDepth = 0, oursBegin = 0, mouseDepth = 0, actionsBegin = -1;
    QString context, bind;
    bool sawMouse = false;
    while (!reader.atEnd()) {
        const auto token = reader.readNext();
        if (token != QXmlStreamReader::StartElement && token != QXmlStreamReader::EndElement) continue;
        // Where the tag ends: the reader may stand a character past its '>'.
        qsizetype after = std::min<qsizetype>(reader.characterOffset(), rc.size());
        while (after > 0 && rc[after - 1] != '>') --after;
        const qsizetype begin = rc.lastIndexOf('<', std::max<qsizetype>(0, after - 1));
        const QString name = reader.name().toString();
        if (token == QXmlStreamReader::StartElement) {
            stack.append(name);
            if (oursDepth) continue;
            if (reader.attributes().value("putgd") == QLatin1String(BudgieClicks::marker)) { oursDepth = stack.size(); oursBegin = begin; continue; }
            if (stack.size() == 2 && name == "mouse" && !sawMouse) { sawMouse = true; mouseDepth = 2; continue; }
            if (!mouseDepth) continue;
            if (stack.size() == 3 && name == "default") result.defaults = true;
            else if (stack.size() == 3 && name == "context") context = reader.attributes().value("name").toString();
            else if (stack.size() == 4 && name == "mousebind" && stack[2] == "context") {
                bind = reader.attributes().value("action").compare(QLatin1String("Press"), Qt::CaseInsensitive) == 0 ? bindKey(context, reader.attributes().value("button").toString()) : QString();
                actionsBegin = -1;
            } else if (stack.size() == 5 && stack[3] == "mousebind" && !bind.isEmpty() && actionsBegin < 0) actionsBegin = begin;
            continue;
        }
        const bool selfClosed = rc.mid(begin, 2) != QLatin1String("</");
        if (oursDepth) {
            if (stack.size() == oursDepth) { result.ours.append({oursBegin, after}); oursDepth = 0; }
        } else if (mouseDepth && stack.size() == 4 && name == "mousebind" && !bind.isEmpty()) {
            // A later bind replaces an earlier one with the same key, as in labwc.
            result.press[bind] = actionsBegin >= 0 && !selfClosed ? rc.mid(actionsBegin, begin - actionsBegin).trimmed() : QString();
            bind.clear();
        } else if (mouseDepth && stack.size() == 2) {
            mouseDepth = 0;
            // <mouse/> has no end tag to write before: treated as no mouse element.
            if (!selfClosed) result.mouseEnd = begin; else result.defaults = false;
        } else if (stack.size() == 1 && !selfClosed) result.rootEnd = begin;
        if (!stack.isEmpty()) stack.removeLast();
    }
    if (reader.hasError()) result.error = QStringLiteral("labwc's rc.xml is not well-formed XML (line %1): %2").arg(reader.lineNumber()).arg(reader.errorString());
    else if (result.rootEnd < 0) result.error = QStringLiteral("labwc's rc.xml has no root element to add mouse binds to");
    return result;
}
// The spaces and tabs before `at`, back to the start of its line; null when something else is on that line.
QString indentBefore(const QString& text, qsizetype at) {
    qsizetype from = at;
    while (from > 0 && (text[from - 1] == ' ' || text[from - 1] == '\t')) --from;
    if (from > 0 && text[from - 1] != '\n') return {};
    return from == at ? QStringLiteral("") : text.mid(from, at - from);
}
QString mousebind(const QString& button, const QString& actions, int number) {
    return QStringLiteral("<mousebind button=\"%1\" action=\"Press\">%2<action name=\"Execute\" command=\"%3\" /></mousebind>").arg(button, actions, BudgieClicks::command(number));
}
}
class BudgieClickAdaptor final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "org.putgd.App.Budgie")
public:
    explicit BudgieClickAdaptor(BudgieClicks* owner) : QObject(owner), owner_(owner) {}
public slots:
    Q_SCRIPTABLE void Click(uint button) { owner_->pressed(button); }
private:
    BudgieClicks* owner_;
};
QString BudgieClicks::command(int button) {
    return QStringLiteral("dbus-send --session --type=method_call --dest=org.putgd.App.Budgie /org/putgd/Clicks org.putgd.App.Budgie.Click uint32:%1").arg(button);
}
bool BudgieClicks::hasBinds(const QString& rc) {
    const auto found = scan(rc);
    return found.error.isEmpty() ? !found.ours.isEmpty() : rc.contains(QStringLiteral("putgd=\"%1\"").arg(QLatin1String(marker)));
}
QString BudgieClicks::withoutBinds(const QString& rc, QString* error) {
    if (error) error->clear();
    const auto found = scan(rc);
    if (!found.error.isEmpty()) { if (error) *error = found.error; return rc; }
    QString out = rc;
    for (qsizetype i = found.ours.size() - 1; i >= 0; --i) {
        // With the element goes the line break and indentation put.gd (or the bridge's own formatting) placed before it.
        qsizetype begin = found.ours[i].begin;
        while (begin > 0 && (out[begin - 1] == ' ' || out[begin - 1] == '\t')) --begin;
        if (begin > 0 && out[begin - 1] == '\n') { --begin; if (begin > 0 && out[begin - 1] == '\r') --begin; }
        else begin = found.ours[i].begin;
        out.remove(begin, found.ours[i].end - begin);
    }
    return out;
}
QString BudgieClicks::withBinds(const QString& rc, QString* error) {
    QString clean = withoutBinds(rc, error);
    if (error && !error->isEmpty()) return rc;
    const auto found = scan(clean);
    if (!found.error.isEmpty()) { if (error) *error = found.error; return rc; }
    const bool ownMouse = found.mouseEnd < 0;
    // Without a mouse element of the user's, labwc runs on its defaults; ours then asks for them too.
    const bool defaults = ownMouse || found.defaults;
    const qsizetype at = ownMouse ? found.rootEnd : found.mouseEnd;
    const QString closing = indentBefore(clean, at);
    const bool lines = !closing.isNull();
    const QString inner = ownMouse ? closing + "\t\t" : closing + "\t";
    auto actions = [&](const QString& context, const QString& button, bool focusRaise) {
        const auto own = found.press.constFind(bindKey(context, button));
        if (own != found.press.constEnd()) return *own;
        return defaults && focusRaise ? QStringLiteral("<action name=\"Focus\" /><action name=\"Raise\" />") : QString();
    };
    QStringList contexts;
    auto context = [&](const QString& name, const QStringList& binds) {
        const QString separator = lines ? "\n" + inner + "\t" : QString();
        contexts.append(QStringLiteral("<context name=\"%1\" putgd=\"%2\">").arg(name, QLatin1String(marker)) + separator + binds.join(separator) + (lines ? "\n" + inner : QString()) + "</context>");
    };
    QStringList client, titlebar;
    for (const char* modifiers : clientModifiers)
        for (const auto& button : buttons) {
            const QString chord = QLatin1String(modifiers) + QLatin1String(button.name);
            // labwc's defaults give a bare press on a window Focus and Raise.
            client.append(mousebind(chord, actions("Client", chord, !*modifiers), button.number));
        }
    for (const auto& button : buttons) titlebar.append(mousebind(QLatin1String(button.name), actions("Titlebar", QLatin1String(button.name), button.number == 1), button.number));
    context("Client", client);
    context("Titlebar", titlebar);
    QString block;
    if (ownMouse) {
        const QString separator = lines ? "\n" + inner : QString();
        block = QStringLiteral("<mouse putgd=\"%1\">").arg(QLatin1String(marker)) + separator + "<default />" + separator + contexts.join(separator) + (lines ? "\n" + closing + "\t" : QString()) + "</mouse>";
    } else block = contexts.join(lines ? "\n" + inner : QString());
    clean.insert(at, lines ? "\t" + block + "\n" + closing : block);
    return clean;
}
BudgieClicks::BudgieClicks(QObject* parent, QString rcPath) : QObject(parent), rcPath_(std::move(rcPath)), fixedPath_(!rcPath_.isEmpty()) {
    if (!fixedPath_) {
        const auto labwc = runningLabwc().value_or(LabwcSession{});
        labwc_ = labwc.pid;
        const QString config = labwc.xdgConfigHome.isEmpty() ? QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation) : labwc.xdgConfigHome;
        rcPath_ = !labwc.configDir.isEmpty() ? labwc.configDir + "/rc.xml" : config + "/budgie-desktop/labwc/rc.xml";
    }
    auto bus = QDBusConnection::sessionBus();
    if (bus.isConnected()) {
        adaptor_ = new BudgieClickAdaptor(this);
        exported_ = bus.registerObject(objectPath, adaptor_, QDBusConnection::ExportScriptableSlots);
    }
    settle_.setSingleShot(true);
    settle_.setInterval(settleMs);
    connect(&settle_, &QTimer::timeout, this, &BudgieClicks::changed);
    // The bridge rewrites the file in place; an editor may replace it.
    const QString directory = QFileInfo(rcPath_).absolutePath();
    if (QFileInfo::exists(directory)) watcher_.addPath(directory);
    if (QFileInfo::exists(rcPath_)) watcher_.addPath(rcPath_);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, [this] { settle_.start(); });
    connect(&watcher_, &QFileSystemWatcher::fileChanged, this, [this] { settle_.start(); });
    // Binds a crash left behind go as soon as the resident is up.
    settle_.start();
}
BudgieClicks::~BudgieClicks() {
    if (exported_) QDBusConnection::sessionBus().unregisterObject(objectPath);
    if (watching_) { watching_ = false; sync(); }
}
QJsonObject BudgieClicks::clickCapability() const {
    QJsonObject capability{{"route", "labwc-mouse-binds"}, {"drawnBy", "recorder"}, {"needsPointer", true}, {"beforeCapture", true},
        {"targetReasons", QJsonObject{{"window", "A window recording on labwc has no pointer position: the compositor's window capture source has no cursor source, and labwc reports no window geometry."}}}};
    QString reason, message;
    auto bus = QDBusConnection::sessionBus();
    const QFileInfo rc(rcPath_);
    if (!exported_ || !bus.isConnected()) { reason = "session_bus_unavailable"; message = "The session bus is unavailable, so labwc's mouse binds cannot reach put.gd."; }
    else if (!bus.interface() || bus.interface()->serviceOwner(service).value() != bus.baseService()) { reason = "budgie_service_unavailable"; message = "This put.gd does not own org.putgd.App.Budgie on the session bus, so labwc's mouse binds cannot reach it."; }
    else if (!rc.isFile()) { reason = "labwc_config_missing"; message = "labwc's configuration " + rcPath_ + " does not exist, so put.gd cannot add the mouse binds that report clicks."; }
    else if (!rc.isWritable() || !QFileInfo(rc.absolutePath()).isWritable()) { reason = "labwc_config_read_only"; message = "labwc's configuration " + rcPath_ + " cannot be written, so put.gd cannot add the mouse binds that report clicks."; }
    else if (!fixedPath_ && !isLabwc(labwc_) && !runningLabwc().value_or(LabwcSession{}).pid) { reason = "labwc_not_running"; message = "labwc is not running in this session."; }
    capability["available"] = reason.isEmpty();
    capability["targets"] = reason.isEmpty() ? QJsonArray{"monitor", "desktop", "region"} : QJsonArray{};
    if (!reason.isEmpty()) { capability["reason"] = reason; capability["message"] = message; }
    return capability;
}
Error BudgieClicks::sync() {
    QFile file(rcPath_);
    if (!file.open(QIODevice::ReadOnly)) return {"labwc_config_missing", "Cannot read labwc's configuration " + rcPath_, {}};
    if (file.size() > maximumRcBytes) return {"labwc_config_invalid", "labwc's configuration " + rcPath_ + " is too large to edit", {}};
    const QString text = QString::fromUtf8(file.readAll());
    file.close();
    QString error;
    const QString wanted = watching_ ? withBinds(text, &error) : hasBinds(text) ? withoutBinds(text, &error) : text;
    if (!error.isEmpty()) return {"labwc_config_invalid", error, {}};
    if (wanted == text) return {};
    QSaveFile out(rcPath_);
    if (!out.open(QIODevice::WriteOnly) || out.write(wanted.toUtf8()) < 0 || !out.commit()) return {"labwc_config_read_only", "Cannot write labwc's configuration " + rcPath_, {}};
    // What `labwc --reconfigure` does: SIGHUP has labwc read rc.xml again.
    if (!fixedPath_) {
        if (!isLabwc(labwc_)) labwc_ = runningLabwc().value_or(LabwcSession{}).pid;
        if (!isLabwc(labwc_) || ::kill(pid_t(labwc_), SIGHUP) != 0) return {"labwc_not_running", "labwc could not be told to read its configuration again", {}};
    }
    logging::info(logRecording(), watching_ ? "labwc mouse binds for click highlights added" : "labwc mouse binds for click highlights removed", {});
    return {};
}
void BudgieClicks::changed() {
    if (!watcher_.files().contains(rcPath_) && QFileInfo::exists(rcPath_)) watcher_.addPath(rcPath_);
    // The bridge may be half way through writing: a file that does not parse is read again at its next change.
    if (const auto error = sync(); error && error.code != "labwc_config_invalid" && error.code != "labwc_config_missing")
        logging::warning(logRecording(), "labwc's mouse binds for click highlights could not be kept in step", {{"error", logging::errorFields(error)}, {"watching", watching_}});
}
void BudgieClicks::watchClicks(const QJsonObject&, RecordingClicks::Sink sink, RecordingClicks::Reply reply) {
    const auto capability = clickCapability();
    if (!capability["available"].toBool()) { reply({capability["reason"].toString(), capability["message"].toString(), {}}, {}); return; }
    watching_ = true;
    sink_ = std::move(sink);
    if (const auto error = sync()) {
        watching_ = false;
        sink_ = {};
        reply(error, {});
        return;
    }
    // labwc reads its configuration again on its own time: the recording starts once it has.
    if (fixedPath_) reply({}, {});
    else QTimer::singleShot(reconfigureMs, this, [reply] { reply({}, {}); });
}
void BudgieClicks::unwatchClicks() {
    if (!watching_) return;
    watching_ = false;
    sink_ = {};
    if (const auto error = sync()) logging::warning(logRecording(), "labwc's mouse binds for click highlights could not be removed", {{"error", logging::errorFields(error)}});
}
void BudgieClicks::pressed(uint button) {
    // The helper knows where: the capture's own pointer position.
    if (watching_ && sink_ && button >= 1 && button <= 3) sink_({{"button", int(button)}, {"pointer", true}});
}
}
#include "BudgieClicks.moc"
