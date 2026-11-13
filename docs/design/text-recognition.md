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
   screenshot-first as configured. Escape cancels. The selector's visible hint is the generic
   one that every action gets, so it does not say that the capture copies text. The capture delay, the "Also delay captures started by a
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
a package audit can identify it without a control pipe or any data. `--version` is not supported.

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
  `/sys/devices/system/cpu`, the language data directories, and under its own installation prefix
  only `lib`, `lib64` and `share/putgd` (the AppImage's bundled engines and English data). It may
  write nowhere. TCP is denied from ABI 4. The signal and abstract-socket scopes are set from ABI 6.
  This is what stops a compromised helper from writing `~/.bashrc` or reading `~/.aws/credentials`.
  - **The rules are symlink-safe.** Landlock grants the directory a path resolves to, so every
    path is resolved first. A prefix that is `/`, a home directory or anything above one grants
    nothing, and a `lib` that is a symlink into a home directory grants nothing. A user language
    directory is granted only when it reaches the home directory without a symlink below it. A
    prefix such as `/home/alice/bin` therefore never exposes `/home/alice`.
- **seccomp** on every thread (`SECCOMP_FILTER_FLAG_TSYNC`), a hand-written x86_64 deny list
  with the architecture checked and x32 numbers refused. Landlock does not cover everything, so
  the filter also denies what changes something outside the process:
  - **Killed, the whole process:** `execve`, `execveat`, `ptrace`, `process_vm_readv`,
    `process_vm_writev`, `pidfd_getfd`, the three `io_uring` calls, `bpf`, `perf_event_open`,
    `userfaultfd`, `keyctl`, `unshare`, `setns`, `mount`, `pivot_root`, `chroot`, `fork`, `vfork`,
    and any `clone` that is not a thread or that asks for a new namespace. The helper can start
    threads but no process, and nothing can outlive it.
  - **Fail with `EPERM`:** `socket` and `connect`. The `chmod` and `chown` families, extended
    attributes, file times, `mknod`, `link`, `symlink`, `rename`, `unlink`, `rmdir`, `mkdir` and
    `truncate`. `setpriority`, `ioprio_set`, `kcmp`, `process_madvise` and `process_mrelease`.
    System V IPC and POSIX message queues. `add_key` and `request_key`. The new mount API.
    `tkill` and `pidfd_send_signal`. `clone3` fails with `ENOSYS`, so glibc falls back to `clone`.
  - **Allowed only for the helper itself (pid 0 or its own pid):** `prlimit64`, the `sched_set*`
    calls, `migrate_pages` and `move_pages`. A helper therefore cannot change another process's
    resource limits, such as setting the parent's hard CPU limit to zero.
  - **Signals only to itself:** `kill`, `tgkill`, `rt_sigqueueinfo` and `rt_tgsigqueueinfo` are
    allowed with the helper's own pid as the target and nothing else. Below Landlock ABI 6 nothing
    else would stop a helper from killing its parent.
- Sanitizer builds skip the memory limit, the dumpable setting and the `ptrace` kill, which
  the sanitizers need. Confinement is tested on builds without a sanitizer only.

The architecture is x86_64, the only packaged one. The Landlock ABI of each host the engine
was tried on: Mint 22.3 on m74 (kernel 6.14) 6, Debian 13 on d69 (6.12) 6, Kubuntu 26.04 on
k72 (7.0) 8, Fedora 44 on f44 (7.2) 10, and GitHub's runners 8. The confinement test passes on
m74, d69, k72 and f44 and in CI's `ubuntu:24.04` container as an unprivileged user. It tries a
socket, an `exec`, a child process, a write in the home directory, a read outside the allowed
directories, a read beside a symlinked prefix, a signal to its parent, a `chmod` in the home
directory, a change to its parent's limits and a read of another process's memory, after a
banded, parallel recognition, and all of them fail.

### Bounds and budgets

| Bound | Value |
| --- | --- |
| Input | At most 24 megapixels and at most 32,768 pixels on a side. More is refused with `input_excessive`, which is the editor's own limit. As 8-bit grey that is at most 24 MiB, shared through the memfd. For a PNG the resident also applies the configured image limits |
| Deadline | Fixed by the parent before it prepares the input, 20 seconds at most. At the deadline the parent ends the exact child, with terminate and then kill, and the job is `text_job_timeout`. Cancellation is checked between waits |
| Result | Text at most 1 MiB. At most 20,000 words of at most 256 bytes each. At most 64 codes of at most 8 KiB each. Over a bound the result is cut there and `truncated` is true. The 8 MiB output cap governs all of them together, and the helper cuts candidates first, then words, then codes, then text |
| Languages | At most three per job and at most 64 MiB of data files in all. A language that a data file pulls in by itself counts toward both. Over either limit the job fails with `language_unavailable` |
| Memory | One budget: peak resident size of the whole job of 128 MiB with the shipped English data, which is a 4 MB file. Other languages add the size of their data beyond English, 192 MiB at most. The `RLIMIT_DATA` ceiling comes from the same figure. The resident set size is what acceptance measures |

The helper recognizes on up to four engines at once and starts another engine only while its
resident set stays under the budget, so the budget decides how many cores a job can use. A
worker that has read one task reserves only the working memory its next task needs beyond the
largest it has read, and an extra engine is counted without the language data it shares. There is no smaller memory figure for small regions. The helper is transient, and the memory buys
speed.

**Language data is trimmed in memory.** Some distributions ship a larger model than English
needs. Arch's `tesseract-data-eng` is 23,466,654 bytes, because it also holds the legacy
engine's dictionaries and tables. The helper reads the file and hands Tesseract only the
entries that the LSTM engine reads (the configuration, the ambiguities and parameters, the LSTM
model with its three dictionaries, the unicharset, the recoder and the version), with the
offset table rewritten. The output is identical on all 178 captures, on Arch's model and on the
small LSTM-only English model. A model with no LSTM entry is refused as `language_unavailable`,
not run on the legacy engine at about five times the memory. The memory budget grows by the
trimmed size of an extra language, not the file's size. The receipts still record each data
file's own size and SHA-256.

### Time budget

Recognition has a time budget inside the deadline. It stops 16 seconds after the job starts
and returns what it has recognized so far, in reading order, with `truncated` true. The
notification and the editor then say the text was cut. A job that has text to give never
fails for time. This was decided because recognition time follows the number of characters,
not the number of pixels. It costs roughly a millisecond a character on each core. On m74 one
engine costs 0.77 ms a character and on k72 0.53 ms. A 24-megapixel page of dense text, about
100,000 characters, therefore cannot be read inside the deadline, and no pixel limit changes
that.

- A task is not started when the slowest task so far says it cannot finish.
- Every pass gets Tesseract's own monitor deadline.
- Most of a dense page's time is in Tesseract's layout and line finding, which the monitor
  cannot interrupt.
- The budget is a guarantee. At the deadline, plus one second of grace for a pass that ends on
  its monitor deadline, the main thread closes the hand-over of results under a lock and waits
  only for band copies already in progress. A thread still in an engine's start or in a
  recognition that overran is detached. Everything a worker touches is one heap object that is
  not freed while such a thread lives, and the helper leaves with `_exit` once the result and
  the checkpoint are accepted. A job therefore never waits on a slow engine start past its
  budget. This was shown by injecting a 40 second sleep into an extra engine's start: a region
  finished in 1.02 seconds, a 4K screen in 6.08 seconds, and a dense 24-megapixel page in 15.25
  seconds, truncated, all with exit 0 and a result.

Patrick decided that a result cut at the time budget is returned and marked, rather than
failing (2026-11-13).

### Scale, panels, single pass and bands

The method is Tesseract with page segmentation 6 (the region read as one block of lines),
Otsu thresholding, the dictionary on, no border, and put.gd's own polarity (a dark page, light
text on a dark background, is inverted by put.gd's estimate before Tesseract sees it). Page
segmentation 3, which was the first choice, takes monospaced, aligned lines for table columns.
On the corpus it lost whole terminal and plain-page regions.

- **Screen or region.** An input of at least 4,000,000 pixels is a screen. A screen is cut into
  panels, and its job has a cap of 16,000,000 scaled pixels. Anything smaller is one panel, the
  whole image, with a cap of 8,000,000. The caps were measured on m74: a larger one sent
  24-megapixel inputs into the CPU limit.
- **Panels.** A whole screen is read panel by panel, each at its own scale, in reading order.
  An XY cut over a 4 by 4 pixel grid of text-like activity (grey steps of at least 32 levels)
  separates windows, columns and bars. Long straight runs (window borders, rules, underlines) are
  ignored, so they do not hide a gutter. A cut down the page needs a gutter of at least three
  line heights with activity on both sides, and a gutter that a text line runs straight across is
  not a gutter. A cut across the page is kept only when one of its parts is then cut down. Each
  panel is trimmed to its content and gets its own profile, polarity and scale. Panels are joined
  top to bottom, then left to right, with a new block for each. A region under 4 megapixels is
  never cut into panels, because on a chat window cutting the message list read 2.0 to 2.2 percent
  against 0.4.
- **Preferred scale.** Each panel asks for 24 divided by its text height, rounded up, from 1 to 3
  times. The text height is the median height of its ink row runs, and 2 when unknown.
- **The cap.** When the panels' scaled pixels together (width times height times scale squared,
  summed) exceed the job's cap, every panel's scale is multiplied by the square root of the cap
  over that sum, never below 1. Scaling is done one band at a time from the unscaled input, so the
  whole scaled image never exists.
