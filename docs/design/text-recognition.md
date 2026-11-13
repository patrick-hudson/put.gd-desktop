# Text recognition and QR codes

put.gd can copy the text out of a screen capture and read QR codes in it. It can also
show a QR code for an uploaded file's link. This is
[#1428](https://github.com/patrick-hudson/put.gd-desktop/issues/1428). The user's view
is in the [user guide](../user/user-guide.md#copy-text-from-the-screen). This page is the
contract.

Recognition happens only when someone asks for it, and nothing recognized is ever stored.
It runs in one temporary helper, `putgd-ocr`, that is confined to a sandbox, takes
pixels in sealed memory and gives text back in memory. The resident and `putgd-ui`
never link or load Tesseract, Leptonica or ZXing-cpp.

Lines that start "TO CONFIRM before merge" mark a fact that is still being settled by
an open item of #1428. The coordinator replaces each one with the settled fact before this
page merges. They are listed in the pull request that adds this page.

## What it does

| Where | What | Starts from |
| --- | --- | --- |
| The Copy text action | Select a region, copy its text and QR code contents to the clipboard, and say how much in a notification | The command band, the tray, a hotkey, `action.invoke {"action":"text"}` |
| The editor's Copy text tool | Select recognized words on the canvas and copy them. List the codes found. Quick redact adds Redact marks over text of five kinds | The tool (G) at the end of the toolbar, `editor.recognize` |
| History's Copy text | The same as the action, read from an image's retained local copy | An image item's inspector and menu, `history.copyText` |
| History's Show QR code | A QR code of the item's link, with Copy image and Save | A link item's inspector and menu, `history.qr` |
| `text.recognize` | Recognition for an agent from a region, a window, a monitor, the desktop, the clipboard, a file or a History item | The session methods, `putgd-cli` and MCP |

What it does not do:

- It does not search History by the text inside captures.
- It does not recognize anything on its own, in the background or ahead of time.
- It does not download language data, translate, describe images, read handwriting, read
  linear barcodes or PDF417, or open a link from the action or from a notification.
- It has no rectangle parameter for a region. An agent that knows an area captures a
  monitor, window or desktop and filters the returned words by their boxes.

## The Copy text action

1. **Start.** The tray's **Copy text from region**, the main window's **Copy text**
   button, the `text` hotkey (unassigned by default) or `action.invoke {"action":"text"}`
   with an optional `delaySeconds`. A screen that is locked refuses it as it refuses every
   capture.
2. **Select.** The region selector of **Capture region** on that desktop, live or
   screenshot-first as configured. Escape cancels. Where a selector says what the capture
   will do, it says "Copies text". The capture delay, the "Also delay captures started by a
   hotkey" preference and their ordering work as they do for a region. A text capture never
   changes the rectangle that **Repeat last region** remembers.
3. **Recognize.** The pixels go to `putgd-ocr`. It reads codes first and fills their areas
   with the background, then recognizes the text.
4. **Copy.** One plain-text clipboard write, then one desktop notification:

   | Found | Clipboard | Notification |
   | --- | --- | --- |
   | Text | The text | "Copied 214 characters" |
   | Text and codes | The text with each code's content on a line of its own | "Copied 214 characters and 1 code" |
   | Only codes | The content, one per line | "Copied 1 code" |
   | Nothing | Unchanged | "No text found" |
   | The user changed the clipboard first | Unchanged | "Text not copied: the clipboard changed" |
   | The write failed | Unchanged | "Text could not be copied" |

   One character reads "1 character" and one code "1 code". When the result was cut, the
   message adds a second line, "The text was cut at a size or time limit.". When a language
   from Settings had no data, it adds "(German data is missing)" or "(German and French data
   are missing)". The notification is titled "Copy text", never contains recognized text, a
   host name or code content, and has no action buttons. A failed operation is titled "Text
   recognition failed" and carries the error's fixed message. A second press while one runs
   notifies "Text recognition is still running" and changes nothing else. With notifications
   off the copy still happens.

   A code's line goes before the first text line whose top edge is below the code's centre.
5. **Nothing recognized is stored.** See [operations, journal and clipboard](#operations-journal-and-clipboard).

The action never opens a link.

## Sources of `text.recognize`

| Source | How the pixels are acquired |
| --- | --- |
| `region` | The native selector, as the action does it |
| `window`, `monitor`, `desktop` | The adapter's `prepareAcquisition` for that source. Identity is frozen and pixels are read at the same points as for a capture on that adapter. Recognition changes no adapter's acquisition timing. A source an adapter cannot capture reports that adapter's own error |
| `clipboard` | An image on the clipboard, or exactly one copied local PNG or JPEG file, reserved as a clipboard import is. Anything else is `source_unsupported` |
| `file` | An absolute path to a local regular PNG or JPEG file, opened once and never through a link. A PNG is decoded in the resident with image-format detection off. A JPEG is decoded by the [image worker](image-worker.md) |
| `history` | The item's retained local copy, checked against its recorded size and SHA-256. Nothing is downloaded |

The screen sources hold the one interactive-acquisition slot (`busy_selecting`) until their
pixels are delivered. Every source counts against the pending-operation limit. The resident
runs one text operation at a time, counted from acceptance. A second request is refused with
`text_busy`, so a job never waits behind another one's selection.

## The `putgd-ocr` helper

`putgd-ocr` is a temporary helper that follows the [image worker](image-worker.md)'s
control protocol: a framed control pipe, a nonce, the build identity, an authenticated
checkpoint and a deadline fixed by the parent. It differs in how data moves and how it is
confined.

| Part | Source | Role |
| --- | --- | --- |
| `textjob` protocol | `src/core/TextJobProtocol.*` | The messages, bounds, error vocabularies, the filters for untrusted text and the language table. Qt Core only |
| `TextJob` | `src/core/TextJob.*` | The parent side, used by the resident and by the editor. Starts one helper, reaps it, reads its result |
| `TextService` | `src/app/TextService.*` | The resident's interface that the action, `text.recognize` and History call. Owns a text operation from acceptance to its end |
| `TextBackend` | `src/app/TextBackend.h` | The seam the service sees. Tests inject a fake |
| `putgd-ocr` | `src/ocr/` | The helper: confinement, layout and bands, recognition, codes, the copy filter |
| `textjob::runChild` | `src/ocr/TextJobChild.*` | The helper's half of the protocol, which maps the input and writes the result |

The helper links a Qt Core only protocol library, not `putgd_core`, which brings Qt GUI and
Qt SQL. The package audits refuse a `putgd-ocr` that needs QtGui, QtSql, QtNetwork or
QtDBus. It has no display, session socket, configuration or credentials. `putgd-ocr
--version-json` prints the build identity before it reads any input or confines itself, so
a package audit can identify it without a control pipe.

TO CONFIRM before merge: the `--version-json` path is in the packaging pull request
(#1438), not yet on the integration branch.

### How data moves

| Step | Rule |
| --- | --- |
| Input | A sealed memfd. The parent decodes the image, applies the crop and every Redact mark, flattens alpha onto white and converts to 8-bit grey in strips of 256 rows. It writes a 32-byte header (the bytes `PGTJ`, version 1, width, height, stride and three zero words, little-endian) and then the grey rows. It adds the seals `F_SEAL_WRITE`, `F_SEAL_SHRINK`, `F_SEAL_GROW` and `F_SEAL_SEAL`, starts the child, and closes its own copy once the child has started. The mapping never counts in the parent again. The child maps it read-only, and checks the seals and the exact size first |
| Descriptors | The child gets the input at descriptor 3 and the output at descriptor 4, set by `dup2` in the child-process modifier. Every other descriptor is closed on `exec`, and the helper closes the rest itself before it does anything. Descriptor 2 stays a capped pipe that the parent discards |
| Environment | Cleared. The child gets only `PUTGD_TEXT_DATA` (the language data directories, in lookup order), `OMP_THREAD_LIMIT=1` and `LC_ALL=C.UTF-8` |
| Output | A second memfd the parent opened. The helper writes JSON of at most 8 MiB only through that inherited descriptor, never by a path. The parent reads it only after the authenticated checkpoint (its byte count and SHA-256 must match) and a zero exit, through its own descriptor |
| Staging | None. Recognized text never reaches a disk, and neither do pixels from the screen, from the clipboard image or from a PNG file. A JPEG from the `file`, `clipboard` or `history` source is decoded by the image worker, which stages a copy of that file under `/tmp` as it does for every import |
| Lifetime | One job, then exit. `PR_SET_PDEATHSIG` ends the helper with its parent. The parent starts and waits for the helper on one thread that outlives it, because the signal follows the starting thread |
| One at a time | A second job in the same process is refused at once with `text_busy`. Nothing queues |

### Confinement

The helper confines itself at the top of `main`, before it reads any input. If any part
cannot be installed, the job fails with `helper_sandbox_unavailable`. That failure is the
authority. The resident's capabilities report only the known prerequisite in advance, the
kernel's Landlock version, and nothing stronger.

- `PR_SET_NO_NEW_PRIVS` and `PR_SET_DUMPABLE 0`.
- Resource limits: no core file. `RLIMIT_DATA` at 288 MiB, lowered to 192 MiB when the job
  reads English only. `RLIMIT_CPU` at 21 seconds for each recognition thread. `RLIMIT_FSIZE` at
  8 MiB, which is also the output cap. `RLIMIT_NOFILE` at 32. There is no `RLIMIT_AS`, because
  mapped libraries and glibc's reservations would trip it.
- **Landlock** through system calls, ABI 3 or later so that truncation by path is covered.
  The helper may read `/usr/lib`, `/usr/lib64`, `/lib`, `/lib64`, `/proc/self`,
  `/sys/devices/system/cpu`, its own installation prefix and the language data directories. It
  may write nowhere. TCP is denied from ABI 4. The signal and abstract-socket scopes are set
  from ABI 6. This is what stops a compromised helper from writing `~/.bashrc` or reading
  `~/.aws/credentials`.
- **seccomp** on every thread (`SECCOMP_FILTER_FLAG_TSYNC`), a hand-written x86_64 deny
  list with the architecture checked and x32 numbers refused. `socket` and `connect` fail with
  `EPERM`. `clone3` fails with `ENOSYS`, so glibc falls back to `clone`. These kill the whole
  process: `execve`, `execveat`, `ptrace`, `process_vm_readv`, `process_vm_writev`,
  `pidfd_getfd`, the three `io_uring` calls, `bpf`, `perf_event_open`, `userfaultfd`,
  `keyctl`, `unshare`, `setns`, `mount`, `pivot_root`, `chroot`, `fork`, `vfork`, and any
  `clone` that is not a thread or that asks for a new namespace. The helper can start threads
  but no process, and nothing can outlive it.
- Sanitizer builds skip the memory limit, the dumpable setting and the `ptrace` kill, which
  the sanitizers need. Confinement is tested on builds without a sanitizer only.

The architecture is x86_64, the only packaged one. The Landlock ABI of each host the engine
was tried on: Mint 22.3 on m74 (kernel 6.14) 6, Debian 13 on d69 (6.12) 6, Kubuntu 26.04 on
k72 (7.0) 8, Fedora 44 on f44 (7.2) 10, and GitHub's runners 8. The confinement test passes on
m74, d69, k72 and f44 and in CI's `ubuntu:24.04` container as an unprivileged user. It tries a
socket, an `exec`, a child process, a write in the home directory, a read outside the allowed
directories and a read of another process's memory after a banded, parallel recognition, and
all of them fail.

TO CONFIRM before merge: the engine follow-up (#1452) narrows the Landlock read rule from
the helper's whole prefix to `<prefix>/lib`, `lib64` and `share/putgd`, and allows signals
only to the helper's own process, so that a kernel below Landlock ABI 6 cannot let it signal
its parent. Describe the rule that merged.

### Bounds and budgets

| Bound | Value |
| --- | --- |
| Input | At most 24 megapixels and at most 32,768 pixels on a side. More is refused with `input_excessive`, which is the editor's own limit. As 8-bit grey that is at most 24 MiB, shared through the memfd |
| Deadline | Fixed by the parent before it prepares the input, 20 seconds at most. At the deadline the parent ends the exact child, with terminate and then kill, and the job is `text_job_timeout`. Cancellation is checked between waits |
| Result | Text at most 1 MiB. At most 20,000 words of at most 256 bytes each. At most 64 codes of at most 8 KiB each. Over a bound the result is cut there and `truncated` is true. The 8 MiB output cap governs all of them together, and the helper cuts candidates first, then words, then codes, then text |
| Languages | At most three per job and at most 64 MiB of data files in all. A language that a data file pulls in by itself counts toward both. Over either limit the job fails with `language_unavailable` |
| Memory | Peak resident size of the whole job under 128 MiB with the shipped English data for any accepted input, and under 80 MiB for a region up to 1 megapixel. With other data the budget grows by the size of the data files beyond English, 192 MiB at most. The `RLIMIT_DATA` ceiling comes from the same figure. The resident set size is what acceptance measures |

TO CONFIRM before merge: the 80 MiB figure for a region up to 1 megapixel is the plan's.
The engine measured 101 to 103 MiB on a 1-megapixel dense region with four threads (see
[measurements](#measurements)). The coordinator raised the budget for that case to 128 MiB
while the engine item measured it. State which figure the engine follow-up settles.

### Time budget

Recognition has a time budget inside the deadline. It stops 16 seconds after the job starts
and returns what it has recognized so far, in reading order, with `truncated` true. The
notification and the editor then say the text was cut. A job that has text to give never
fails for time. This was decided because recognition time follows the number of characters,
not the number of pixels. On m74 and d69 it costs 0.55 to 1 millisecond for each character on
Tesseract 5.3.4 and 5.5.0 alike, so a 24-megapixel page of dense text, about 100,000
characters, needs 54 seconds or more, and no pixel limit brings every accepted input inside
the deadline.

- A band is not started when the slowest band so far says it cannot finish.
- Every pass gets Tesseract's own monitor deadline.
- Most of a dense page's time is in Tesseract's layout and line finding, which the monitor
  cannot interrupt. A 1.5 second deadline stopped one pass after 717 of about 7,600 characters,
  and the pass still took 4.4 seconds.

### Scale, single pass and bands

- **Scale.** Chosen from the estimated text height, 1 to 3 times, and limited by a cap of 8
  megapixels on the scaled pixels of one job. Above the cap the scale falls toward 1 times. The
  cap was measured on m74: a larger one sent 24-megapixel inputs into the CPU limit. Scaling
  is done one band at a time from the unscaled input, so the whole scaled image never exists.
- **Single pass.** When the scaled image is at most 8 megapixels it is recognized whole, and
  Tesseract's own layout gives the reading order.
- **Bands.** A larger scaled image is cut into horizontal bands of 1 to 2 megapixels, never
  more than 8 megapixels after scaling with overlap included.
  - Each cut goes at the last blank row gap in the final quarter of the band.
  - With no gap there, the cut is made at the limit and the next band starts earlier by
    twice the estimated line height, and by no more than a quarter of the band. Every band
    starts strictly below the one before. A word found in both bands is merged by box overlap and
    keeps the higher confidence.
  - When a band would be shorter than four text lines, which happens for a very wide image, the
    scale drops to 1 times. If it still would be, the input is refused with `input_excessive`.
  - Reading order for a banded image is band by band, top to bottom, with Tesseract's order
    inside each band. The result says `banded: true`. There is no global multi-column order
    for an image large enough to be banded.
- **Threads.** Recognition runs on up to four threads, one engine each, from the CPUs the
  process may run on (its affinity or a container's cpuset, not the machine's online count).
  Bands run in parallel. A single pass of half a megapixel or more is laid out once and its
  blocks run in parallel. Results join in task order, never in the order the tasks finish.
  An extra engine starts only while the resident set stays under the job's memory budget.
- **Codes first.** ZXing-cpp reads the full grey image. Then each decoded code's area is
  filled with the background in every band's copy, so a band never needs a code, and code
  modules never leak into the text. Formats read are QR Code, Micro QR Code, Data Matrix and
  Aztec. The helper uses only `ReadBarcodes(ImageView, ReaderOptions)`, which exists in ZXing-cpp
  2.2.1, 2.3.0 and 3.1.1, and maps the formats by name across versions.
- **Polarity.** A dark page, light text on a dark background, is inverted by put.gd's own
  estimate before Tesseract sees it.

TO CONFIRM before merge: the recognition method is not final. The values above (page
segmentation mode 3, Otsu thresholding, the 1 to 3 times scale rule, no border, put.gd's own
inversion) are what merged in #1431. The engine follow-up (#1452) settles the method on the
real corpus and the per-class error rates. Replace this note with the chosen method and its
numbers.

### What the copy keeps

Two outputs come from one pass:

- **Text for copying.** Line breaks are kept and a blank line separates layout blocks.
  Words are dropped when a person would not want them or could not read them:
  - a word of one or two characters needs a confidence of 40 or more
  - a lone symbol must be a code symbol (as in `&&`, `+=` or `{`) with a confidence of 85 or more
  - a word of symbols alone may hold only code symbols, never a dash, quote or sign that an icon produced
  - a line under 5 pixels tall is dropped
  - a word whose glyphs are within 32 grey levels of its own background is dropped, so near-white text on white is not copied while the dimmest real interface text stays

  Tesseract splits URLs after the scheme and at dots ("https: //example. test"). The filter
  joins them back into one. Tesseract's word confidence does not separate icons from text on
  put.gd's own captures: real words scored 13 to 35 and every URL near 0, while icon noise
  scored 0 to 38. So the rules use shape, with the confidence floor only for short words.
- **Candidates.** Every recognized word with its box and confidence, unfiltered. Only the
  editor asks for them, for Quick redact. The resident never holds them. The `words` an agent
  reads are the filtered words of the copied text, not these.

## Untrusted content

Recognized text and code content come from whatever was on the screen. They are screen data
from an untrusted source and never instructions.

- Before any copy, display or agent reply, the text goes through one filter
  (`textjob::sanitizeText` and `sanitizeCodeContent`). The text is put in NFC. C0 controls
  except tab and newline are removed, as are C1 controls and DEL. U+2028 and U+2029 become
  newlines. Every format character (Unicode category Cf: the bidirectional override,
  embedding and isolate characters, and the zero-width, invisible and tag characters) is
  removed. Recognized text keeps U+200C and U+200D, which Persian and Indic scripts need. Code
  content keeps no format character. The parent applies the filter again to what it reads from the
  helper, so a compromised helper cannot skip it.
- A result with any field outside the contract, or any geometry outside the image, is refused
  as `text_incompatible`.
- The editor's code rows show any remaining unusual character escaped, as `\u{200B}`.
- Content that is not valid text (binary, or bytes that do not decode as UTF-8 or the code's
  declared charset) is reported as `kind: "binary"` with its length only. It is neither copied
  nor shown and does not count as a code in the notification. A region holding only binary
  codes ends as "No text found".
- The MCP descriptions of `get_text_recognition`, `get_editor` and `copy_history_text` say so.
- A link in a code is opened only from the editor's code row, only for an http or https URL
  of at most 2,048 characters without control characters, and only when the user presses Open link
  (or an agent calls `editor.openCode`). User-info in the URL is shown, not hidden.
- Copy text is an ordinary copy. put.gd does not mark it as a secret, so a clipboard manager
  such as Klipper may keep it.

## Languages and where data is read from

- English data ships with every package. Other languages come from the distribution's
  Tesseract language packages, or from `.traineddata` files in `~/.local/share/putgd/tessdata/`,
  which every package kind reads. put.gd does not download language data.
- The directories, in lookup order:

  | Directory | Holds |
  | --- | --- |
  | `~/.local/share/putgd/tessdata` | The user's files |
  | `<bin>/../share/putgd/tessdata` | The AppImage's own English data |
  | `/usr/share/tesseract-ocr/5/tessdata` | Debian, Ubuntu and Mint |
  | `/usr/share/tesseract/tessdata` | Fedora |
  | `/usr/share/tessdata` | Arch |

- Each language resolves on its own, in that order. A user file that fails to load falls back
  to the system copy of that language when there is one.
- Identifiers are validated by the pattern `^[a-z]{3,4}(_[a-z0-9]+){0,2}$`, with `osd` and
  `equ` refused because they are not languages. No engine configuration, variable or path is
  accepted from a caller. A saved preference is validated for the pattern, the count (1 to 3)
  and uniqueness only. Whether the data is installed is checked when a job starts. A removed
  language package therefore never makes a saved configuration invalid.
- The list of installed languages comes from the resident reading the names and sizes of
  `*.traineddata` files in those directories. That loads no engine and starts no helper.
  The helper validates a file when a job loads it.
- The default, when a job names none and Settings has no preference, is the desktop locale's
  language when its data is installed (a fixed ISO 639-1 to Tesseract table), then English.
- A language named in a `languages` parameter that cannot load fails the job with
  `language_unavailable` naming it. A language from Settings that cannot load is skipped when
  another loads. The result lists `languagesUsed` and `languagesSkipped`, and the notification adds
  "(German data is missing)".
- The editor's helper reads the same saved preference. The resident hands it over in the
  editor's bootstrap, and without one the editor uses the locale's language and then English.
- Settings has three choices, Language, Second language and Third language, in its Capture
  tab under Text recognition. The first reads "Automatic" with the languages that resolves
  to, and a saved language whose data went away stays chosen and says "(not installed)".

TO CONFIRM before merge: the Settings section is in the desktops pull request (#1442), which
is not merged.

## Operations, journal and clipboard

- **A route-free action.** `text` is in `Settings::invocableActionNames()` and in
  `shortcutOrder()`. It has a hotkey and nothing else: no destination, archive or naming, no
  place under Per-action exceptions, and it is exempt from `output_required`. The config schema
  accepts `actions.text` with only a `shortcut`. Schema 1 normalization adds it with an empty
  hotkey. As with every action added before it, a configuration written by this build is not
  accepted by an older build.
- **States.** `accepted`, `acquiring`, `recognizing`, then `completed`, `cancelled` or
  `failed`. Acceptance is journaled as for a capture on that adapter: operation ID, action,
  configuration revision and the two durable checkpoints. Recognition never starts before both
  are written, and `recognizing` is written before the helper starts. The journal holds states
  and the terminal outcome (character and code counts, or an error code) and nothing recognized.
  A text operation shows as current work like any accepted operation.
- **Recovery.** `HistoryStore::recover` marks an operation found in `recognizing` as
  `interrupted` with `text_interrupted` ("Text recognition was interrupted; nothing was
  copied"). One found in `accepted` or `acquiring` gets `source_interrupted`, as any capture
  does. Neither is rerun.
- **No upload path.** A text operation has no items. It does not go through the upload
  completion path (`performCopy`, posters, upload copies). It has its own completion.
- **Journal size.** A completed or cancelled text operation's row is deleted when it reaches
  that state, and only a row whose action is `text` and that has no items. A failed or
  interrupted one stays.
- **History.** A completed or cancelled text operation is not listed. A failed or interrupted
  one is listed as "Text recognition failed" with its reason and no retry, like Capture failed.
- **Never recorded.** Recognized text, code content and source pixels are never written to the
  journal, the History database, logs, UI checkpoints, notifications or diagnostics. The logs say
  only that an operation began, its state, its copy outcome and its error. Diagnostics carry a
  `text` block with `active` and `resultCacheBytes`, and nothing recognized.
- **The clipboard.** The write goes through `deliverCopy` as an explicit copy, the way Copy
  link does. At acceptance it takes the explicit-copy ticket and the expected clipboard
  revision, and advances the upload-copy epoch, which also moves past any pending link copy. It
  is suppressed if the user changed the clipboard after acceptance, or a later explicit copy took
  the ticket. The outcome is `copied`, `suppressed` or `failed`, in the reply and the
  notification. `copy: false` takes no ticket, skips the write and the notification, and leaves the
  clipboard and every pending copy alone. The editor's text and code copies, History's Copy text
  and the QR image copy follow the same rule. PRIMARY is never touched.
- **Result retention for an agent.** The interactive action and History's own Copy text ask
  the helper for text and codes only, and keep nothing but counts. An operation started by
  `text.recognize`, or by `history.copyText` with `keepResult` true, keeps its result in the
  resident's memory, at most 4 MiB in all: the text first, then codes, then as many words as
  fit, with `truncated` true when anything was cut. It is one compact byte array, never a
  parsed tree, so freeing it returns the memory, and the resident asks for heap reclamation when
  it goes. It is kept until the next recognition is accepted, 120 seconds pass or the resident
  exits. It is never persisted. After that, `text.inspect` answers with `resultExpired: true`.
- **Terminal record.** The resident keeps an in-memory record of the last 32 text operations
  (state, source, counts, `copyStatus`, timings, error code). `text.inspect` answers from it once
  an operation ends. An older or unknown ID is `operation_not_found`. It holds nothing
  recognized.

## The editor

The Copy text tool (G) is the last tool of the toolbar, after Blur, behind its own separator.
Choosing it starts recognition for the current revision. Nothing is recognized before that. While
it is the active tool, a new recognition revision starts recognition again by itself. With any
other tool active it does not. The user's side is in the
[user guide](../user/user-guide.md#copy-text-in-the-editor). The rest of the editor is in
[image editor](image-editor.md).

- **Where it runs.** `putgd-ui --edit-image` starts `putgd-ocr` as its own child through the
  same `TextJob` class the resident uses, on a worker thread that outlives it. The class lives in
  `src/ui`, because `src/editor` may not include `core`. The pure parts, `TextLayer` and
  `QuickRedact`, stay in `src/editor`. A restart waits for the cancelled child to be reaped.
  Copies go to the resident as a `copyText` frame and through `deliverCopy`, so they survive the
  editor closing.
- **What recognition reads.** The source pixels inside the current crop, with every Redact
  mark painted opaque black. Other marks (arrows, text, blur, highlight) are not part of the
  input, and Blur does not hide text from it.
- **The recognition revision** changes only when a Redact mark changes (add, move, resize,
  remove) or the crop does, by any route including undo and redo. Arrows, text and other marks
  do not change it. A new revision cancels a running job and discards its result, clears any text
  selection and the code list, and rejects a result that arrives late.
- **Redacted text can never be copied.** Besides blacking the pixels, any word box or code
  quadrilateral that touches a Redact mark (sharing an edge counts) is dropped from the result
  before anything is shown, selected, copied or returned. A code that still decodes through its
  error correction is therefore not reported. Copy all text is rebuilt from what is left.
- **Selection.** Like text in a document. A press on a word and a drag to another selects
  every word between them in reading order. Shift+click extends, double-click takes the line,
  Ctrl+A takes everything, Ctrl+C copies while the canvas has focus (a text field keeps its own
  Ctrl+C). Escape clears the selection, and a second Escape returns to Select.
- **Panel.** The state ("Recognizing…", "214 words", "No text found", or a failure's
  message), Copy selection, Copy all text (codes included, as in the action), the note "Reads the
  original image. Marks you added are not included, and Blur does not hide text from it.", and
  "The text was cut at a size or time limit." for a truncated result.
- **Codes.** One row each, with its kind (QR code, Micro QR code, Data Matrix or Aztec code),
  its content on one elided line with unusual characters escaped, and Copy. A valid http or https
  URL's row shows the whole URL wrapped, with the host in its ASCII form, so it is in view before
  Open link is pressed. Open link then opens it at once through the desktop portal, with no
  dialog. A binary code's row shows its kind and length only, with Copy and Open link disabled.
- **Quick redact** is a menu in the Redact tool's panel (Email addresses, Phone numbers, IP
  addresses, Card numbers, Keys and tokens, All of these) with "Suggestions only. Recognition can
  miss text." always beside it.
  - Matching runs over the unfiltered candidates, joined across adjacent words and wrapped
    lines, after whitespace and Unicode digits are normalized. Where a kind's shape fixes the
    alphabet, the usual confusions are allowed: `O` for `0`, `l` and `I` for `1`, `S` for `5` and
    `B` for `8`.
  - Categories: `email`, `phone` (international and national forms, at least 7 digits), `ip`
    (IPv4 and IPv6), `card` (13 to 19 digits with spaces or dashes, Luhn-valid) and `key`. Keys and
    tokens means these shapes, listed in the tool's help: AWS access key IDs, GitHub, GitLab, Slack
    and Stripe prefixed tokens, JWTs, PEM private-key blocks, and unbroken hex or base64 runs of 32
    characters or more.
  - A word belongs to one match at most. Where kinds overlap the more specific wins, in the order
    key, email, card, ip, phone.
  - A match becomes Redact marks over its word boxes, padded by a quarter of the line height, one
    box per line, clamped to the image. One invocation is one undo step. The result says "Added 3
    suggested redactions" or "No matches found. Text can still be there.". There is no "safe"
    verdict and no upload confirmation.
  - Choosing a category with no result for the current revision starts recognition and waits for
    it, or joins the job already running, under that job's deadline. If a Redact mark or the crop
    changes while it waits, it ends with `edit_stale` and adds nothing.
  - A match is a suggestion. Redact is what removes pixels.

## History

- **Copy text** is on an image item's inspector and menu, beside Edit. It is for PNG and JPEG
  items and reads the retained local copy, the same rule as Edit again. With no local copy it is
  disabled with "Copy text needs the local copy. Turn on Keep a local copy in Settings → Capture
  for future captures.". put.gd never downloads an upload to read it. It calls `history.copyText`
  and reports counts in the status line.
- **Show QR code** is on an uploaded item whose link Copy link would copy. See
  [QR codes](#qr-codes).
- History search stays name-only.

## QR codes

- **Writing** uses Nayuki's QR Code generator (MIT), `qrcodegen.cpp` and `qrcodegen.hpp`,
  vendored unmodified from tag v1.8.0 in `src/core/qrcodegen/`. It compiles into the core
  library, so the resident (Copy image and Save through `history.qr`) and `putgd-ui` (the window)
  share it. It needs no helper and loads no image plugin. ZXing's writer is not used, because the
  distributions ship versions whose writer APIs differ.
- **The link** is exactly what Copy link would copy now, after the same receipt refresh. An
  expired link that can be refreshed is refreshed first, as **Refresh and copy link** does.
- **The code.** Byte mode at error correction level M exactly (`boostEcl` off), a 4-module quiet
  zone, whole pixels per module, black on white in both themes. A link over 2,331 bytes, the
  capacity of version 40 at level M, is refused with `qr_too_long`.
- **The window** draws the code at the largest whole number of device pixels per module that fits
  its code area (560 pixels square by default, growing with the window), never under 3. The
  largest code, 185 modules with its quiet zone, is then 555 pixels. It shows the full link
  under the code, so it can be checked before scanning, and offers Copy image and Save…, both
  through `history.qr`, so the window touches neither the clipboard nor a file.
- **Copy image and Save** produce a 1-bit PNG at the smallest whole scale that reaches 1,024
  pixels, whatever the window shows. The PNG holds only IHDR, IDAT and IEND and is written with
  zlib by hand. Save needs an absolute `.png` path and fails on an existing file unless
  `overwrite` is true, as the editor's Save a copy does.
- **When it is offered.** Show QR code is disabled, with the reason in its tooltip, for a
  private, revoked or removed link and for an expired one that cannot be refreshed. The reasons
  are `details.reason` of `link_unavailable`: `private`, `revoked`, `removed`, `expired` or `none`.

## Agent contract

Every interactive capability has a session method, a `putgd-cli` form (the CLI forwards any
method as it is) and an MCP tool with strict validation, `additionalProperties: false`.
`schemas/text-recognition.schema.json` states every
helper message and session method as data, with examples under `tests/fixtures/text/`. A
parameter that names an operation is spelled `id`. The recognition methods reply at once and are
followed through `inspect`, as for scrolling capture and recording.

| Method | Parameters | Returns |
| --- | --- | --- |
| `action.invoke` | `action: "text"`, `delaySeconds?` (0, 3, 5 or 10) | `operationId` and `state`. Copies |
| `text.recognize` | `source` (region, window, monitor, desktop, clipboard, file or history), `path` for `file` only, `historyId` for `history` only, `languages?` (1 to 3 installed identifiers), `copy?` (default false), `delaySeconds?` for the screen sources | `operationId`, `state`. Keeps its result for `text.inspect` |
| `text.inspect` | `id` | `state`, `source`. Once ended: `characterCount`, `codeCount`, `copyStatus`, `truncated`, `banded`, `languagesUsed`, `languagesSkipped`, `timings` (`spawnMs`, `engineStartMs`, `recognizeMs`, `copyMs`, `totalMs`) and `error`. While the result is kept also `text`, `words` `[{text, rect: [x,y,w,h], confidence}]` and `codes` `[{format, kind: "text" or "binary", text, length, quad}]` in source pixels. Afterwards `resultExpired: true` |
| `text.cancel` | `id` | The terminal record. Answers once the running job has stopped, up to 35 s. Nothing is copied |
| `capabilities` | none | A `text` block: `available`, `error` with the install hint or the missing confinement, `engines`, `landlockAbi`, `languages` `[{id, bytes, kind}]`, and `sources` with `available` and `error` for each source. It starts no helper |
| `editor.recognize` | `id` | `state` and `result`. Starts recognition for the current revision. `editor.inspect` gains a `text` block |
| `editor.selectText` | `id`, `result`, and `from` with `to` (word indexes in reading order), or `all`, or `none` | The selection |
| `editor.copyText` | `id`, `result`, `all?` (default false, the current selection) | `copyStatus` and the character count |
| `editor.copyCode`, `editor.openCode` | `id`, `result`, `index` | `copyStatus`, or the launch result. Open is refused for anything but a valid http or https URL |
| `editor.apply` command `redactText` | `categories` (1 to 5 of email, phone, ip, card, key) | Created shape IDs and counts per category. It must be the only command in its batch, or the batch is refused with `edit_command_invalid` before any recognition. It answers when it is done, not at once, within the recognition deadline plus a margin |
| `history.copyText` | `id`, `keepResult?` (default false) | `operationId` and `state`. Follows `text.inspect` |
| `history.qr` | `id`, `mode` (`copy`, `save` or `show`), `path` (absolute, `save` only), `overwrite?` (`save` only) | Once the receipt refresh and the copy or write are done, under Copy link's deadline: `copyStatus`, the saved `path`, or the window state. `show` needs the History window open |

`editor.inspect`'s `text` block has `state` (`idle`, `recognizing`, `done` or `failed`),
`result` (a positive integer that identifies one recognition result and changes with every new
one, or null), `words`, `codes`, `truncated`, `selection` (`{from, to}` or null) and, for a
failure, `error`. Every indexed editor method checks `result` against the current identity and
refuses a stale one with `edit_stale` before anything is copied.

The MCP tools are `recognize_text`, `get_text_recognition`, `cancel_text_recognition`,
`recognize_editor_text`, `select_editor_text`, `copy_editor_text`, `copy_editor_code`,
`open_editor_code`, `copy_history_text` and `history_qr_code`, and `perform_action` takes `text`.
See [the MCP helper](../automation/mcp.md#text-recognition-and-qr-codes).

### Failure codes

Each failure has a fixed message with no path or payload. The user's side is in
[errors](../user/errors.md#text-recognition-and-qr-codes).

| Code | From | Meaning |
| --- | --- | --- |
| `input_excessive` | Helper, parent | Over 24 megapixels, over the edge limit, or a wide image that bands cannot cut |
| `text_job_io` | Helper | The helper could not read its input or write its result |
| `language_unavailable` | Helper, resident | A language's data is not installed, or a job named more than three, or more than 64 MiB. Names the language |
| `engine_unavailable` | Helper, resident | No English data, or the engine failed to start |
| `helper_sandbox_unavailable` | Helper, resident | The confinement could not be installed, or the kernel's Landlock is older than ABI 3 |
| `text_job_timeout` | Parent | The 20 second deadline passed |
| `text_job_failed` | Parent | The helper crashed, was killed, hit a resource limit, exited nonzero, or answered out of protocol |
| `text_incompatible` | Parent | The build, the protocol or the result did not match |
| `text_busy` | Parent, resident | Another text operation is running |
| `text_helper_missing` | Parent, resident | `putgd-ocr` is not installed next to the resident |
| `text_interrupted` | Journal | The resident stopped during recognition |
| `edit_stale` | Editor | A Redact mark or the crop changed, so the result or the Quick redact is no longer current |
| `qr_too_long` | History | The link is over 2,331 bytes |

Others that these methods use are `source_unsupported`, `source_unavailable`,
`image_unavailable` and `history_not_found` (a source that is not an image, cannot be read or has
no retained copy), `session_locked`, `busy_selecting`, `queue_full`, `link_unavailable` and
`qr_empty` (History), `file_exists` and `save_failed` (QR save), and `ui_unavailable` and
`ui_view_closed` (`history.qr` with `show`).

## Desktops

`text` acquires exactly as `region` does. Each adapter's early paths, which name `region` and
`edit`, learn it.

| Desktop | What treats `text` as a region |
| --- | --- |
| Cinnamon X11 | The early crosshair in `X11CoreSourceAdapter`, live and screenshot-first |
| Plasma | The `putgd-region` selector, its cleanup and the transparent-frame fence before ScreenShot2 |
| GNOME | The extension's map from an action to its region preparation, and the adapter's prepared hotkey tickets, which are indexed by action |
| Budgie | As Plasma, with Budgie's presentation-based lock witness |
| Hyprland | The early press catcher in `HyprlandSourceAdapter`, and the bind `putgd:text` |

An adapter that can select a region for text reports `text` in its `capture` capabilities,
and the capabilities `text.sources.region` folds the desktop's region source and the engine's
availability together. On an adapter without it `region` is unavailable with
`source_unsupported`, and the other sources work.

**GNOME's extension version.** `text` joins the extension's hotkey list, so the packaged
extension is version 23. As with every extension update, a session that still has version 22
loaded must log out and in once. Until then `GnomeAdapter` refuses region preparation with the
existing "log out once" step (`helper_outdated`), so Copy text from a region reports it, as
Capture region and region recording do, and the `text` hotkey reads `helper_outdated` and is not
counted as a failed restore of a saved hotkey. With version 22 loaded, `text.recognize` from
`window`, `monitor`, `desktop`, `clipboard`, `file` and `history`, the editor tools and History's
actions still work, because they need nothing new from the extension. Every existing gate keeps its
version rule. Scrolling capture stays available from version 22.

TO CONFIRM before merge: the desktops item (#1442) is not merged. Its facts are taken from its
branch. Cinnamon X11 on m74 and Plasma Wayland on d69 passed the hotkey, tray and window button
pipeline to an independent paste with a stand-in helper. GNOME on f44, Budgie and Hyprland have not
been driven, and the selector's "Copies text" label has not been seen on a screen. No desktop
acceptance has run for the real engine.

## Where the controls are

| Surface | Control |
| --- | --- |
| Command band | **Copy text**, after Capture scrolling region and before Upload clipboard. Narrow label "Text". The glyph `copy-text` is a letter T inside selection corners. It is a secondary command, icon-only at the stage where Capture scrolling region is. Its tooltip says "Select a region and copy the text in it. Nothing is saved or uploaded." It shows no output line, and its unavailable badge follows `text.sources.region` |
| Tray, both implementations | **Copy text from region**, directly after Capture scrolling region. The native menu appends ID 41 with no output row |
| Settings, Hotkeys | A row **Copy text from region** after Capture scrolling region |
| Settings, Capture | Text recognition: Language, Second language and Third language |
| Capture output summaries | Never list it |
| History | **Copy text** beside Edit, **QR code** beside Open and **Show QR code** in the item's menu |
| Editor | The Copy text tool (G), and Quick redact in the Redact panel |

## Packaging

These are hard dependencies, because the helper links the libraries and one complete package is
the house rule.

| Package kind | Runtime and English data | Build |
| --- | --- | --- |
| DEB (Ubuntu 24.04, Mint 22, Ubuntu 26.04, Debian 13) | `tesseract-ocr-eng`, with `libtesseract5` and `libzxing3` found by the ELF scan | `libtesseract-dev`, `libleptonica-dev`, `libzxing-dev` |
| RPM (Fedora 44 and 45) | `tesseract-langpack-eng`, with `tesseract-libs` and `zxing-cpp` found by the ELF scan | `tesseract-devel`, `leptonica-devel`, `zxing-cpp-devel` |
| pacman | `tesseract`, `tesseract-data-eng`, `zxing-cpp` | The same |

- `PUTGD_OCR` is `AUTO` for developer builds, like `PUTGD_RECORDING`, and required in every
  package build. A package build without the libraries fails. At run time a missing helper or
  missing English data reports `text_helper_missing` or `engine_unavailable` with what to
  install, and the actions stay visible and say why they are off. Recording's Recommends are
  unchanged.
- The distributions ship ZXing-cpp 2.2.1 (Ubuntu 24.04, Mint 22 and Fedora), 2.3.0 (Ubuntu
  26.04 and Debian 13) and 3.1.1 (Arch). The helper is built and tested against both ends,
  Tesseract 5.3.4 with ZXing-cpp 2.2.1 on m74 and Tesseract 5.5.4 with Leptonica 1.87.0 and
  ZXing-cpp 3.1.1 built from source on k72.
- **AppImage.** It builds Leptonica (no image-format libraries at all), Tesseract (LSTM only,
  no curl, no archive, no OpenMP, no training tools) and ZXing-cpp (readers only) from pinned
  sources, and carries English `tessdata_fast` pinned by commit and SHA-256. The engines go in
  `usr/lib` and the data in `usr/share/putgd/tessdata`. Tesseract is built with the run path
  `$ORIGIN`, because the helper's own run path does not cover transitive dependencies. Without it,
  a host that has Leptonica installed would bring back every image-format library. The helper
  gets `$ORIGIN/../lib` after deployment, and the audits check every resolved library path from an
  empty environment, so a host copy cannot satisfy an image-owned library name.
- The audits (`tools/releases/audit_native.py` and the AppImage audit) require `putgd-ocr`,
  refuse Tesseract, Leptonica or ZXing-cpp in any other product binary, and refuse QtGui, QtSql,
  QtNetwork and QtDBus in the helper. `libgomp` is allowed: Qt Core already brings it into every
  resident through `libb2` on Debian 13, Ubuntu 26.04, Mint 22 and Fedora 44.
- The engines and the data join `bundled-components.json` with SBOM identities, complete-source
  collection, configure recipes and notices, and the exact-asset audit. They do not bypass the
  CVE gate. The QR generator is a component of every package, with its MIT notice.

TO CONFIRM before merge: the packaging pull request (#1438) is not merged and its AppImage
size, whole-package size, stable CVE rehearsal and policy acceptances are open. See
[release](../release/release.md#text-recognition-dependencies-1428) and
[CI](../release/ci.md#text-recognition-packaging) for the rules as they stand.

## Performance and memory

| Rule | Check |
| --- | --- |
| No binary but `putgd-ocr` needs libtesseract, libleptonica or libZXing | The audit rule, on every package build |
| The resident never loads them, or libcurl or libarchive, at run time | After a workload of 20 `text.recognize` monitor jobs and 5 driven region actions and a 180 second wait, the resident's `/proc/<pid>/maps` against the idle forbidden-library list plus these names, on every desktop. `libgomp` is not on the list |
| Idle resident RSS and PSS stay under 40 MiB after that workload, with every helper's exit recorded | Gate C on the same resident |
| The agent result cache is bounded | Resident RSS 60 seconds after a `text.recognize` of the largest result, against the same point without it: growth under 8 MiB, and gone after 120 seconds |
| `putgd-ui` startup and its mapped libraries are unchanged | An A/B of main-window and `--edit-image` open time and library list against the nightly head |
| Helper peak RSS within the budgets above | Measured in every cell, including a dense 24-megapixel input |
| From the end of the selection to the clipboard acknowledgement, p95 under 1 second for a region up to 1 megapixel and under 3 seconds for a full 4K monitor capture | 30 runs on m74 and on d69, native package and AppImage, 5 runs in every other cell |
| The hotkey-to-selector time of the other captures does not change | An A/B with the hotkey latency probe |
| No recognition without an explicit action, and no warm daemon | By design. The helper exits after one job |
| AppImage growth under the size target | Compared with the nightly head |

Spawn, engine start, recognition and copy are timed separately in every receipt, so a missed
bar shows where the time went.

TO CONFIRM before merge: none of the rows above has been measured on a candidate build.
Item 8 of #1428 (measurement and desktop acceptance) runs them once all items have merged.
Replace this note with its results.

### Measurements

These are measurements of the helper alone, taken before any candidate build existed. They are
not acceptance of a put.gd build. Times are `TextJob`'s `totalMs` (spawn to the result read back),
so they leave out the parent's grey conversion and the clipboard. Release builds of the helper,
three runs each, all within 1 percent. Threads were set by CPU affinity. Both hosts are an i5-6500T:
m74 (Mint 22.3, distribution Tesseract 5.3.4) and d69 (Debian 13, distribution Tesseract 5.5.0).
Peak is the helper's RSS.

| Input | Threads | Scale | m74 time | m74 peak | d69 time | d69 peak |
| --- | --- | --- | --- | --- | --- | --- |
| 540×80 paragraph | 1, 2, 4 | 2× | 0.29 s | 46 MiB | 0.28 s | 47 MiB |
| 1200×830 dense UI region (1 MP) | 1 | 2× | 1.11 s | 62 MiB | 1.09 s | 63 MiB |
| | 4 | 2× | 0.99 s | 101 MiB | 0.96 s | 103 MiB |
| 3840×2160 tiled UI (4K) | 1 | 1× | 6.32 s | 73 MiB | 6.49 s | 75 MiB |
| | 4 | 1× | 2.88 s | 103 MiB | 2.82 s | 105 MiB |
| 3840×2160 dense text (about 37,000 characters) | 4 | 1× | 14.4 s, cut at 35,262 characters | 122 MiB | 14.6 s, cut | 124 MiB |
| 6000×4000 tiled UI (24 MP) | 1 | 1× | 9.31 s | 91 MiB | 8.99 s | 92 MiB |
| 6000×4000 dense text (24 MP) | 4 | 1× | 15.8 s, cut | 139 MiB | 13.0 s, cut | 118 MiB |

- A dense 1-megapixel region costs 0.67 seconds at 1×, 0.94 seconds at 2× and 1.37 seconds at
  3× on m74, so 2× costs about 1.4 times 1×.
- The model load makes engine start 115 to 135 milliseconds. Not loading the word lists left it
  unchanged.
- Extra engines cost 20 to 45 MiB each. Without the memory gate, four threads reached 125 to
  211 MiB, and the dense 24-megapixel case failed at `RLIMIT_DATA`. Ungated, the 4K UI page took
  1.48 seconds and the 1-megapixel region 0.97 seconds.
- One overshoot is left: dense 24 megapixels at 139 MiB on m74, because the first band
  understates what the next ones add.
- On m74 the 1 second bar for a 1-megapixel region is missed by a hair: the helper's own work
  alone is 0.96 to 0.99 seconds before the parent's capture, grey conversion and clipboard. The
  3 second bar for a 4K monitor is met for dense interface (2.82 to 2.88 seconds) and not met for
  a 4K page of dense text, which hits the 16 second budget and is cut. Patrick decides whether the
  bars or the scale rule change.

TO CONFIRM before merge: the speed on m74 against the 1 second and 3 second bars. It is open
decision 3 of #1428, and the engine follow-up (#1452) may change the numbers.

### Quality

Quality is scored per engine build and per target class, never pooled. The fixed corpus is 174
real Capture region images from published nightly 26.1112.4660 on six desktops (m74 Mint
Cinnamon X11, d69 Debian Plasma, f44 Fedora GNOME, k72 Kubuntu Plasma, Budgie on labwc and
Hyprland on Omarchy), seven classes each, light and dark, at 100 and 125 percent. Each image has
a transcript. The scorer is `tests/performance/text/score.py`. It uses Unicode NFC, collapsed
whitespace and a case-sensitive Levenshtein distance. The bars are a median character error rate
of at most 2 percent for body text of 11 pixels and larger and at most 5 percent for smaller
interface text, and an exact match for at least 90 percent of URLs and email addresses of 11
pixels and larger. A second language (German) must meet the body bar on one engine build.

Distribution Tesseract on the corpus, before put.gd's method, shows why the method matters.
Default page segmentation 3 reads aligned text as columns, and page segmentation 6 does not.
Median character error rate, 1× scale, English data, `OMP_THREAD_LIMIT=1`:

| Host | Tesseract | Terminal, mode 3 | Terminal, mode 6 | Small interface, mode 3 | Small interface, mode 6 |
| --- | --- | --- | --- | --- | --- |
| m74 | 5.3.4 | 49.050 % | 0.000 % | 1.530 % | 1.530 % |
| d69 | 5.5.0 | 48.035 % | 0.033 % | 22.417 % | 7.871 % |
| f44 | 5.5.4 | 55.534 % | 0.196 % | 5.122 % | 54.457 % |
| k72 | 5.5.0 | 48.003 % | 0.000 % | 23.858 % | 1.397 % |
| Budgie guest on f44 | 5.5.4 | 48.068 % | 0.000 % | 17.193 % | 1.752 % |
| Hyprland guest on f44 | 5.5.3 | 46.824 % | 0.000 % | 2.963 % | 1.406 % |

These are method baselines, not the shipped method and not acceptance. The native monitors
were 1920×1080, so the full 4K case is not in the corpus yet.

TO CONFIRM before merge: the settled recognition method and its per-class error rates for each
engine build, against the bars above, from the engine follow-up (#1452) and item 8. Do not
publish a number that the corpus has not measured.

## Not built yet

- Desktop acceptance of any part. The editor tool and History actions have run in tests, in the
  synthetic catalog render and once offscreen against the real helper.
- `history.qr` with `show` has no agent close for the window. A second show replaces it, and
  closing History closes it.
- Searching History by recognized text, recognition in the background, in-app language downloads,
  translation, handwriting, linear barcodes and PDF417 are out of scope as final decisions.
