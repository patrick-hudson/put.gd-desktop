#pragma once
// Private control protocols between the resident (putgd.exe) and the transient
// helpers it supervises. This header is the contract every helper and the
// resident build against; keep it in sync with docs/ui-process-protocol.md,
// docs/image-editor.md and docs/s3.md (the Linux equivalents).
//
// Transport: newline-delimited JSON objects on the child's stdin (parent -> child)
// and stdout (child -> parent). Every frame is
//   {"version":1,"seq":"<decimal, starts at 1 per direction, +1 each frame>",
//    "type":"<frame type>","payload":{...}}
// Frames are at most 10 MiB. A gap, a repeat, an unknown type or an oversized
// frame fails the helper. stderr is drained to the log and never parsed.
// The parent kills a child that misses its deadlines; a child whose parent pipe
// closes exits at once. No frame carries credentials unless stated below, and no
// frame ever carries pixels: images travel through owner-only files whose path,
// byte count and SHA-256 are checked on both sides.
//
// Common handshake (every helper):
//   parent  bootstrap        {role, nonce, generation, build:{version,commit}, ...role fields}
//   child   ready            {nonce, build:{version,commit}}      (mismatch -> parent kills child)
//   parent  activate         {}                                   (the child may now show UI / do work)
//   child   heartbeat        {}                                   (every 1 s while active; 15 s silence fails)
//   child   log              {level, message}                     (optional, redacted text only)
//   parent  cancel           {}                                   (capture and s3 only: stop and exit; a
//                                                                 capture answers failed/capture_cancelled,
//                                                                 an s3 request answers its result with status
//                                                                 unknown or failed as the truth allows)
//
// --capture (role "capture"): live region/window/desktop acquisition.
//   bootstrap fields: mode (region|window|desktop), output:{path}, limits:{imagePixels,imageBytes},
//                     excludePids:[...], includeCursor (bool), monitors:[{id,rect:[x,y,w,h],scale}]
//   child  selecting      {}                                 (overlay visible; Escape cancels)
//   child  selected       {rect:[x,y,w,h], target:{kind, title?, pid?}}  (geometry frozen; overlay hidden)
//   child  checkpoint     {bytes, sha256, width, height}     (PNG fully written + flushed to output.path)
//   child  failed         {code, message}  codes: capture_cancelled, capture_geometry, capture_failed,
//                                          input_excessive, encoding_failed, artifact_io
//   parent checkpointAccepted {}            (child exits 0)
//   Deadlines: ready within 10 s of start; no deadline while selecting; checkpoint within 30 s of selected.
//
// --image-job (role "image-job"): decode and normalize an image away from the resident.
//   bootstrap fields: input:{path, kind: png|dib|file}, output:{path, kind: png|dib} (dib writes the
//                     CF_DIBV5 bytes a clipboard owner can hand out verbatim), preview:{path, edge} (optional),
//                     limits:{imagePixels, imageBytes}, reencode (bool: false keeps PNG bytes verbatim),
//                     previewOnly (bool: write only the preview; output may be absent)
//   child  checkpoint   {bytes, sha256, width, height, mime, previewWritten}   (bytes/sha256 of the output;
//                                                                              0/"" when previewOnly)
//   child  failed       {code, message}  codes: source_unsupported, input_excessive, encoding_failed, artifact_io
//   parent checkpointAccepted {}
//
// --s3-worker (role "s3"): exactly one request per process (docs/s3.md).
//   bootstrap fields: none beyond the common ones.
//   parent request  {kind, destination:{providerId,profileId,profileRevision,settings,credentials},
//                    credentials:{accessKey, secretKey, sessionToken?}, ...kind fields}
//     kind=checkName      candidate                      -> result {status: absent|present|inconclusive, objectReference:{key, etag?, versionId?}, concurrencyToken, error}
//     kind=upload         artifact:{path,bytes,sha256,mime}, candidate, publicationId, condition (absent|match),
//                         objectReference, concurrencyToken, timeoutMs?
//                                                        -> progress frames, then result {status: uploaded|collision|failed|unknown, receipt, error}
//     kind=stat           receipt                        -> result {status: own|foreign|missing|unknown, receipt, error}
//     kind=reconcile      same fields as upload           -> result {status: own|foreign|missing|unknown, receipt, error}
//     kind=remove         receipt, deleteId, progress    -> checkpoint frames (see below), then
//                                                           result {status: removed|missing|changed|failed|unknown|hidden_version_retained|version_retained|unsupported_safe_removal, progress, error}
//     kind=testDestination                               -> checkpoint frames, then result {status: ok|failed, upload, publicGet, cleanup, receipt?, progress?, error}
//   child  progress   {phase: preparing|uploading|finalizing, bytesTransferred?, bytesTotal?}   (at most every 250 ms)
//   child  checkpoint {progress:{schemaVersion:1, ...}}   parent -> checkpointAccepted {ok:true} | {ok:false, error}
//                                                        (the child sends the destructive request only after ok:true)
//   child  result     {...}                               parent -> resultAccepted {}  (child exits 0)
//   Object metadata on PUT: x-amz-meta-putgd-publication = publicationId, x-amz-meta-putgd-sha256 = sha256.
//   objectReference is {"key": "<full key>", "etag": "<quoted etag>", "versionId"?: "..."} and receipt.directUrl
//   is s3::publicObjectUrl(settings, key). Credentials never appear in result, progress, error or log frames.
//
// --ui VIEW (role "ui"): the on-demand window. It talks to the resident over the
// session pipe like the CLI for every request of its own; this channel carries the
// lifecycle, view state and the view commands the resident forwards.
//   bootstrap fields: session (pipe path), residentEpoch, state (last checkpoint, may be {}), view
//   parent activate            {view: settings|history|activity|about|editFile}
//   child  viewShown           {view}
//   parent invalidate          {kind: settings|history|operations|setup|update|shortcuts}
//   parent command             {version:1, requestId, command, params}   command: moveDestination | diagnostics |
//                              historySaveDiagnostics | fileDialog (ui.* session methods; at most 8 outstanding)
//   child  commandResult       {requestId, error:{code,message,details}, result}   on the window's thread, at
//                              most once; unanswered after 10 s the resident fails it with ui_command_timeout
//                              (outcomeUnknown). Codes: ui_command_unsupported (version), ui_command_unknown,
//                              ui_view_closed (no open window; also a helper exit), plus each command's own.
//   child  checkpoint          {state}   250 ms after a view-state change; the resident keeps the last valid
//                              one for ui.viewState and the next helper (no reply)
//   child  shortcutRecording   {active}          parent -> shortcutRecordingChanged {replyTo, active, error}
//   child  closeRequested      {sequence, state} parent -> closeAccepted {sequence} | closeCancelled {sequence}
//   child  closeConfirmed      {sequence}        (child exits 0)
//   parent closeCancel         {sequence}        (child keeps running; parent re-queues views)
//   State checkpoints are at most 8 MiB of compact JSON and never contain credentials, rows or pixels.
//
// --edit-image (role "image-edit"): docs/image-editor.md, "Process and privacy boundaries".
//   bootstrap fields: input:{path, bytes, sha256}, output:{path}, limits:{imagePixels, outputBytes},
//                     destinationLabel, recentColors:[...]
//   child  editing          {}
//   parent command          {requestId, command, params}   command: inspect|apply|preview|saveCopy|submit|cancel|raise
//   child  commandResult    {requestId, ok, result} | {requestId, ok:false, code, message}
//   child  finalizing       {}
//   child  checkpoint       {bytes, sha256, width, height, recentColors} | {code, message, recentColors}
//          codes: edit_cancelled, source_unsupported, input_excessive, edit_export_failed, image_job_io
//   parent checkpointAccepted {}                            (child exits 0)
//   Deadlines: ready within 20 s, finalize within 120 s, each command answered within 8 s.
#include "common/Json.h"
#include <string>

namespace putgd::protocol {
inline constexpr int version = 1;
inline constexpr size_t maximumFrameBytes = 10 * 1024 * 1024;
inline constexpr int heartbeatMs = 1000;
inline constexpr int silenceMs = 15000;
inline constexpr int startupMs = 10000;
inline constexpr int editorStartupMs = 20000;
inline constexpr int editorFinalizeMs = 120000;
inline constexpr int editorCommandMs = 8000;
inline constexpr int captureCheckpointMs = 30000;
inline constexpr int progressIntervalMs = 250;
inline constexpr size_t maximumUiStateBytes = 8 * 1024 * 1024;

struct Frame {
    std::string type;
    unsigned long long sequence = 0;
    json::Object payload;
};
// Encodes one frame line (with the trailing newline) for the given outgoing sequence.
std::string encodeFrame(const std::string& type, unsigned long long sequence, const json::Object& payload);
// Parses one complete line; false when the line is not a valid version-1 frame.
bool decodeFrame(const std::string& line, Frame& out);
json::Object buildIdentity();
bool buildMatches(const json::Object& identity);
}