- **Single pass.** A panel is read in one pass up to 8,000,000 scaled pixels, and Tesseract's own
  layout gives the reading order. A single pass of at least 500,000 scaled pixels is cut into about
  two strips for each engine, between text lines, and the strips are read in parallel.
  - A cut goes in the quietest rows of a blank run (a row counts as blank with up to a 256th of
    its width in ink pixels, so cutting through a descender would read "sample" as "samnle").
  - Where no row is blank, such as a border down a window or lines set without leading, the cut
    is the least-inked row within one line pitch of its share, taken only when that row holds at
    most a quarter of the ink around it. Otherwise that cut is skipped.
  - Strips are at least four line heights, and are joined in page order.
  - Strips are not quite the whole block, because Tesseract judges line statistics per strip. On
    editor code the median error was 1.270 percent for the whole block and 1.481 percent in two
    strips for each engine (m74, Tesseract 5.3.4). Dense terminals read exactly the same.
- **Bands.** A panel above 8,000,000 scaled pixels is cut into horizontal bands of the scaled
  image over twice the number of engines, between 1 and 2 megapixels and at least eight line
  heights, never more than 8 megapixels with overlap included.
  - Each cut goes at the last blank row gap in the final quarter of the band.
  - With no gap there, the cut is made at the limit and the next band starts earlier by
    twice the estimated line height, and by no more than a quarter of the band. Every band
    starts strictly below the one before. A word found in both bands is merged by box overlap and
    keeps the higher confidence, and the seam keeps the engine's logical order, so a right-to-left
    line joins correctly. A band boundary is not a paragraph break.
  - When a band would be shorter than four text lines, which happens for a very wide image, the
    scale drops to 1 times. If it still would be, the input is refused with `input_excessive`.
  - The result says `banded: true` when any band was cut.
