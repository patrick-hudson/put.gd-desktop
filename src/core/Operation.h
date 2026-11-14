#pragma once
#include "ArtifactStore.h"
#include "LocalArchive.h"
#include "StorageProvider.h"
#include <QJsonArray>
namespace putgd {
struct OperationItem {
 Artifact artifact; QString uploadStatus="stored",candidate,condition,concurrencyToken,publicationId;
 QJsonObject objectReference;
 StorageReceipt receipt; LocalCopy local;
 QString remoteStatus="not_requested";
 QString shareStatus="not_requested";
 QString shareIntent; // pending explicit create; inspection must not erase it
 QJsonObject shareIntentOptions; // non-secret options of that original intent
 Error error;
 QString phase;std::optional<UploadProgress> uploadProgress;
 // The provider's last acknowledged resumable-upload journal (UploadRequest::resume).
 // Durable but private: never part of publicJson().
 QJsonObject uploadJournal;
 QJsonObject toJson() const;
 static OperationItem fromJson(const QJsonObject&);
 UploadRequest uploadRequest(const Destination&,std::optional<int> requestTimeoutMs={}) const;
 bool linkAvailable() const;
 // An uploaded file whose wanted link was not confirmed or not yet made: one
 // link-only retry finishes it with the options kept at upload (#1496).
 bool linkFinishable() const;
 // The remote states in which a stored file may still be shared.
 static bool shareableRemote(const QString& remoteStatus);
};
// An operation's state (#1433). Its name is the journal's and the wire's string;
// never rename one. Unknown is a string no build of put.gd writes (a newer
// build's journal), kept verbatim and never changed by a step.
enum class OperationState {Accepted,Acquiring,Editing,Recording,Recognizing,Stored,Running,Completed,Partial,Failed,Cancelled,Interrupted,Unknown};
QString operationStateName(OperationState);
OperationState operationState(const QString&);
// The one table of allowed steps. Staying in the same state is not a step.
bool operationStep(OperationState from,OperationState to);
struct Operation {
 QString id,action,settingsRevision,created;
 Settings settings; Destination destination;
 QList<OperationItem> items;
 quint64 invocation=0,clipboardRevision=0;
 QString note;
 QString copyStatus="pending";
 bool cancelled=false;
 QString phase="accepted";
 std::optional<int> requestTimeoutMs; // Explicit retry budget, separate from immutable settings.
 Error error;
 QJsonObject toJson() const;
 QJsonObject publicJson() const;
 static Operation fromJson(const QJsonObject&);
 static bool activeState(const QString&);
 OperationState state() const {return state_;}
 QString stateName() const;
 bool active() const;
 // Every state change goes through here. A step the table does not allow
 // is refused and logged with its from and to; the state stays as it was.
 bool advance(OperationState to);
 // A state read from elsewhere, unchecked: a journal row or a test fixture.
 void restore(OperationState state) {state_=state;unknownState_.clear();}
private:
 OperationState state_=OperationState::Accepted;
 QString unknownState_;
};
}
