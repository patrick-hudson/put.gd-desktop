#pragma once
#include "ClipboardPayload.h"
#include "Error.h"
#include "Settings.h"
#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <functional>
#include <optional>
namespace putgd {
struct CopyPayload { QString kind,text;QStringList files;QByteArray encodedImage; };
// Calls originate on the session thread. Acquire/copy callbacks may arrive on any
// thread, including after cancellation; services marshal and lifetime-guard them.
// cancelAcquisition requests cancellation and need not synchronously drain callbacks.
// The adapter outlives its services and never reads clipboard payloads for revision queries.
// Capture actions are region, window, desktop, monitor (the output under the
// pointer when the action is prepared) and repeat (the rectangle of the last
// region capture, kept in the adapter's memory only; no selector).
class SourceAdapter {
public:
 using PreparedAcquisition=std::function<void(const ResourceLimits&,std::function<void(PayloadResult)>)>;
 virtual ~SourceAdapter()=default;
 // The resident supervisor sets the one live UI helper before it can show a
 // window, and clears it on exit. Native adapters exclude it at invocation.
 virtual void setUiProcess(qint64) {}
 void setCaptureAppearance(const QJsonObject& appearance) { captureAppearance_=appearance; }
 void setCaptureOutcome(const QString& outcome) { captureOutcome_=outcome; }
 // Region screenshots only (#629): freeze every monitor before selection and
 // cut the result from those frames. Recording selection always stays live.
 void setRegionScreenshotFirst(bool frozen) { regionScreenshotFirst_=frozen; }
 QString captureOutcome() const { return captureOutcome_; }
 // Delayed capture (#1363), in seconds. Only a live region reads it, when it is
 // prepared: the area is chosen first, then the countdown runs, then the pixels
 // are read. Every other delayed capture counts down before its target is
 // frozen, through delayed().
 void setCaptureDelay(int seconds) { captureDelay_=seconds>0?seconds:0; }
 // Shows the countdown and answers once it has ended and nothing of it is left
 // on screen: {} at expiry, cancelled when the user dismissed it or the
 // acquisition was cancelled, or why this desktop cannot show one. It reads no
 // pixels and freezes no target.
 virtual void countdown(int,std::function<void(Error)> reply) { reply({"delay_unavailable","Delayed capture is not available on this desktop",{}}); }
 // Whether the session's screen is locked now. A desktop with no way to tell
 // answers false; its capture path then decides.
 virtual void sessionLocked(std::function<void(bool)> reply) { reply(false); }
 // countdown(), then the lock check: a delay that expires on a locked screen
 // captures nothing.
 void delayed(int seconds,std::function<void(Error)> reply) {
  countdown(seconds,[this,reply=std::move(reply)](Error error) mutable {
   if(error){reply(std::move(error));return;}
   sessionLocked([reply=std::move(reply)](bool locked) mutable {
    reply(locked?lockedError():Error{});
   });
  });
 }
 static Error lockedError() { return {"session_locked","The screen was locked when the capture delay ended, so nothing was captured.",{}}; }
 // Repeat last region (#1363): refusals shared by every adapter.
 static Error noLastRegion() { return {"repeat_unavailable","There is no region to repeat yet. Capture a region first.",{}}; }
 static Error lastRegionLayoutChanged() { return {"repeat_layout_changed","The monitor layout changed since the last region capture. Capture a region again.",{}}; }
 virtual quint64 clipboardRevision() const=0;
 virtual QJsonObject capabilities() const=0;
 // Freeze native target identity/layout on the session thread before persistence
 // yields. Pixel acquisition waits until both journal checkpoints are durable.
 virtual PreparedAcquisition prepareAcquisition(const QString& action) {
  return [this,action](const ResourceLimits& limits,std::function<void(PayloadResult)> reply) {
   acquire(action,limits,std::move(reply));
  };
 }
 // Preparation may show region selection, but must not read pixels until the
 // returned continuation is invoked after both durable journal checkpoints.
 virtual PreparedAcquisition prepareAcquisition(const QString& action,const ResourceLimits&) {
  return prepareAcquisition(action);
 }
 // Early cancellation/failure reports metadata only; ordinary payload delivery
 // still belongs to the durable continuation. Legacy adapters need no preview.
 virtual PreparedAcquisition prepareAcquisition(const QString& action,const ResourceLimits& limits,
                                               std::function<void(Error)>) {
  return prepareAcquisition(action,limits);
 }
 // Optional source release reports that an independent prepared helper drained.
 // Legacy adapters keep interactive ownership until ordinary completion.
 virtual PreparedAcquisition prepareAcquisition(const QString& action,const ResourceLimits& limits,
                                               std::function<void(Error)> terminal,std::function<void()> released) {
  // Keep legacy completion ownership. An adapter with an independent prepared
  // helper must override this overload and report release only after drain.
  if(released)released();
  return prepareAcquisition(action,limits,std::move(terminal));
 }
 virtual void acquire(const QString&,const ResourceLimits&,std::function<void(PayloadResult)>)=0;
 virtual void cancelAcquisition()=0;
 // Revision acknowledged by the last successful app copy, not a later owner query.
 // Adapters without an acknowledgement cannot advance automatic-copy eligibility.
 virtual std::optional<quint64> lastCopyRevision() const {return {};}
 // Cancel unsent asynchronous commits when superseded, paused for an import,
 // cancelled, or shut down. Report copy_suppressed only when no write committed.
 virtual void invalidateCopyEligibility() {}
 virtual void copy(const CopyPayload&,std::function<void(Error)>)=0;
 // Per-user desktop setup this adapter runs: state, code, message, details.
 // "not_applicable" for adapters with no per-user desktop setup.
 virtual QJsonObject desktopSetup() const {return {{"state","not_applicable"}};}
 // Re-runs that setup. The reply carries the state after it finished.
 virtual void recheckDesktopSetup(std::function<void(QJsonObject)> reply) {reply(desktopSetup());}
protected:
 // Non-secret presentation context; adapters copy it when freezing each request.
 QJsonObject captureAppearance_;
 QString captureOutcome_;
 bool regionScreenshotFirst_=false;
 int captureDelay_=0;
};
}