- **Two columns.** A tall two-column page of 4 megapixels or more is two panels: the whole left
  column, then the whole right column, each banded within. Below 4 megapixels a two-column page is
  one panel, read as page segmentation 6 reads it. There is no global multi-column order across
  bands.
- **Threads.** Recognition runs on up to four engines, one for each CPU the process may run on
  (its affinity or a container's cpuset, not the machine's online count). The bands and strips of
  every panel go into one pool. Results join in task order, never in the order the tasks finish. A
  screen measures its first task before it shares, and an extra engine starts only while the time
  left covers twice an engine's start plus the slowest task.
- **Codes first.** ZXing-cpp reads the full grey image. Then each decoded code's area is
  filled with the background in every band's copy, so a band never needs a code, and code
  modules never leak into the text. Formats read are QR Code, Micro QR Code, Data Matrix and
  Aztec. The helper uses only `ReadBarcodes(ImageView, ReaderOptions)`, which exists in ZXing-cpp
  2.2.1, 2.3.0 and 3.1.1, and maps the formats by name across versions. Its options are the most
  ZXing offers: try harder, try rotated, try inverted, try downscaled, and up to 255 symbols.

Two limits of the code reader were measured and stay. A rotated Micro QR code at 3 pixels a
module is not read, on ZXing-cpp 2.2.1 or 3.1.1. The same code unrotated at 3 pixels, or rotated
at 5 pixels, is read. A dark code with no quiet zone, such as one that a white page touches, is
not read either, and the same code with a 14 pixel dark frame is. Neither a larger scale, a
different binarizer, an inverted pass nor a pass with the found codes masked reads them.

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
  joins them back into one, and does the same for an email address split at its domain dot.
  Tesseract's word confidence does not separate icons from text on put.gd's own captures: real
  words scored 13 to 35 and every URL near 0, while icon noise scored 0 to 38. So the rules use
  shape, with the confidence floor only for short words.
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
  - Matching runs over the unfiltered candidates, on lines as they appear (words grouped by
    height and ordered left to right), after whitespace and Unicode digits are normalized. Emails,
    phone numbers, card numbers and addresses match across adjacent words and wrapped lines. A
    line fragment joins the line before it only with evidence for that category: a boundary
    character (`@ . - _ +` for an email, `. :` for an address, the numeric separators for a phone or
    card number, a token alphabet run on both sides for a key), or, geometrically, a wrapped column
    whose lines end within 1.5 average character widths of its widest right edge. A preceding word,
    such as "Please email", is not taken in.
  - Recognition returns a long token as several words on one line, with gaps no wider than a word
    space and misread characters, so keys and tokens are matched per line on the words of the
    token alphabet joined without spaces (one misread character allowed in eight). A segment is
    marked when it holds a known prefix at the start of a word (tolerant of the usual confusions and
    of a lost or doubled separator), a JSON web token, a hex run (with `l`, `i`, `O`, `S` and `t`
    confusions) or a base64 run with the shape of random data. A PEM block is everything from a
    BEGIN line to an END line, with dashes read as `~` or a long dash. Shape guards keep ordinary
    interface text from looking like base64: at least 20 percent capitals, at most 40 percent
    digits, under 30 percent of the letters in lowercase runs of five or more, and at most one dot,
    colon or comma in 80 characters. A prefix never fires inside a word ("ghosts", "they").
  - Where a kind's shape fixes the alphabet, the usual confusions are allowed: `O` for `0`, `l` and
    `I` for `1`, `S` for `5` and `B` for `8` inside numbers, and a misread character or two inside
    a token. A number separated by dots alone is a phone number only as 3-3-4 or in pairs, so a
    version such as 26.1112.4660 is not suggested.
  - Categories: `email`, `phone` (international and national forms, at least 7 digits), `ip`
    (IPv4 and IPv6), `card` (13 to 19 digits with spaces or dashes, Luhn-valid) and `key`. Keys and
    tokens means these shapes, listed in the tool's help: AWS access key IDs, GitHub, GitLab, Slack
    and Stripe prefixed tokens, JWTs, PEM private-key blocks, and unbroken hex or base64 runs of 32
    characters or more.
  - A word belongs to one match at most. Where kinds overlap the more specific wins, in the order
    key, email, card, ip, phone.
  - A match becomes Redact marks over its word boxes, one box per line, padded by a quarter of the
    line height above and below and by one and a half characters on each side, clamped to the
    image. Recognition boxes can stop a whole glyph short, which the side padding covers. A mark can
    also take in an ordinary word joined to a token, such as a label with digits ("sha256"), the
    words after a prefixed token up to its usual length, or the rest of a PEM BEGIN or END line.
    Recall wins over that precision. One invocation is one undo step. The result says "Added 3
    suggested redactions" or "No matches found. Text can still be there.". There is no "safe"
    verdict and no upload confirmation.
  - Choosing a category with no result for the current revision starts recognition and waits for
    it, or joins the job already running, under that job's deadline. If a Redact mark or the crop
    changes while it waits, it ends with `edit_stale` and adds nothing.
  - A match is a suggestion. Redact is what removes pixels. If the edit was already submitted or
    discarded when it finishes, it adds nothing, says "Quick redact added nothing: the edit was
    already submitted." and returns `editor_finalizing`.
  - Known limit: recognition damage that matching cannot repair. A 16-digit card number of repeated
    digits, such as `9999990000000006` at 13 px in a monospaced dark font, is counted wrong by
    Tesseract at every scale tried, and a card number read with one digit too many fails the Luhn
    check. Dropping a digit to repair it would make any number pass.
  - Measured through the real editor and helper on k72 (Tesseract 5.5.0, offscreen, not a desktop):
    all 17 items on each of the 24 synthetic pages and 30 of 30 on the hashes page were covered, the
    two clean pages got no mark, and a held-out set (12, 13, 16 and 24 px, 16 pages) covered 303 of 304
    items with 2 extra marks. Quick redact still suggests only. The key and token shapes rest on
    measured thresholds with real but not wide margins, and should stay behind that line.

