#pragma once
#include <QJsonObject>
#ifdef Q_OS_LINUX
#include <QDBusArgument>
#endif
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QStringList>
#include <QVariantMap>
#include <functional>
class QFileDialog;class QWidget;

namespace putgd {
// #259: every open, save and folder dialog. On Linux the desktop's own chooser
// runs in its XDG portal backend (org.freedesktop.portal.FileChooser over
// D-Bus), so the native GNOME, KDE or Cinnamon dialog appears without a Qt
// platform theme in this process: the resident's and helpers' generic theme
// stays as it is (docs/memory-strategy.md). Without a portal, a FileChooser
// interface or a folder-capable version, or when it errors, does not answer or
// fails before its chooser could be seen, Qt's widget dialog opens instead,
// themed from the Theme tokens. Closing the desktop's chooser is a cancel.
// PUTGD_FILE_DIALOG=fallback skips the portal (lab and tests).
class FileDialog final:public QObject {
 Q_OBJECT
public:
 enum class Mode { Open, Save, Folder };
 struct Filter { QString label; QStringList patterns; };
 struct Options {
  Mode mode=Mode::Open;
  QString title;
  QString folder; // the starting folder; empty lets the chooser pick
  QString name;   // Save: the suggested file name
  QList<Filter> filters;
 };
 // How the chooser ended: a local path, a cancel, or a failure the person
 // should see (it is not a cancel), with its reason.
 struct Result {
  enum class Status { Chosen, Cancelled, Failed };
  Status status=Status::Cancelled;
  QString path;  // Chosen: the local path
  QString error; // Failed: why, in words for the person
  bool chosen()const{return status==Status::Chosen;}
  bool failed()const{return status==Status::Failed;}
 };
 using Done=std::function<void(const Result&)>;
 explicit FileDialog(QWidget* owner);
 ~FileDialog()override;
 // Opens the chooser for owner's window, modal like QFileDialog::exec; false
 // (and nothing opens) while one is already open.
 bool open(const Options& options,Done done);
 bool isOpen()const{return phase_!=Phase::Idle;}
 // Closes the open chooser as a cancel (the portal's Request.Close).
 void cancel();
 // {open, mode, title, backend portal|fallback, parentWindow, fallbackReason,
 // last {status chosen|cancelled|failed, backend, path, fallbackReason}}, for
 // the windows' inspect and MCP.
 QJsonObject state()const;
 // The open fallback dialog, if that is what is showing (tests).
 QFileDialog* fallbackDialog()const;
 // Themes Qt's widget dialog in the active scheme and follows scheme switches.
 static void theme(QFileDialog* dialog);
 // How long the portal may take to accept the request (tests shorten it).
 static void setPortalCallTimeoutMs(int milliseconds);
 // How soon after the portal accepts the request a Response 2 still counts as
 // the portal failing rather than the person closing its chooser (tests).
 static void setPortalFailureWindowMs(int milliseconds);
 // The portal's parent_window for owner: x11:<xid> on X11, empty elsewhere
 // (Qt has no public xdg-foreign export for a Wayland window).
 static QString parentWindow(const QWidget* owner);
signals:
 void stateChanged();
private slots:
 void portalResponse(uint response,const QVariantMap& results);
private:
 enum class Phase { Idle, Portal, Fallback };
 void startPortal();
 void requestPortal();
 void abandonPortal();
 void openFallback(const QString& reason);
 void finish(const QString& status,const QString& path,const QString& error={});
 void subscribe(const QString& handle);
 void unsubscribe();
 void closeRequest(const QString& handle);
 void block(bool on);
 QPointer<QWidget> owner_;
 Options options_;
 Done done_;
 Phase phase_=Phase::Idle;
 QString backend_,parent_,reason_,handle_,token_;
 QJsonObject last_;
 QPointer<QFileDialog> fallback_;
 QPointer<QWidget> blocker_;
 QObject* gone_=nullptr;
 quint64 attempt_=0;
 QElapsedTimer asked_;
};
#ifdef Q_OS_LINUX
namespace portal {
// org.freedesktop.portal.FileChooser filters, a(sa(us)): 0 is a glob, 1 a MIME type.
struct FilterRule { uint type=0; QString pattern; };
struct Filter { QString label; QList<FilterRule> rules; };
void registerTypes();
QDBusArgument& operator<<(QDBusArgument&,const FilterRule&);
const QDBusArgument& operator>>(const QDBusArgument&,FilterRule&);
QDBusArgument& operator<<(QDBusArgument&,const Filter&);
const QDBusArgument& operator>>(const QDBusArgument&,Filter&);
}
#endif
}
#ifdef Q_OS_LINUX
Q_DECLARE_METATYPE(putgd::portal::FilterRule)
Q_DECLARE_METATYPE(putgd::portal::Filter)
#endif
