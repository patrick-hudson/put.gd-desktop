# Interface wording

How put.gd's interface text reads: titles, status lines, tooltips, hints, notifications,
errors, Settings descriptions and dialogs. Every new or changed string follows it. Strings
written before #2028 are rewritten area by area, and until that is done an old string is not
a precedent. Action names and the other shared words are in
[visual language](visual-language.md#words-and-actions).

## Who we sound like

A quiet, direct Linux utility for developers and system administrators, in the spirit of
`git` or Wireshark. Users know what a clipboard, bucket, keyring and hotkey are. Say what
happened or what a control does, give the next step if there is one, and stop.

## Rules

1. **Name the event, not a wrapper.** Write `Last action failed` or `Upload status unknown`,
   not `Needs attention` or `Remote outcome needs inspection`.
2. **Plain English, not spec prose.** Words such as *acquisition*, *dispatch*, *reconcile*,
   *retained*, *outcome*, *undergoing* and *operation* belong in code and design documents.
   Write `Upload was cancelled after it was sent`, not `The request was cancelled after
   dispatch; reconcile its saved identity`.
3. **Short sentences with periods.** No semicolons, no colons that join clauses and no
   "…, so …; …, but …" chains. A colon only separates a label from its value
   (`Couldn't save %1: %2.`). Write `Paused (mouse moved)`, not `Paused: you moved the mouse`.
4. **No reassurance tics.** Leave out "Nothing is uploaded", "Your edits are kept", "Other
   files remain" and "X is unchanged" when the open window or the label already shows it.
   Keep a state line only where silence would mislead:
   - an upload the user started stopped before sending: say `the upload didn't start`;
   - a remote state put.gd can't confirm, such as an unknown upload, a hidden old version or
     a checksum mismatch;
   - a live privacy or hardware fact, such as the microphone staying open while it is on, or
     blurred text being recoverable;
   - a surprising scope, such as uploads started after a dialog opened that keep running.
5. **Exact facts.** Failure codes in backticks stay verbatim, and so do IAM permissions,
   limits, `%1` and `%2`, shortcuts and real labels as the UI shows them (`Retry upload`, not
   `Retry`). Sizes are KB, MB and GB with 1 KB = 1024 bytes, never raw byte counts. Never add
   a confirmation, an undo step or an "uploading…" notice to explain a string away.
6. **Spelling and verbs.** US English, except `cancelled` and `cancelling` with two l's.
   **Select** buttons and menu items (`Select Upload again`), **press** keys (`press %1`),
   **turn on** and **turn off** toggles, **change** dropdowns, **use** tools and formats.

## By kind of string

| Kind | Form | Example |
| --- | --- | --- |
| Window and dialog title | Sentence-case topic, then ` — put.gd`, so taskbars and window switchers still name the app. A subject follows the topic after a colon. The main window is `put.gd`; the About window is `About put.gd`. | `History — put.gd`, `QR code: %1 — put.gd` |
| Notification title | Sentence-case topic without `put.gd`: the desktop already shows the app as the notification's source. | `Update available` |
| Notification body | One or two sentences: the cause, then the next step. | `put.gd %1 is available. Download it in Settings → General → Updates.` |
| Status and tray | The concrete state in 2–4 words, at most 30 characters, no period. | `Finished with errors`, `Review before uploading` |
| Tooltip | One phrase: verb, target and `(Shortcut)`, with no period. A second short sentence only for a caveat someone would not guess. No tooltip on a container, and none that repeats the label. Anything longer goes in the user guide. | `Upload edited image (Ctrl+Enter)` |
| Empty state or hint | One line: what is empty, or the next action. | `No errors.` |
| Error | The code, what failed, what was or wasn't stored, and the exact button, setting or permission that fixes it. | `` `operation_settling` Previous attempt is still finishing. Wait a moment and try again. `` |
| Settings description | At most 3 sentences and 40 words: what it changes, its default, its limit or unit, the real trade-off (memory, privacy, turning itself off) and any failure code it causes. Internals and rare cases go in the user guide. | `Memory limit for uncompressed images waiting to be encoded (default: 64 MB). Captures that exceed this limit fail with image_memory_limit. Caps queued image data only, not total app memory.` |
| Dialog | The real effect and the real risks, such as a non-atomic SFTP delete or uploads that keep running. No `Effect on X:` prefix and no `Other files remain.` | `Stops uploads that were running when this dialog opened. Newer uploads keep running.` |

Fit is part of the string. A status or tray line must fit its widget without eliding at the
default text size, and an error must not lose its recovery step to elision (see
[visual language](visual-language.md#shared-implementation)).

## Where the detail goes

Detail cut from a tooltip or a Settings description is not dropped. It moves to the
[user guide](../user/user-guide.md), or to [failure codes](../user/errors.md) for an error,
in the same rollout, as a documentation pull request beside the code pull request.

## Examples

| Before | After |
| --- | --- |
| Needs attention | Last action failed |
| Completed with a problem | Finished with errors |
| Waiting for your decision | Review before uploading |
| Upload the finished, sanitized image (Ctrl+Enter). The source file is not uploaded. | Upload edited image (Ctrl+Enter) |
| No errors. When something goes wrong, it's listed here with what to do next. | No errors. |
| Link not copied: the clipboard changed during the upload. Copy it from History. | Link not copied because your clipboard changed. Copy it from History. |
| Nothing was uploaded: a change wasn't applied. Check the edits, then upload again. | Couldn't apply an edit, so the upload didn't start. Check your edits, then select Upload again. |
| `proton_result_unknown` The request was cancelled after dispatch; reconcile its saved identity before retrying | `proton_result_unknown` Upload was cancelled after it was sent, so the file may already be in Proton Drive. Check there before retrying. |
| Effect on S3: removes %1 only if the recorded identity still matches. Other objects remain. | Deletes %1 from S3 only if the recorded ETag still matches. |
| Choose Redact tool | Switch to Redact |