## History

- **Copy text** is on an image item's inspector and menu, beside Edit, including when a batch has
  several images. It is for PNG and JPEG items and reads the retained local copy, the same rule as Edit again. With no local copy it is
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
  zlib by hand. Save needs an absolute `.png` path and fails on an existing file or a dangling symlink
  unless `overwrite` is true, as the editor's Save a copy does. Without `overwrite` the file is made
  complete in a private file in the same directory and published with `link()`, which fails if the
  destination appeared meanwhile. The window's Save asks again before it replaces a file. A PNG that
  could not be encoded is `save_failed` with "The QR code could not be encoded". The clipboard
  revision is taken before the receipt refresh and checked with the copy ticket, so a newer user copy
  made while the refresh runs is not overwritten.
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

`editor.inspect`'s `text` block has `state` (`idle`, `recognizing`, `completed` or `failed`),
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
`image_unavailable`, `artifact_missing` and `history_not_found` (a source that is not an image,
cannot be read or has no retained copy), `session_locked`, `busy_selecting`, `queue_full`, `link_unavailable` and
`qr_empty` (History), `file_exists` and `save_failed` (QR save), and `ui_unavailable` and
`ui_view_closed` (`history.qr` with `show`). The editor methods also answer `editor_finalizing` (the edit
was submitted or discarded), `editor_busy` (a Quick redact is already waiting), `text_unavailable`
(nothing recognized to copy), `code_binary`, `link_unsupported` and `link_not_opened`.

## Desktops

`text` acquires exactly as `region` does. Each adapter's early paths, which name `region` and
`edit`, learn it. Copy text from a region works on Cinnamon X11, GNOME, Plasma, Budgie and
Hyprland, and on generic graphical X11 sessions (GNOME on X11 and Plasma on X11).

| Desktop | What treats `text` as a region |
| --- | --- |
| Cinnamon X11 | The early crosshair in `X11CoreSourceAdapter`, live and screenshot-first |
| Generic X11 (GNOME on X11, Plasma on X11) | `X11SourceAdapter` reports `capture.text` and maps it to the region path before it freezes the target, with the same live and screenshot-first selection. A text capture does not replace Repeat's rectangle |
| Plasma | The `putgd-region` selector, its cleanup and the transparent-frame fence before ScreenShot2 |
| GNOME | The extension's map from an action to its region preparation, and the adapter's prepared hotkey tickets, which are indexed by action |
| Budgie | As Plasma, with Budgie's presentation-based lock witness |
| Hyprland | The early press catcher in `HyprlandSourceAdapter`, and the bind `putgd:text` |

An adapter that can select a region for text reports `text` in its `capture` capabilities,
and the capabilities `text.sources.region` folds the desktop's region source and the engine's
availability together. The generic Wayland data-control adapter has no region capture, so it reports
`text` unavailable with `region`, and the other sources work. The selector's visible hint is the
generic one for every action. It does not say "Copies text".

**GNOME's extension version.** `text` joins the extension's hotkey list, so the packaged
extension is version 23. As with every extension update, a session that still has version 22
loaded must log out and in once. Until then `GnomeAdapter` refuses region preparation with the
existing "log out once" step (`helper_outdated`), so Copy text from a region reports it, as
Capture region and region recording do, and the `text` hotkey reads `helper_outdated` and is not
counted as a failed restore of a saved hotkey. Recording a hotkey for any action still works with
version 22 loaded. With version 22 loaded, `text.recognize` from `window`, `monitor`, `desktop`,
`clipboard`, `file` and `history`, the editor tools and History's actions still work, because they
need nothing new from the extension. Every existing gate keeps its version rule. Scrolling capture
stays available from version 22.

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

- `PUTGD_OCR` is `AUTO` for developer builds, like `PUTGD_RECORDING`, and `ON` in every package
  build. A package build without the libraries fails. At run time a missing helper or
  missing English data reports `text_helper_missing` or `engine_unavailable` with what to
  install, and the actions stay visible and say why they are off. Recording's Recommends are
  unchanged.
- The distributions ship ZXing-cpp 2.2.1 (Ubuntu 24.04, Mint 22 and Fedora), 2.3.0 (Ubuntu
  26.04 and Debian 13) and 3.1.1 (Arch). The helper is built and tested against both ends,
  Tesseract 5.3.4 with ZXing-cpp 2.2.1 on m74 and Tesseract 5.5.4 with Leptonica 1.87.0 and
  ZXing-cpp 3.1.1 built from source on k72.
- **Arch** ships a larger English model, 23,466,654 bytes, than Debian, Ubuntu and Fedora, which
  ship the 4,113,088-byte LSTM-only `eng.traineddata` that the AppImage carries. put.gd loads
  only the part the LSTM engine uses (see [bounds and budgets](#bounds-and-budgets)), so a job's
  memory is the same order on every distribution. Arch's installed OCR packages are 35,214,737 bytes,
  most of it that model, and the roughly 12 MB native estimate does not hold there.
- **AppImage.** It builds Leptonica (no image-format libraries at all), Tesseract (LSTM only,
  no curl, no archive, no OpenMP, no training tools) and ZXing-cpp (readers only) from pinned
  sources, and carries English `tessdata_fast` pinned by commit and SHA-256. The engines go in
  `usr/lib` and the data in `usr/share/putgd/tessdata`. Tesseract is built with the run path
  `$ORIGIN`, because the helper's own run path does not cover transitive dependencies. Without it,
  a host that has Leptonica installed would bring back every image-format library. The helper
  gets `$ORIGIN/../lib` after deployment, and the audits check every resolved library path from an
  empty environment, so a host copy cannot satisfy an image-owned library name. The pins, recipes
  and commits are in [release](../release/release.md#text-recognition-dependencies-1428).
- The audits (`tools/releases/audit_native.py` and the AppImage audit) require `putgd-ocr`,
  refuse Tesseract, Leptonica or ZXing-cpp in any other product binary, and refuse QtGui, QtSql,
  QtNetwork and QtDBus in the helper. `libgomp` is allowed: Qt Core already brings it into every
  resident through `libb2` on Debian 13, Ubuntu 26.04, Mint 22 and Fedora 44.
- The engines and the data join `bundled-components.json` with SBOM identities, complete-source
  collection, configure recipes and notices, and the exact-asset audit. They do not bypass the
  CVE gate. The QR generator is a component of every package, with its MIT notice. The
  AppImage also carries ZXing-cpp's embedded `libzueci` 1.0.1 (BSD-3-Clause), with its notice.
- Measured on the integration branch at bcb868f9a (CI run 43380438975): the AppImage grew from
  158,030,328 bytes to 164,518,392 bytes, which is 6,488,064 bytes, against a target of under 12 MB.
  A no-Recommends DEB adds 13,537,280 bytes (13,220 KiB, including putgd's own 2,388 KiB) over the
  nightly head. The named OCR engine and data packages install at 11,283,456 bytes on Ubuntu 24.04,
  12,387,251 on Fedora 44 and 35,214,737 on Arch (23,466,654 of it the English model). Fresh-container
  installs of a DEB, an RPM and a pacman package with no optional dependencies each had the engine
  and English data. The stable CVE policy, rehearsed on d69 against all six SBOMs, passed with zero
  flagged findings, and no finding named Tesseract, Leptonica, ZXing-cpp, `libzueci`, `tessdata_fast`
  or `qrcodegen`.

## Performance and memory

| Rule | Check |
| --- | --- |
| No binary but `putgd-ocr` needs libtesseract, libleptonica or libZXing | The audit rule, on every package build |
| The resident never loads them, or libcurl or libarchive, at run time | After a workload of 20 `text.recognize` monitor jobs and 5 driven region actions and a 180 second wait, the resident's `/proc/<pid>/maps` against the idle forbidden-library list plus these names. `libgomp` is not on the list |
| Idle resident RSS and PSS stay under 40 MiB after that workload, with every helper's exit recorded | Gate C on the same resident |
| The agent result cache is bounded | Resident RSS 60 seconds after a `text.recognize` of the largest result, against the same point without it: growth under 8 MiB, and gone after 120 seconds |
| `putgd-ui` startup and its mapped libraries are unchanged | An A/B of main-window and `--edit-image` open time and library list against the nightly head |
| Helper peak RSS within the budget | Measured in every cell, including a dense 24-megapixel input, and with three languages |
| No recognition without an explicit action, and no warm daemon | By design. The helper exits after one job |
| AppImage growth under the size target | Compared with the nightly head |

Spawn, engine start, recognition and copy are timed separately in every receipt, so the time of a
capture shows where it went. **There is no speed bar.** The 1 second bar for a region up to 1
megapixel and the 3 second bar for a full 4K screen are withdrawn. Patrick decided on 2026-11-13 that the
times are recorded, not gated, and that very long text is returned cut at the time budget.

Measured on candidate fe838a950 (CI run 43394391197), 20 monitor jobs and 5 driven region jobs and a
180 second wait, with every helper's exit recorded (25 of 25), none of the forbidden libraries mapped
and the frozen Gate C evaluator met: on m74 the native package left the resident at 10,040 KiB RSS and
5,795 KiB PSS, and the AppImage at 10,816 KiB and 8,967 KiB. The same workload on candidate 8a3294c3c (CI run 43423721988) passed too: on k72 the native package left
the resident at 10,812 KiB RSS and 5,182 KiB PSS and the AppImage at 11,108 and 7,484 KiB, on f44 the
AppImage at 9,968 and 6,111 KiB, and on an Ubuntu GNOME guest the native package at 9,116 and 5,872 KiB
and the AppImage at 9,596 and 7,658 KiB.

TO CONFIRM before merge: the final end-to-end figures (selection to clipboard, helper peak and the
resident's memory after the workload) on the final candidate, commit 357210d2f1cb7b65f77166e34eb535d42e497fd0
(CI run 43448444706), from the final acceptance comment on #1428. Replace the candidate fe838a950
numbers above and in the user guide with them.

### Measurements

Recognition costs roughly a millisecond a character on each core. A paragraph takes about half a
second. A region packed with text or a whole 4K screen takes several seconds. These are
measurements of the helper alone, taken on the engine round four code (#1462), not acceptance of a
put.gd build. Times are `TextJob`'s `totalMs` (spawn to the result read back), so they leave out
the parent's grey conversion and the clipboard. Release builds, up to four engines, peak is the
helper's VmHWM. m74 is an i5-6500T on Mint 22.3 with distribution Tesseract 5.3.4. k72 uses
Tesseract 5.5.4 built from source.

| Input | m74 time and peak | k72 time and peak |
| --- | --- | --- |
| Paragraph (540 by 80) | 0.29 s, 46 MiB | 0.16 s, 36 MiB |
| 1 MP dense region | 0.64 s, 82 MiB | 0.34 s, 73 MiB |
| Dense terminal, 8,739 characters, 1001 by 990 | 2.55 s, 120 MiB | 1.32 s, 79 MiB |
| 4K tiled UI | 6.08 s, 72 MiB | 4.26 s, 58 MiB |
| Dense 4K text | 10.7 s, complete, 101 MiB | 5.7 s, 80 MiB |
| 24 MP tiled UI | 6.3 s, 111 MiB | 3.2 s, 102 MiB |
| Dense 24 MP text | 15.8 s, cut, 116 MiB | 13.1 s, 87 MiB |
| GNOME 4K screen, light | 5.5 s, 89 MiB | 3.4 s, 79 MiB |
| GNOME 4K screen, dark | 6.9 s, 90 MiB | 2.0 s, 81 MiB |

- Every measured input stays inside the 128 MiB budget with English data.
- The dense terminal with a different number of engines on m74: 1 engine 6.72 s (86 MiB), 2
  3.51 s (87 MiB), 3 2.52 s (104 MiB) and 4 2.55 s (120 MiB). Four take no less time than three
  because 8 strips do not divide evenly. With four engines about 3,400 characters are read a
  second on m74 and 6,600 on k72.
- End to end on m74 on candidate fe838a950, before the last engine change: 30 actual selections of
  that dense terminal (8,739 characters each), each independently pasted exactly, took p95 5.7 s
  with the native package and 3.6 s with the AppImage. Whole 4K screens took 5 to 9 s (30 file
  runs on d69: 9.1 s light, 8.0 s dark at p95). A short paragraph took about half a second end to end.
- Engine start takes 115 to 153 milliseconds on m74 and d69, which is the model load.
- With three languages (eng, deu and fra, 2.6 MB beyond English) on m74, measured on the code of #1452:
  a 1 MP region 1.55 s at 116 MiB, a 4K UI page 9.8 s at 90 MiB, a 24 MP UI page 13.1 s, cut, at 111 MiB
  and a dense 24 MP page 8.8 s, cut, at 133 MiB with one engine. English alone peaks at 106 MiB of
  `RLIMIT_DATA`'s data against its 192 MiB limit.
- On Arch's model, before and after the trim, on m74: English 112 to 54 MiB for a paragraph and 137 to
  120 MiB for a dense 24 MP page, and three languages (53,117,539 bytes) 155 to 81 MiB and 180 to 115
  MiB.

### Quality

Quality is scored per engine build and per target class, never pooled. The fixed corpus is 178 images:
174 real Capture region images from published nightly 26.1112.4660 on six desktops (m74 Mint
Cinnamon X11, d69 Debian Plasma, f44 Fedora GNOME, k72 Kubuntu Plasma, Budgie on labwc and
Hyprland on Omarchy), seven classes each, light and dark, at 100 and 125 percent, with two real GNOME 4K
screens and two 24 px pages. Each image has a transcript. The scorer is
`tests/performance/text/score.py`, and `helper_score.py` runs the real helper. Scores use
Unicode NFC, collapsed whitespace and a case-sensitive Levenshtein distance. The bars are a median
character error rate of at most 2 percent for body text of 11 pixels and larger and at most 5 percent
for smaller interface text, and an exact match for at least 90 percent of URLs and email addresses
of 11 pixels and larger. A second language (German) must meet the body bar on one engine build.

Median character error rate through the real helper after the panel work (#1458, #1462), per class:

| Class and size (bar) | Captures | Tesseract 5.3.4, Mint 22 on m74 | Tesseract 5.5.4, built on k72 |
| --- | --- | --- | --- |
| browser-github, body (2%) | 24 | 0.147% | 0.147% |
| browser-local, body (2%) | 24 | 0.000% | 0.000% |
| browser-wikipedia, body (2%) | 24 | 2.573% | 2.573% |
| editor-code, body (2%) | 24 | 1.481% | 1.545% |
| monitor, body (2%) | 26 | 0.000% | 0.000% |
| small-ui, body (2%) | 8 | 0.421% | 0.417% |
| small-ui, small (5%) | 16 | 0.089% | 0.067% |
| terminal, body (2%) | 30 | 0.000% | 0.000% |
| 24 px pages (2%) | 2 | 0.000% | 0.000% |
| URLs exact (90%), emails exact (90%) | | 52 of 52, 52 of 52 | 52 of 52, 52 of 52 |

- **Wikipedia.** All 24 Wikipedia transcripts said "squares in a square grid" where the captured pixels
  say "squares within a square grid". With the transcripts corrected, candidate fe838a950 scored
  1.874% on every engine build but Arch's (1.533%). The 2.573% above is against the old transcripts.
  The citation markers are not the cause: left out of transcript and output, the score barely moved.
- **Whole 4K screens.** The two GNOME 4K captures went from 63 to 64 percent error to 9.2 to 9.3
  percent once screens were read panel by panel. What remains is words run together in small
  navigation text, a dropped `#` and the order of panels inside the Wikipedia window. The monitor
  class median hides these two, which are 2 of its 26 captures.
- **Before the method changed.** Page segmentation 3 with layout blocks, the first method, scored
  terminal 54.4%, browser-local 31.0%, editor-code 13.6% and Wikipedia 2.6%, and read 24 of 48 URLs and
  34 of 48 emails exactly, on the 174 real captures.
- **German.** On 12 synthetic pages (14, 16 and 20 px, DejaVu Sans and Serif, light and dark, with
  umlauts and ß; there is no real German capture), `deu` scored 0.00% and `eng+deu` 0.43%, and English
  alone 5.7%, with `tesseract-ocr-deu` 4.1.0 on m74.
- **English models.** `tessdata_best` meets every bar at about 3.3 times the time and 15.4 MB, and
  put.gd ships `tessdata_fast`.
- **Engine builds.** Candidate fe838a950 scored all six engine builds separately (Mint 22 and
  Ubuntu 24.04, Ubuntu 26.04, Debian 13, Fedora 44, Arch and the AppImage's own). Every class median
  and URL and email rate was inside its bar, with Arch's URL rate 50 of 52 (96.15%).

TO CONFIRM before merge: the per-class error rates for each engine build on the final candidate, which
include the strip and panel changes of #1462 that came after fe838a950, from the final acceptance
comment.

## Known limits

- Recognition can misread. Small, unusual or low-contrast type, superscripts and a busy background
  produce errors. Check what you paste.
- A capture with a lot of text is returned cut at the 16 second budget and marked as cut.
- A rotated Micro QR code at 3 pixels a module, and a code with no quiet zone, are not read.
- Quick redact suggests only, and a number of repeated digits can be miscounted.
- A tall two-column page below 4 megapixels is read as one block, and a banded image has no global
  multi-column order.
- `history.qr` with `show` has no agent close for the window. A second show replaces it, and
  closing History closes it.
- Searching History by recognized text, recognition in the background, in-app language downloads,
  translation, handwriting, linear barcodes and PDF417 are out of scope as final decisions.
