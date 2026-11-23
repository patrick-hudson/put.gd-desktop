# Text recognition and QR codes

put.gd can copy the text out of a screen capture, emoji included, and read QR codes in it.
It can also show a QR code for an uploaded file's link. This is
[#1428](https://github.com/patrick-hudson/put.gd-desktop/issues/1428), with emoji from
[#1748](https://github.com/patrick-hudson/put.gd-desktop/issues/1748). The user's view
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
| The editor's Copy text tool | Select recognized words on the canvas and copy them. List the codes found. Quick redact adds Redact marks over text of five kinds, the account's own name and the user's saved list | The tool (G) at the end of the toolbar, `editor.recognize` |
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
3. **Recognize.** The pixels go to `putgd-ocr`. It reads codes first. Then, at the same time, it
   names the emoji it finds in the colour plane, leaving out any inside a decoded code (see
   [emoji](#emoji)), and recognizes the text in grey copies with the codes' areas filled with the
   background. Last, it puts each emoji in place of what it read there.
4. **Copy.** One plain-text clipboard write, then one desktop notification:

   | Found | Clipboard | Notification |
   | --- | --- | --- |
   | Text | The text | "Copied 214 characters" |
   | Text and codes | The text with each code's content on a line of its own | "Copied 214 characters and 1 code" |
   | Only codes | The content, one per line | "Copied 1 code" |
   | Nothing | Unchanged | "No text found" |
   | The user changed the clipboard first | Unchanged | "Text not copied: the clipboard changed" |
   | The write failed | Unchanged | "Text could not be copied" |

   When an agent asks for the copy, a code put.gd recognises as holding a secret is copied as a
   placeholder such as `[2FA QR code withheld]` (see [the editor](#the-editor)'s Quick redact QR codes,
   #1941 and #1962).

   One character reads "1 character" and one code "1 code". Characters are counted as a person
   counts them (grapheme clusters): an emoji with a skin tone, a flag or a family is one. When the result was cut, the
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
| Input | A sealed memfd. The parent decodes the image and applies the crop and every Redact mark. In strips of 256 rows it flattens alpha onto white, at the image's own pixels whatever its device pixel ratio (a scaled screen's capture has one), and writes each strip twice: as 8-bit grey and as 8-bit RGB, the colour that finds emoji. The memfd holds a 32-byte header (the bytes `PGTJ`, version 2, width, height, stride, colour stride and two zero words, little-endian), the grey plane, then the colour plane. Rows are packed: the header is refused unless the stride is the width and the colour stride three times it. The parent writes the header, then starts the child, so the child loads its libraries and confines itself while the strips are written (#1929). The child's input descriptor is the memfd opened again read-only, so the child can't change what it is about to read. The parent lets the image go once the strips are written, adds the seals `F_SEAL_WRITE`, `F_SEAL_SHRINK`, `F_SEAL_GROW` and `F_SEAL_SEAL`, closes its own copies, and only then sends the bootstrap; the child opens the input once activated after it. If writing or sealing fails, or the job is cancelled first, the parent ends the child without a bootstrap. The mapping never counts in the parent again. The child maps it read-only, and checks the seals and the exact size first. It reads the colour plane row by row and lets each run of rows go as it passes them. It reads rows again only inside a large coloured region (a chat bubble) and around each colour glyph it examines (to find its background, grow its box and match it), and lets those go too, so the colour plane never sits whole in its resident set |
| Descriptors | The child gets the input at descriptor 3 and the output at descriptor 4, set by `dup2` in the child-process modifier. Every other descriptor is closed on `exec`, and the helper closes the rest itself before it does anything. Descriptor 2 stays a capped pipe that the parent discards |
| Environment | Cleared. The child gets only `PUTGD_TEXT_DATA` (the language data directories, in lookup order), `OMP_THREAD_LIMIT=1` and `LC_ALL=C.UTF-8`, plus `PUTGD_TEXT_UNHINGED=1` when [Unhinged mode](#unhinged-mode) is on |
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
- Resource limits: no core file. `RLIMIT_DATA` at 576 MiB, lowered to 480 MiB when the job
  reads English only. `RLIMIT_CPU`, which covers the whole process, at 21 seconds times the engine ceiling counted
  before confinement, with the hard limit one second higher. Both are capped at any hard limit the
  helper inherits, so they can be equal. `RLIMIT_FSIZE` at
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
| Input | At most 24 megapixels and at most 32,768 pixels on a side. More is refused with `input_excessive`, which is the editor's own limit. As 8-bit grey and 8-bit RGB that is a memfd of at most 96,000,032 bytes: the 32-byte header, 24,000,000 bytes of grey and 72,000,000 of colour. For a PNG the resident also applies the configured image limits |
| Deadline | Fixed by the parent before it prepares the input, 20 seconds at most. At the deadline the parent ends the exact child, with terminate and then kill, and the job is `text_job_timeout`. Cancellation is checked between waits |
| Result | Text at most 1 MiB. At most 20,000 words of at most 256 bytes each. At most 64 codes of at most 8 KiB each. Over a bound the result is cut there and `truncated` is true. The 8 MiB output cap governs all of them together, and the helper cuts candidates first, then words, then codes, then text |
| Emoji | At most 512 candidate glyphs are matched. Over all its passes the scan stops taking runs of colour pixels after the row that reaches or exceeds 1,048,576. It stops once 3 seconds have passed since the helper began recognition. That clock starts before the codes are read, and the scan itself starts after them. The limit is checked while it reads rows, groups and matches. What it found stays. A cut scan does not mark the text `truncated` |
| Languages | At most three per job and at most 64 MiB of data files in all. A language that a data file pulls in by itself counts toward both. Over either limit the job fails with `language_unavailable` |
| Memory | One budget: peak resident size of the whole job of 320 MiB with the shipped English data, which is a 4 MB file. Other languages add the trimmed size of their data beyond English, 384 MiB at most. The budget is the threshold the helper checks before it starts another engine. `RLIMIT_DATA`, about one and a half times the budget, is 576 MiB from the start and is lowered to 480 MiB when the job asks for English alone. The resident set size is what acceptance measures |

The helper recognizes on up to eight engines at once and starts another engine only while its
resident set stays under the budget, so the budget decides how many cores a job can use. A
worker that has read one task reserves only the working memory its next task needs beyond the
largest it has read, and an extra engine is counted without the language data it shares. There is no smaller memory figure for small regions. The helper is transient, and the memory buys
speed.

### Unhinged mode

`preferences.unhingedMode` (Settings → Advanced → Performance, off by default, [#1937](https://github.com/patrick-hudson/put.gd-desktop/issues/1937)) raises the concurrency limits put.gd sets on four kinds of work: text recognition, live recording's colour conversion and software encoders, History's poster frames for videos, and History's thumbnails. Each keeps its deadline and output bounds; the memory and CPU-time limits that follow the thread count grow with it, as below. With the switch off, the engine, faster-core, memory and second-read figures elsewhere in this document are the ones that apply.

| Limit | Off | On |
| --- | --- | --- |
| Recognition engines | At most eight, one per CPU the process may use | At most one per CPU the process may use. Affinity and the cgroup `cpu.max` quota still apply. As with the switch off, fewer start when the tasks, the time left or the memory budget allow fewer |
| Faster cores (#1929) | On a mixed machine, the engines keep to the faster CPUs when those hold at least four physical cores and slower CPUs are allowed too | Not applied: every allowed CPU |
| Memory budget | 320 MiB, 384 MiB at most with other languages; `RLIMIT_DATA` 1.5 times the ceiling, lowered to 1.5 times the budget for English-only jobs | 128 MiB plus 64 MiB for each engine the job may start (whether or not it starts them all), never under 320 MiB. Above that floor, the ceiling (the budget plus 64 MiB for other languages) stays within half of `MemAvailable`, and 1.5 times the ceiling within any inherited hard `RLIMIT_DATA`. Both are read before confinement. If `MemAvailable` can't be read, the 320/384 MiB defaults apply. `RLIMIT_DATA` is set and lowered as with the switch off |
| Second-read cap | 25% of the first pass, at least 0.4 s and at most 1 s, ending 1 s before the job's own budget | The same share, floor and margin, with the 1 s cap multiplied by the larger of 1 and the engines the job may start ÷ 4. As with the switch off, the time left can shorten second reads or leave none. Second reads run only for jobs that ask for candidates (the editor's text recognition, Quick redact and the check before uploading), so only those get the extra time |
| Live recording | `x264enc` threads two thirds of the CPUs within 2–8, `openh264enc` and `videoconvert` within 2–4 | Those three elements use every logical CPU, at least two threads |

What stays, and why:
- `OMP_THREAD_LIMIT=1`. Every engine already runs on its own thread, so Tesseract's OpenMP inside each engine oversubscribes the CPUs. On a four-core i5 it more than doubled a dense 4K screen's time and cut every run short at the budget.
- Bands and strips are cut as for at most four engines, so with four or more engines the tasks are the same at any count.
- The emoji scan's 3 s and Quick redact's 2 s matcher bound. They decide how thorough the scan is and stop a runaway pattern; they don't limit CPUs.

The posters and thumbnails are in [History poster frames](recording/history-posters.md#history-poster-frames): posters decode on more threads without the extra nice +10, and History decodes stored thumbnails in parallel, up to one a logical CPU and as many as half of `MemAvailable` holds at 128 MiB each, but always at least one (one when it can't be read).

The parent passes the switch in the helper's cleared environment as `PUTGD_TEXT_UNHINGED=1`, because the engines, the memory budget and the limits are fixed before confinement. On aurora-amd (12 threads), a dense 4K screen took 1.69 s with the switch on against 1.98 s off, with peak resident memory of 371 MiB against 271. On a four-core i5 the times were the same. The copied text was identical on all 277 measured inputs ([#1948](https://github.com/patrick-hudson/put.gd-desktop/pull/1948)).

**Why 320 MiB (#1808).** The budget was 128 MiB until 2026-11-21. A whole screen's first task
already used that up, so a GNOME 4K screen ran all 43 of its tasks on one engine and a 1080p
screen on two of four. The #1805 research measured the corpus in the in-process benchmark with
the memory gate turned off and everything else as shipped. On k72 the screens of 4 megapixels
or more took 2.75 s on average instead of 5.18 s (four screens), and the 1080p screens 0.94 s
instead of 1.15 s (median of 22). In those runs the text was byte-identical to the gated
helper's on all 176 captures on aurora-amd and 50 on k72, and the largest peak, a dense
24-megapixel page with four engines, was 258 MiB. That page also ran normally with
`RLIMIT_DATA` at 320 MiB. Through the packaged helper of #1809, which first spends about a second
of a 4K job on one thread for the emoji scan and page analysis, the gain is smaller. On k72 the
CI DEB ran through its own resident and packaged helper. The baseline was today's helper built
from the #1805 research tree, linked to the same distro Tesseract and driven by `test_ocr_run`. The screens of 4 megapixels or more went from
4.07 s on average to 3.14 s and 3.06 s in two runs. The dense page read 66,138 to 70,602
characters before the time budget instead of 48,546, and the highest peak was 172 MiB. On
aurora-amd the candidate AppImage was compared with the nightly AppImage. Its bundled engine
peaked at about 90 MiB on the corpus, so the old budget never held it back there, and only the
dense page got faster: 15.35 s to 12.8 s. The text was
byte-identical on all 176 captures on both hosts
([evidence](https://github.com/patrick-hudson/put.gd-evidence/tree/0e6051e3c427dd671ea776b25aae7dde215b1805/records/2026-11-21-issue-1808-ocr-memory-budget)). Patrick allowed the larger figure
because the helper is short-lived: it holds the memory only while it recognizes and exits after
the job, never living in the resident, and `RLIMIT_DATA` still bounds a runaway.

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
engine costs 0.77 ms a character and on k72 0.53 ms. A 24-megapixel page of dense text, with
100,000 characters or more, therefore cannot be read on one engine inside the deadline, and no
pixel limit changes that. Four engines still leave the densest test page cut.

The figure of a millisecond a character is for one engine. The helper can use up to eight, as
many as the memory budget lets it start, so how much of a dense page it reads before the time
budget depends on that budget too. Under the old 128 MiB budget a 4K screen ran on one engine
and took as long as `--threads 1`. In the #1805 benchmark on aurora-amd, the dense 24-megapixel
page gave 88,112 characters in 16 seconds with the 128 MiB gate and 180,776 with the gate
turned off (#1808).

- A task is not started when the slowest task so far says it cannot finish.
- Every pass gets Tesseract's own monitor deadline.
- Most of a dense page's time is in Tesseract's layout and line finding, which the monitor
  cannot interrupt.
- Quick redact's second reads have an allowance of their own inside this budget, ending at
  least a second before it, and wait at most 250 ms for a read ending on its monitor deadline
  (see "One budget for every second read").
- The budget is a guarantee. At the deadline, plus one second of first-pass grace for a pass that ends on
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

Otsu's threshold is one for the whole region, so a line of faint ink (comment grey on a dark
editor) disappears whenever brighter text pulls the threshold above it (#1894). The page profile
also counts faint edge ink per row: pixels at least 12 levels from the background and 8 from the
pixel before them on the background's side of the threshold. A run of such rows with no ink at
the threshold, half to three line heights tall (at most eight runs), is read once more with its
band's contrast stretched, darkest pixel to black and paper to white. Only a job that asks for
candidates (Quick redact and the check before uploading) does this, as one of the second reads
described below, inside their budget; the words join the candidates after the copied text is made, so Copy text is
unchanged, and a word where one was already read is left out. When neither this test nor the
unread-rows test below finds a band, these tests add no extra read.

On a whole screen of 4 MP or more, read in panels, a panel is trimmed to its text, and a faint
line has no grey step of 32 levels, so it can sit outside its panel (#1912). For the faint lines,
each panel first grows outward one row or column at a time. A side moves out while at least a
quarter of the new row or column, across the current span, is within 6 grey levels of the panel's
background. On a span of 8 px or more, each quarter of it must also hold such pixels, at least
1/32 of the span. The new row or column must also meet no other panel and no area already grown.
A desktop clearly darker than a dark editor fails the test, so growth usually stops at the
window's edge, but it is judged from pixel values: it crosses a window boundary wherever the new
row or column passes the same checks, as a neighbouring area within 6 levels of the shade does.
A faint line below a window's last bright line, or running past its text, can then fall inside
the grown area and go through the faint-row test above. Over a whole window, Otsu's threshold can
fall below a long faint line beside short bright text, which then counts as ink, not as faint; so
in a grown area, rows outside the rows its panel was read in whose ink and faint-ink pixels
together number at least four (or one per 256 pixels of width, when that is more) form runs, and a
run half a line to three lines tall is read again the same way. Its band adds up to two rows on
each side, never into the rows the panel was read in, and is skipped only when one faint band
covers the whole padded band. Faint and unread runs together are at most eight an area; the rest
are not read, and the result is not marked truncated for them (#1929: on a grainy dark corpus image
that took 6.4 s of faint reads and then sent the check before uploading to "couldn't finish").
When the image is
split, a window that holds only faint text may have no panel of its own to grow from: panels come
from grey steps of 32 levels, which other content in the window can supply. A large image whose layout gives no split
is one panel, the whole image, as is anything under 4 MP.

One misread character breaks a key or token for Quick redact. A job that asks for candidates
also reads a second time the lines holding an **uncertain token** (#1911): a word that Tesseract
scored below 70, with letters and digits both, of at least 12 characters once quotes (including the
backtick), brackets and `, ; : .` are trimmed from both ends. Each such line is read from a
contrast copy built on 16 px tiles: each tile's background is the median grey level of all pixels
in the window of tiles around it (up to 5 by 5, fewer at an edge), every pixel is replaced by its
distance from that background, and the distances are stretched so the local 98th percentile (at
least 32 levels) is black, on white. One copy reads dark and light pages, low contrast and coloured backgrounds alike. Bands
are at most four times the height of their tallest uncertain word. A band is read only across its
words' run of text: their columns grown by four times the tallest word's height, then outward,
whole columns at a time, to the first gap of blank columns at least 0.6 times that height wide
(rounded down, at least two). A blank column has no pixel 12 or more grey levels from the
panel's background in the band's rows. With no such gap the band runs to the panel's edge. So a
full-width row of a 4K terminal is not read across the blank screen beside it. A main read cut
short skips it. Its words join the candidates after the copied text is made; Copy text is
unchanged, and a page with no uncertain token does no extra work.

**One budget for every second read (#1929).** The faint-line, unread-row and uncertain-token reads
go in one queue: the faint and unread bands first, then the token bands least confident first
across all panels.
- **Engines.** The first engine takes from it, and so does every extra engine that started and
  whose first-pass thread is done with it (one still starting or overrunning sits out), as many
  as there are bands queued. While time and the memory budget allow, more start, up to the job's
  threads or the bands queued, whichever is fewer. The memory check counts
  each first-pass engine still starting as one engine's cost, and reserves each new engine its
  cost and 20 bytes a scaled pixel of the largest second-read band. Each engine reads on a thread
  of its own, the first one too, so the job's wait bounds them all.
- **Time.** The allowance is a quarter of the first pass's time, at least 400 ms and at most
  1 s, less whatever would bring it within 1 s of the job's own budget (it can be removed). It is
  worked out once the second-read bands are queued, from the time since the first pass began
  (building the queue included), and its clock starts once each band's size has been counted. A read whose band would not finish at the slowest pace seen so far is not
  started, and one cut short adds nothing. After the allowance, the job waits at most 250 ms more
  for a read ending on its monitor deadline, then closes the handover. A reader still running is
  left as the first pass's late workers are.
- **Never truncated by them.** A second read that is skipped, cut short or left out never sets
  `truncated`. Their words follow the first read's candidates and are left out, not cut, where
  the result's bounds would cut them: a word over 256 bytes, the 20,000-word cap, or the 8 MiB
  output. They join only a result those bounds left whole.

`truncated` still means what it did: the first pass did not finish, or the first pass's own
result was cut at a size bound. Until #1929 the token read ran on one engine for as long as the first pass
took, which doubled a dense 4K check on a 4-core i5 (5.2 s to 10.3 s). On the 978-image corpus
test split the one budget covered 1,351 secrets against 1,347 and flagged 642 decoys against
652, with the copied text identical.

How this was chosen, on Patrick's 10k secret corpus (test split, 978 images):
- Recognition misses were not specific to dark themes (27% of secrets on dark themes, 28% on
  light). No preprocessing beat the first pass when it replaced it: local inversion, the local
  contrast copy above, colour-channel distance and 2x upscaling covered 62 to 64% of secrets,
  against 64% for the first pass, and Sauvola thresholds 50%.
- A second read **added** to the first is what helps: a second, different reading is a second
  chance at a long random string. Keeping only the more confident reading of a line gained
  nothing. Rereading only the token's own area, not its line, gave half the gain.
- With the matcher as merged, secrets fully covered went from 64.9% to 69.7% and decoys covered
  from 34.7% to 39.6%; on the x3 PDF, 285 to 293 of 359 items at 96 dpi and 297 to 299 at
  144 dpi, with decoys unchanged. A filter dropping second-read matches unlike the first read
  removed secrets and decoys at about the same rate, so there is none: a missed secret is the
  worse failure.
- Cost, one job at a time on aurora: none measurable on the 176 ordinary captures (the test held
  on 9); on a 98-image sample of the corpus, where it ran on 91, 7 ms at p50 and 303 ms at p95;
  on a 4K dark terminal whose every line held tokens, 2.06 s became 4.06 s, held by the cap.

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
  two strips for each engine (at most four counted) when more than one engine may run. The cuts go
  between text lines, the strips can be read in parallel, and a panel with no usable cut stays
  whole.
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
  image over twice the number of engines (at most four counted), between 1 and 2 megapixels and
  at least eight line heights, never more than 8 megapixels with overlap included.
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
- **Threads.** Recognition may run up to eight engines. Fewer start when the memory budget, the
  number of tasks or the time left allows fewer.
  - The ceiling is the CPUs the process may use: its affinity, or a container's cpuset, not the
    machine's online count. If the affinity can't be read, it is the online count. That is
    lowered to the tightest cgroup v2 `cpu.max` quota readable on the process's cgroup or a
    visible parent, rounded up.
  - The helper counts before confinement, because Landlock then hides `/sys/fs/cgroup`.
  - Bands and strips are cut as for at most four engines. So with four to eight engines a job's
    tasks are the same, and so is its text, unless the time budget cuts the job short. More
    engines share the same tasks. When strips followed the engine count, six engines read 47 of
    the 176 corpus captures differently.
  - A task's memory growth is not measured while an extra engine is starting. With eight engines
    on a GNOME 4K screen, a task measured beside seven engine starts showed 79 MiB, and every
    extra worker then found no room and quit.
  - On aurora-amd (Ryzen 5 PRO 8500GE, 6 cores and 12 threads), the AppImage's own resident and
    helper, with eight engines against the four of #1809's CI AppImage:

    | | 4 engines | 8 engines |
    | --- | --- | --- |
    | 176-capture corpus | 86.1 s | 78.5 s |
    | 4 MP+ screens, mean | 1.31 s | 1.16 s |
    | Dense 24 MP page | 12.8 s | 9.0 s |
    | Peak, dense page | 139 MiB | 204 MiB |

    The text was identical on all 176 captures. On k72's four cores nothing changed. The record
    is [put.gd-evidence `records/2026-11-22-issue-1812-ocr-engine-count`](https://github.com/patrick-hudson/put.gd-evidence/tree/03ec9a20f11f719e5ddac17014e32073bf79c6dd/records/2026-11-22-issue-1812-ocr-engine-count).
  - The bands and strips of every panel go into one pool. Results join in task order, never in
    the order the tasks finish. Extra engines start at once, and only while the time left covers
    twice an engine's start plus the slowest task. A banded page or a screen of panels reserves
    each one its cost and 20 bytes a scaled pixel of its largest task (a dense band grew by 15
    to 20; the second reads' new engines are reserved the same way for their largest band),
    and once its first task is done starts more as that task's cost allows. Until #1929 such a
    page read its first task alone first, which left three of four cores idle for 1.4 s of a
    dense 4K terminal's 4.6 s first pass on k72.
  - The first engine starts on a thread of its own while the codes are read and the page is
    analysed. Its cost is the rise of the process's private pages over its start, never under
    16 MiB: the code and emoji scans beside it fault in the input's mapping, and counting those
    took a dense 4K terminal on m74 from 5.1 s to 6.2 s.
  - **Faster cores.** On a machine of mixed cores the helper keeps its engines to the faster
    CPUs among those it may use. Their count becomes a further ceiling on the engines; the
    eight-engine cap, the cgroup quota and the job's other limits still apply. This happens only
    when those faster CPUs hold at least four physical cores (distinct package and core ids from
    sysfs topology) and slower CPUs are allowed too. Otherwise, including when topology is
    missing, the helper keeps the affinity it was given, as on a uniform machine. It reads sysfs
    before confinement:
    - Intel's `cpu_core` CPUs, when `cpu_atom` lists CPUs too.
    - Otherwise each CPU's `acpi_cppc/highest_perf`. Groups are cut at a step of a sixth or
      more, and every CPU above the slowest group counts as faster. A complete set of values
      with no such step means a uniform machine. AMD's ranking of alike cores differs by
      smaller steps.
    - `cpufreq/cpuinfo_max_freq`, the same way, only when `highest_perf` is missing or invalid
      for some CPU.

    aurora-amd's Ryzen 5 PRO 8500GE is a mixed machine whose faster kind is only two physical
    cores. By `highest_perf`, its two Zen 4 cores (four logical CPUs) read 196 and its four
    Zen 4c cores 144, all with the same maximum frequency. With two faster physical cores it keeps
    every CPU, as measured on a dense 4K terminal: 2.18 s on the four Zen 4 CPUs, 2.06 s on four Zen 4c cores,
    1.94 to 2.05 s on any four CPUs, and 1.88 s on eight.
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

One job can give two outputs:

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
  reads are the filtered words of the copied text, not these. Only a candidate from the
  uncertain tokens' second read carries `"second": true`; faint-line candidates and the copied
  `words` never do.

### Emoji

Tesseract's language models hold no emoji, so it reads one as a stray symbol, a letter
glued to the word beside it, or nothing: "🎉" became `&`, "I ❤️" became `IW` and "😂😂"
became `6S` ([#1748](https://github.com/patrick-hudson/put.gd-desktop/issues/1748)). The
helper finds emoji in the colour plane and names them itself, from templates built into it.

- **The templates.** The fully-qualified emoji of Unicode Emoji 16.0 that a font draws as one
  glyph: all 3,781 as Noto Color Emoji 2.051 draws them, and 3,417 as Twemoji Mozilla 0.7.0
  does. Noto Color Emoji is the
  emoji font of every supported distribution, so it is what GTK, Qt, Chromium, terminals and the
  distributions' Firefox packages draw. Mozilla's own Firefox builds draw emoji with Twemoji
  Mozilla, the same file in Firefox ESR 153 and Firefox 158 and 159, and Discord shows Twemoji
  pictures. Noto redraws some emoji in each release, so the emoji that Noto 2.047 (Ubuntu 24.04,
  Linux Mint 22) and 2.057 (Fedora 44 and 45, Aurora) draw differently from 2.051 are templates
  too (445 and 291; 2.057 drew a new 🚀). Each later Noto set keeps only drawings that differ by
  more than 0.03 from every drawing of the same emoji kept in the Noto sets before it. Qt draws a COLRv1 font such as 2.057 itself and leaves
  out some of its shading (flags and 🍳 come out flat), so the 261 emoji Qt 6.11 draws
  differently are templates as well: a Plasma desktop on Fedora or Aurora shows those. Each
  template is the glyph's ink box stretched to 20 by 20 and to 10 by 10 pixels, premultiplied
  RGBA, with its aspect and its share of colour pixels.
  [`tools/text/emoji_templates.py`](../../tools/text/emoji_templates.py) makes them with Pango
  and cairo, from Qt's drawings by [`tools/text/emoji_draw_qt`](../../tools/text/emoji_draw_qt)
  for the Qt ones, and the file, its inputs and their checksums are in
  [`src/ocr/emoji/SOURCE.md`](../../src/ocr/emoji/SOURCE.md). The 8,195 templates' 16,576,768
  bytes are part of the helper's read-only data. Nothing is generated at build time and no font
  ships.
- **Plain shapes are never named.** Emoji that are plain shapes (coloured circles and squares,
  keycaps, arithmetic signs, arrows, letters or signs in a square) are templates too, 315 of
  them, but a glyph they fit best is left as Tesseract read it. On a screen those shapes are status
  dots, record buttons, badges and coloured letters far more often than emoji, and matching
  them still keeps a red dot from passing for a stop sign.
- **Finding a glyph.** A pixel whose red, green and blue differ by 32 levels or more is colour.
  It belongs to a colour core only inside a 2 by 2 block of colour pixels, so the one-pixel
  colour fringes of subpixel-rendered text never do. Cores are joined row by row, and cores
  inside one glyph (a face and its tears, a cone and its confetti) become one: a smaller core
  joins a larger one whose box, grown by 15 percent of its side, holds it. A glyph's background
  is the commonest colour, to 4 bits a channel, within its own size around it. When that colour
  is under 35 percent of those pixels the glyph is not on a flat background (a photo, a
  gradient) and is skipped. A core larger than 160 pixels is a coloured region, not a glyph:
  when three quarters of it is one colour (a chat bubble, a coloured panel), it is read again
  with colour meaning colour that is not the region's own, so the emoji in a blue or green
  bubble are found. The box then grows to the ink touching it, on each edge by at most 30
  percent of the core's larger side (whole pixels) plus 2, so the parts with no colour
  (outlines, white eyes) join it. The grown box's larger side must be 10 to 160 pixels and its
  shorter side at least 4.
- **Naming it.** Templates whose share of colour pixels is within 0.35 of the glyph's are
  compared with the glyph stretched to 10 by 10, each laid on the glyph's background, by summed
  byte differences. A template's share is counted as it shows on that background, among the
  pixels that differ from it, so a white field on a white page does not count (England's flag
  is its red cross there, as the glyph is). Glyphs are matched grouped by background, so the
  templates are laid on each background once. The closest 64 are compared at 20 by 20, and the closest 24 are drawn at the
  glyph's own size and compared pixel by pixel: the mean colour difference over the pixels
  either of them inks; a glyph larger than the templates' 20 pixels is compared at their size
  instead, since drawing a 20-pixel template larger only blurs it. Each is tried in the box the
  glyph grew to, when that box has about the
  template's shape, and in the boxes the template's own shape gives from the box's height or
  width, anchored at either side or centred. A part of a glyph across a blank row or column
  (confetti, tears) widens the glyph's box when it joins it. That trims a letter glued to the glyph and restores
  a white part on a white page. The closest four are then tried one pixel wider and narrower on
  each edge. A box much wider than tall is also shortlisted as a square at each end, and what is
  left on either side of a named emoji is matched again when it could hold another, for emoji
  that touch, up to four in one glyph.
- **When it is named.** The best template's difference must be at most 0.09, and at most a
  quarter of that template's contrast with the background, its mean colour distance from the
  background over its opaque pixels. A dark template on a dark page, or a pale one on white,
  stands out too little to tell from a faint coloured letter: without this rule, on the corpus,
  every glyph that was named but was no emoji was such a fit, a new moon or a dark-skinned face matched to a dim
  coloured letter of code. A glyph whose closest template is more than 0.18 away even
  stretched is not drawn at its own size, which spares most coloured letters the costly step.
- **Where it goes.** After recognition, the characters the glyph covers (at least 40 percent of
  each, by Tesseract's character boxes) leave their words. A word left empty goes, and one cut
  in two becomes two, each as sure as its own characters were read. The emoji becomes a word of the nearest line that shares 40 percent of its
  height, within three glyph widths, in that line's own order (right to left when Tesseract read
  the paragraph that way), or of a line of its own after the last line above it. With a gap of
  under a quarter of its height to a neighbour it joins that neighbour with no space
  ("Hello👋"): a glued emoji's gap was a median 0.15 of its height, and one after a space 0.25 or
  more for 247 of 248. A join counts only while that neighbour is copied too. A line qualifies
  when its height and the glyph's overlap by 40 percent of the shorter of the two. Its height is
  the one recognition gave it (for a line of emoji alone, its first emoji's), so an emoji placed on a line does not stretch it towards
  the line below. Across, a line of text is first measured by its recognized box, and a line of
  emoji alone by its emoji, so a row of them stays one line; only when no line qualifies that
  way is a line of text measured with its emoji too. A colour glyph inside a decoded code (a logo in a
  QR code) is not named: the code is copied as its own line. An
  emoji is always copied, whatever the copy filter would make of its pixels. Agents get it as a
  word with its glyph's box, the editor shows and selects it as a word, and Quick redact sees
  it. The editor's Copy selection joins words with spaces, so a glued emoji gets one there;
  Copy all text keeps the join.
- **When it runs.** On its own thread, from just after the codes are read, alongside page
  analysis, the engines' start and recognition (#1815). The job joins it before the emoji are
  placed.
  - Before #1815 it ran first, on one core. Overlapping it made the 176-capture corpus 11% faster
    and 4 MP+ screens 15% faster on aurora-amd (6 cores), and the corpus 8% faster on k72 (4
    cores). The text was identical on all 176 captures and on the 26 #1748 emoji crops. The
    record is [put.gd-evidence `records/2026-11-22-issue-1815-emoji-scan-overlap`](https://github.com/patrick-hudson/put.gd-evidence/tree/1d1dc859d798c1a16789475e4fc1f7cec9a4e659/records/2026-11-22-issue-1815-emoji-scan-overlap).
  - The colour rows the scan has passed, and the rows read again around each glyph once it is
    done, are given back as it goes. The template pages it read are given back when the scan
    ends. Its large working buffers come
    straight from the kernel, so a freed one does not raise glibc's mmap threshold for
    Tesseract's buffers.
  - No growth sample is taken from a task that overlaps the scan, so the scan's pages never count
    as a task's growth. The gate still checks the helper's whole resident set, scan included,
    before it starts extra engines and before an extra worker takes a task that needs more
    memory. The first engine and its worker are never gated.
  - If the system refuses the thread, the scan runs first, inline, as before.
  - The scan's 3-second limit is unchanged and still counts from the start of recognition. The
    scan now shares the CPU with the engines, so on a very busy screen it may get through less
    of the page before that limit. A cut scan keeps what it found and never cuts the text.
  - `recognizeMs` is code reading plus text recognition, plus the time the job waited for the
    scan at the join (or the whole scan, when it ran inline).
- **What it cannot see.** An emoji with no colour (👀, 💀, 🖤, 🤍 and 💬 as Noto draws them)
  has no colour core, so it is not found and Tesseract's reading of it
  stays, usually a symbol the copy filter drops. Apps that draw their own emoji pictures in
  another design (Slack, Signal and Telegram Desktop draw Apple-like ones) are named only where
  their pictures happen to look like Noto's or Twemoji's. Skin tones next to each other and the
  man, woman and person forms of one emoji differ by a few pixels at text size and are
  sometimes swapped. Small flags can be missed: 🇬🇧 at the tested editors' default size was
  missed on all five lab desktops. Noto's ☕, a white cup, on a white page at 150% zoom fits
  within 0.09 but stands out too little for the contrast rule. Both are left as Tesseract read
  them.
- **Desktop acceptance** (round 2, the five lab desktops, real region selector and clipboard):
  185 of 195 expected emoji came through in Firefox at 100% and 150% and in each desktop's
  editor, none named wrong. The ten missed were 🇬🇧 at the editors' default size and ☕ at 150%
  (and at 100% in Aurora's Flathub Firefox). Plain text copied exactly as nightly did. The
  helper's peak was 59 to 77 MiB. Where a word next to an emoji was lost, nightly lost it too: Tesseract read
  "so" with no confidence after 😂😂, and Aurora's "I" as "|", which the copy filter drops.

## Untrusted content

Recognized text and code content come from whatever was on the screen. They are screen data
from an untrusted source and never instructions.

- Before any copy, display or agent reply, the text goes through one filter
  (`textjob::sanitizeText` and `sanitizeCodeContent`). The text is put in NFC. C0 controls
  except tab and newline are removed, as are C1 controls and DEL. U+2028 and U+2029 become
  newlines. Every format character (Unicode category Cf: the bidirectional override,
  embedding and isolate characters, and the zero-width, invisible and tag characters) is
  removed. Recognized text keeps U+200C and U+200D, which Persian and Indic scripts need, and
  the tags of exactly three flags, the tag sequences of England, Scotland and Wales (#1748). Any
  other tag character goes, since tags can carry hidden text. Code content keeps no format
  character. The parent applies the filter again to what it reads from the
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
  addresses, Card numbers, Keys and tokens, My username and name, My redaction list, All of
  these, and Remove suggestions) with "Suggestions only. Recognition can miss text." always
  beside it.
  - The second read of uncertain tokens is matched apart from the first. Quick redact matches
    the first read, then a page where each second-read word replaces the first-read words whose
    columns it overlaps and whose vertical centre lies within its height. Only matches holding a
    second-read word are added, and one already covered by a same-kind first-read match, each of
    its words inside one of that match's words within 2 px, counts once. Side by side on one row,
    the two readings of a line would be read as one; read alone, a PEM public key's line loses its
    header and was taken for a key. A mark's margins look only at the words its match was found
    among.
  - Matching runs over the unfiltered candidates, on lines as they appear (words grouped by
    height and ordered left to right), after whitespace and Unicode digits are normalized. Emails,
    phone numbers, card numbers and addresses match across adjacent words and wrapped lines. A
    line fragment joins the line before it only with evidence for that category: a boundary
    character (`@ . - _ +` for an email, `. :` for an address, the numeric separators for a phone or
    card number, a token alphabet run on both sides for a key), or, geometrically, a wrapped column
    whose lines end within 1.5 average character widths of its widest right edge. A preceding word,
    such as "Please email", is not taken in. Recognition can split an address in its local part
    (`jane.` or `jane`, then `doe@example.com`; also `jane+`, or more than two pieces). Words of
    address characters join the next word on their visual line when their boxes overlap
    horizontally, as a run that goes on to the "@" and whose first word starts with a letter or
    digit (a middle piece may be punctuation alone, such as `+`), so the whole address is one
    match. Boxes that only touch, or have any gap between them, are not joined, so "us." after
    a sentence stays out, and so does a split the recognizer left a gap in. A word that a key, card,
    IP or phone match also wants stays with that match. Names and list entries are matched after
    addresses and don't take a word the address has joined (#1840).
  - Recognition returns a long token as several words on one line, with gaps no wider than a word
    space and misread characters, so keys and tokens are matched per line on the words of the
    token alphabet joined without spaces (one misread character allowed in eight, and one in any shorter word; quotes around a
    word don't count when they enclose 8 characters or more with a digit or a capital, #1820). A segment is
    marked when it holds a known prefix at the start of a word, or right after one of `=`, `:`, `/`,
    `@`, a quote (straight or curly), a guillemet, `(`, `[` or a backtick inside one when its body
    has both a digit and a capital (`KEY_ID=AKIA…`, `"AKIA…"`, `https://ghp_…@host`; an ordinary
    lowercase URL slug has no capital, but a slug that does have both can still match, #1820), tolerant of the usual confusions (for the prefix patterns other than a JSON web token's, a `/` between two letters or digits counts as a letter, as recognition reads a `J` at small sizes, while the separator check reads the text as recognized, #1839) and of a lost or doubled separator, a JSON web token, a hex run (with `l`, `i`, `O`, `S` and `t`
    confusions) or a base64 run with the shape of random data. A PEM block is everything from a
    BEGIN line to an END line, with dashes read as `~` or a long dash; with its END line found within
    100 lines, every line between is taken whatever recognition made of it (#1894). A segment ends
    at a gap of more than three characters between two words, so a table's label cell is not part
    of its value (#1895). A line that starts a new assignment (`NAME=` or `name:`, after an
    editor's line number or not) is no token's wrapped remainder (#1894). Shape guards keep ordinary
    interface text from looking like base64: at least 20 percent capitals, at most 40 percent
    digits, under 30 percent of the letters in lowercase runs of five or more, and at most one dot,
    colon or comma in 80 characters. A prefix never fires inside a word otherwise ("ghosts", "they"). A JSON web token is
    also found when a text caret garbles its start (#1875). A caret just left of it reads as `l`, `I`, `|`,
    `1`, `!`, `j`, `J`, `[` or `{` joined before a whole `eyJ` (`leyJ…` in xed), or merges with the `e`
    into `kyJ` (a Firefox text field) or `byJ` (Kate); such a reading counts only when the word holding it
    has at least two more characters of the token and the body has a digit and a capital. When the header
    itself is misread, the payload finds the token: an `eyJ` (with its `J` misread as usual) right after a
    dot, or a comma read for one: inside a word, again with a digit and a capital in its body, or starting
    the word after one that ends in that dot or comma. Both readings, like any JWT, need a body of at least 24
    characters with a dot or comma in it, and their part up to the next dot or comma must look like base64
    of JSON: 12 characters or more, at least a fifth capitals, and at most 30 percent in lowercase runs of
    five or more. That rejects a dotted file name such as `archive.eyJanuary2026.pdf` or prose such as
    `leyJanuary 2026.`. When the first mark in a run of token words is a token found by its payload and the first word
    kept does not itself start with a head, up to three short words just before it are kept when they join
    (at most nine characters) into a head as read: `ley` `Jhb`, `ky`, `Jew`, `ley)`. A JWT wrapped over
    lines follows the usual wrap checks; besides a next line that starts right after one of its dots or
    commas while it has read at most two, a next line whose own match is a token found by its payload continues a JWT that has read fewer
    than two dots or commas on its first line, counted from its prefix and including the header's dot of a
    token found by its payload (a wrap inside the header), or a token-shaped run above. A token
    collapsed into repeated letters is a token shape too: a segment of 24 or more letters or digits whose
    three commonest characters, ignoring case, are at least 85 percent of them and the commonest alone half.
    Prose never repeats so few letters for so long. A run whose commonest character is `x` or `0`
    is a placeholder, not a token (#1895).
  - Developer secrets with no prefix of their own (#1894), matched on the line's words joined as
    above but allowing one misread character in four, since passwords use symbols freely:
    - a URL of a known scheme (database, cache, queue, mail, file transfer, LDAP, HTTP and others)
      whose user is followed by a password rather than a port, `postgres://user:password@…`, also
      with the `@` lost; from its scheme to the end of the segment;
    - the value of a secret-named setting: a name whose parts (split at anything but a letter or
      digit and at a lowercase-to-capital change) include TOKEN, SECRET, PASSWORD, PASSWD, PWD,
      PASS, PASSPHRASE, CREDENTIAL(S), DSN or APIKEY, or API, ACCESS, SECRET, PRIVATE, SIGNING,
      ENCRYPTION, MASTER, ACCOUNT or APP before KEY, CLIENT or APP before SECRET, or AUTH before
      TOKEN, or AUTHORIZATION, unless its last part names a setting about the secret (TTL, LENGTH,
      MIN, MAX, TIMEOUT, FILE, PATH, URL and the like) or a part is PUBLIC or PUBLISHABLE. In one
      word (`DB_PASSWORD=…`, JSON, CSV or a query string) the word is covered; with the value in
      the next word (`password:` `…`, or a lone `=`) only the value, and only when it ends the
      segment. Authorization's scheme word (Bearer, Basic) stays visible. A value is no secret when
      it is shorter than four characters, digits alone, a reference (`${…}`, `$(…)`, `{{…}}`,
      `<…>`, `%…%`, `$NAME`, `process.env.`, `os.environ`, `ENV[`, `getenv(`, `op://`, `vault:`,
      `arn:aws:`), a plain URL, a placeholder word (`your`, `here`, `example`, `changeme`, `dummy`,
      `fake`, `redacted` and the like), masked (four of one of `X`, `x`, `*`, `•` in a row), a
      publishable key, or a known prefix with a body under 24 characters;
    - a webhook URL its holder can post with (Slack, Discord, Microsoft Teams, Zapier, Google Chat,
      Azure Logic Apps) and a Telegram bot token.
  - Secrets in other forms (#1941), read on each visual row's words with their boxes:
    - **Flags.** A flag that names a secret on any command (`--password=…`, `--token …`, `-D…password=…`),
      but not one about it (`--password-stdin`, `--token-ttl`), and short flags only after the program that
      reads them: `-p…` after the MySQL and MariaDB clients, `-p` after `sshpass` and a registry `login`
      (docker, podman and the like), `-a` after `redis-cli`, `-P` after `sqlcmd`, `-w` after the LDAP tools,
      `-U user%password` after `smbclient`, `-u user:password` after `curl` and `wget`. A password flag
      says what its argument is, so its value needs no secret's shape (`--password 7654`,
      `--password OrchardLantern`); a very short value, a placeholder, a reference, one public by its
      shape (below) and masks stay visible. A value whose argument opens with a quote runs to its closing
      quote, up to eight words on (a quote after `curl -u alice:` does not). A token or secret flag's value
      in the next word also has the shape of a secret: it reads as a password (below), is quoted, or has a
      digit and eight characters or more.
    - **Names glued to their program:** one part ending in a secret word (`PGPASSWORD`, `SSHPASS`,
      `VAULTTOKEN`), not a word that only ends in `pass` (`BYPASS`, `COMPASS`). Also `psk`, `sessionid`,
      `csrftoken`, `PHPSESSID`, `JSESSIONID`, and WPA, PRE-SHARED, NETWORK, WIRELESS, SECURITY, SETUP,
      RECOVERY and SDK before KEY.
    - **Labels.** One to four words that name a secret (by the setting-name rule, `Secret Key`,
      `Client secret`, `Wi-Fi Password`), a 2FA code (`Recovery code`, `Setup key`) or a password in one
      of about 30 languages (`Passwort`, `Mot de passe`, `Пароль`, `密码`, `パスワード`, `Hasło`, `كلمة المرور`,
      `סיסמה`…), their words a word space apart. Then:
      - `Label: value` (or ` :`, ` =`): the value ends the row, or the row's part before a gap of six
        characters, or reads as a password, or is quoted. After `is` or `was` (`The password is …`) the
        value must read as a password. A quoted value runs to its closing quote, up to
        eight words on; a quote after an odd run of backslashes (`\"`) closes nothing. So "Password: must
        be at least 8 characters" stays visible and "Temporary password: Wel2024! (please change it)"
        does not. A value up to 16 characters away belongs to the label; one up to 80 away only in a
        form's row, which the label starts and the value ends.
      - `Label     value` with no colon, the label starting its row and the value three to 80 characters
        away ending it (a router or settings page): the value has a digit or reads as a password, or
        under a Wi-Fi passphrase label is eight characters or more; a password or Wi-Fi passphrase
        runs on over the words after it to the row's or window's end, 80 characters in all.
      - A password word straight before a quoted value or one that reads as a password (`.netrc`'s
        `password …`, nmcli's `password …`, SQL's `PASSWORD '…'`).
      - A label alone on its row, made of words (not a sentence naming a setting), or an assignment with
        nothing after its `=`, and the value alone on the row below, starting under it (a password
        manager's field): it reads as a password, or has a digit and eight characters or more, or is a
        long token-shaped word, or under a Wi-Fi passphrase label is eight characters or more. A prompt or
        a command below is no value.
      - Right-to-left labels read their value on their left.
      - Authorization's scheme word stays visible; its credential is covered when it is 16 characters or
        more with a digit, or reads as a password.
      - A Wi-Fi passphrase assigned in one word runs on through the words after it to the row's end
        (`wpa_passphrase=correct horse battery staple`).
      A row of windows side by side is read whole and also window by window (parts six characters or
      more apart), each part with the part of the row below that starts under it.
    - A value **reads as a password** with six characters or more, a letter, a digit, and a capital or a
      symbol, or ten or more mixing cases irregularly with a symbol and no digit; never a URL, a path, an
      address, a version, a time or a call (`varchar(255)`).
    - A value **shows no secret by its shape**, each matcher checking its own: under a label of this
      section and after a password flag, an SSH, age or PEM public key, a publishable key, a `data:` URI,
      a password hash (`$2b$…`, `$argon2id$…`), a digest (`sha256:…`), a version (`19.1.4+build.107`),
      `TODO`, and the shapes `publicRun` names. After a token or secret flag with its value in the next
      word, a Bearer scheme of a label or in a cookie, also an analytics ID (`G-…`, `UA-…`), a cloud
      region, a colour and a code identifier of three words or more run together; under a label they
      are covered (`Mot de passe : us-east-1`). A `--token=…` value and the older setting rules above
      (which can also cover a Bearer credential at the end of its segment) use the setting rules' own
      value checks.
      Code that reads a secret from elsewhere (`std::env::var("DB_PASSWORD")`) is none either, but a
      reference followed by a quoted literal of four characters or more as its fallback
      (`os.Getenv("DB_PASSWORD")+"…"`, `process.env.KEY||"…"`) shows that literal and is covered.
    - **Cookies:** each value of a `Cookie:` or `Set-Cookie:` header of 16 characters or more, nine in ten
      of them letters and digits, with a letter, and with a digit or with both capitals and small
      letters.
    - **Diff signs:** a key or a setting behind `+`, `-`, `{+` or `[-` at a word's start
      (`-AKIA…`, `+DB_PASSWORD=…`, `{+AKIA…+}`). A wrapped token's next line ends at a diff's setting
      line (`+PORT=3000`) and at a line starting with a shell or SQL prompt.
    - **Token shapes:** a Discord bot token (the bot's id as base64 of 17 to 20 digits, then 6 and 27 to
      40 characters after dots), an age secret key (`AGE-SECRET-KEY-1` and upper-case bech32), a Tailscale
      key (`tskey-auth-`, `tskey-client-`…), and a kubeadm bootstrap token (six and sixteen lowercase
      letters and digits) on a line that names kubeadm or a token.
    - **2FA and wallets:**
      - a TOTP setup key: base32 of 16 characters or more with eight letters or more, as one word or
        groups, after a key, secret, setup, authenticator, 2FA, OTP or scan word on its row or the two rows
        above. It has two digits, or one in six groups or more, or, in capitals as one word or groups of
        one length, none beside words that name a setup key ("Setup key", "Enter this key"). Lowercase
        words fit the alphabet too, so a lowercase run needs two digits. Its rest on the row just below is
        covered when that row holds only base32 groups of the same case, four characters or more in all,
        and so are two such rows of groups that make a key only together;
      - recovery and backup codes under a heading that says so (recovery, backup, emergency, one-time or
        single-use codes or keys), on the heading's own row after it up to the first other word, and on
        the rows below in the heading's column, at most 24 rows, until a row there holds no codes or more
        other words than codes, or more than three rows in a row have nothing there. A
        grouped code with one character misread as `/`, `|` or `!` is still a code;
      - a seed phrase written as numbered words, 1 to 12 (or up to 24), each a lowercase word of three
        to eight letters as BIP39's are, with list numbers ("1.") or several to a row, at least eight of
        the numbers there, in the label's column (from its words' left edge to where its row goes on in
        another window), one word a number (the one nearest the label's column), never a `0.` or a
        number past 24, and only under a label within six rows above: a recovery, seed, secret, backup or
        wallet phrase or words, a mnemonic, "write down", or a 12- to 24-word phrase. A two-digit entry
        within two characters left of the next window's edge (`10.` starts left of `9.`) is that window's
        when a one-digit entry starts at or right of that edge, within two characters of the two-digit one.
        A shopping, ingredient or spelling list of short words looks the same, and with no such label this
        rule leaves it alone. The BIP39 word list itself is not used.
    - **QR codes.** With Keys and tokens chosen, a decoded code whose content starts with `otpauth://`
      or `otpauth-migration://`, a `WIFI:` code with a nonempty password (`P:`, whatever its `T:` says), or
      any content the key rules find when read as lines of words, gets one solid suggested Redact mark
      over the whole symbol, finder patterns and all, and a fifth of its side around it (at least 4 px):
      a code decodes through damage to part of it. Recognition reads the image with every Redact mark
      applied, so a code a mark already touches is found, and covered, only if it still decodes. A
      code that decodes but holds no shape these rules know (an unlabelled password, say) is an
      ordinary code: it is not covered and its content is not withheld. A code Quick redact covers counts
      as a Keys and tokens match, and like any suggestion its mark can be dismissed. The content is
      classified on the recognition worker. The editor keeps it in memory with the rest of the
      recognition result for its panel and Copy text, and put.gd saves it nowhere on disk: Quick redact's
      answer gives the created marks and the code's kind (`codes: {otp: 1}`, also `wifi` and `key`); the
      sensitive text prompt names it ("a 2FA QR code", "a Wi-Fi QR code", "a QR code with a key"); the
      resident and the notification count it as a key or token. `editor.inspect` and `text.inspect` list
      such a code as `kind: "withheld"` with `withheld` naming its kind, and no `text`. A copy an agent
      asks for (`text.recognize` with `copy`, `editor.copyText`, `history.copyText`, `action.invoke` with
      `text`) puts a placeholder such as `[2FA QR code withheld]` in its place, and `editor.copyCode`
      refuses it (`code_withheld`). The codes the person sees in the editor's panel, the person's own Copy
      text (the editor's, History's and the dashboard's) and History's Show QR code are unchanged.
      History's and the dashboard's Copy text are the person's when the request carries the token the
      resident gave its own putgd-ui on the private bootstrap pipe (#1962). Any other request is an
      agent's, and the token is in no reply or log. It needs no kernel feature.
    - **Threat model.** put.gd's agent-facing interfaces (putgd-cli, the MCP helper, session replies and
      the copies they ask for) never hand an agent the content of a code put.gd recognises as holding a
      secret (the shapes above); a code whose secret it doesn't recognise is ordinary content. A malicious same-user
      process with access to the socket or to `/proc` is out of scope: it can capture the screen and
      decode the code itself.
    Key matches that share a word are one match.
  - Limits (#1941): short-lived sign-in codes from an authenticator app or a text message (any six
    digits look alike; a list under a one-time or single-use codes heading is read as recovery codes),
    licence keys, a card's CVC and share-link paths are not looked for. A value with no digit far from its
    label, a password whose label recognition reads too far from it, and a seed phrase with no label above
    it can stay visible. A secret shaped like a public key, a password hash, a digest or a version can
    stay visible under a label of this section; after a token or secret flag with its value in the next
    word, a Bearer scheme or in a cookie, so can one shaped like a cloud region, a colour or a code
    identifier of three words run together. The older setting rules check values their own way and may
    still cover such a value. A QR code is covered only when
    it decodes and its content has one of the shapes above. With nothing beside a seed
    phrase's label, a numbered entry 13 to 24 of a list in the window beside it can be covered with the
    phrase: telling it from the phrase's own second column would uncover that column.
  - Limits: a secret with no shape of its own (Twilio auth tokens, Datadog, Algolia, Postmark, Snyk,
    Heroku, Mistral, Vercel and Segment keys: hex, UUID or plain alphanumeric strings) is found only
    through the secret-named setting it belongs to; alone it looks like a hash or an ID. AWS's
    documentation example keys are matched like real keys.
  - Recognition confusions inside IP addresses (#1913): `@` reads as `0` between digits or dots, and
    in IPv6 groups `@` and `i` read as `0` and `1`, only where every group stays four characters or
    fewer. A wrapped token's next line ends at a bare `#` or `//` word (#1914), and a quoted value runs
    across words to its closing quote (#1915). A hash whose label recognition reads too far from it can
    still be covered.
  - Not secrets (#1894, #1895): a prefixed token whose body is mask characters; a hex run with a
    hash word beside it (`sha`, `sha1` to `sha512`, `md5`, `commit`, `digest`, `checksum`, `hash`,
    `etag`, `integrity`, `blake2b`, `blake3`, the checksum tools' names such as `sha256sum` and
    `MD5sum`, a label cell or `sha256:` included; where it may sit is below) and no secret word on
    its row (`secret`, `key`, `token`, `password`, `signing`, `private`, `hmac`, `auth`,
    `credential`, `api` and the like): that secret word vetoes this hash-word rule only, not the
    public-data rules below; publishable keys (`pk_live_`, `pk_test_`, `pk.ey…`, `sb_publishable_`;
    Stripe's `pk_` is not a key prefix); the base64 of an SSH public key line; certificate and
    public-key blocks; data URIs; JSON web tokens signed with `alg: none`; subresource integrity
    digests (`sha512-…`); Ethereum addresses (`0x` and 40 hex); and Twilio account
    SIDs.
  - Public data by what is around it (#1940, from the #1931 fuzz report's groups 3, 6, 9, 17, 19 and
    21). These words are skipped by the hex, base64 and repeated-run rules; a word holding a known
    prefix's token or a JSON web token is never skipped, and secret-named settings, password URLs and
    PEM blocks are matched over every word as before:
    - a hex dump's row: an offset (four to ten hex characters, one of them may be misread) and six
      byte or word groups or more, or twelve byte groups or more without one; the groups (and the
      offset) are skipped, while the text column after them is only evidence for the fallback below
      and is still matched. A fingerprint of exactly ten groups of four hex is skipped;
    - a digest: up to four words joined, 129 characters at most, 32, 40, 56, 64, 96 or 128 hex
      characters within two as read. It is skipped when it starts its row (after a line number)
      before a ref (`refs/…`, `HEAD`) or a file name in its own column (within four characters of it,
      not in a pane beside it, #1958), the rest of that column at most a word in brackets. It is also skipped on the rows after a checksum tool (`md5sum`, `sha*sum`, `b2sum`,
      `b3sum`, `cksum`, as read too: `mdSsum`), a git plumbing command (`rev-parse`, `show-ref`,
      `ls-remote`, `rev-list`, `cat-file`, `submodule` and the like) or `podman`/`docker` with
      `--no-trunc`, `inspect` or `--digests`. The row is cut into columns at gaps of more than four
      characters; the first column whose words (after a line number) hold the whole command anchors
      the rows. Each later row needs a column starting within four characters of the anchor (after a
      line number only when the command's row has one), right below the row before (within two and a
      half heights), a digest there followed by nothing or one more word, the first row's form, and
      no `=` anywhere on the row; anything else, or 200 rows, ends them;
    - the value of a label that names an identifier, a public key or a signature, from these
      families: request, trace and event identifiers in hex and dashes (`ID`, `Event ID`, `trace_id`,
      `span_id`, `request_id`, `req`, `correlation_id`, `Boot ID`, `Machine ID`,
      `x-amz-request-id`, `traceparent`, `Change-Id` and their spellings); git's `tree`, `parent` and
      `object` at a row's start; and in base64 `x-amz-id-2`, `X-Amz-Cf-Id`, `PublicKey` and
      `public key`, go.sum's `h1:`, `Message-ID`, `X-Google-Smtp-Source`, and a DKIM signature's `b=`
      and `bh=` when `DKIM-Signature`, `ARC-` or `bh=` appears on its row or the six above. No
      certificate field is one. The value is the label's alphabet up to its usual length, at most 15
      percent and two characters more. A word counts as in the alphabet after quotes and a trailing
      comma or semicolon are trimmed, with at most one character in eight outside it and no colon; the
      hex alphabet takes dashes and needs a digit after recognition's confusions are mapped, and the
      base64 one takes `+`, `/`, `_`, `-` and `=` padding at the end. A
      label of no fixed length (`b=`, `X-Google-Smtp-Source`) runs on over the rows below whose every
      word is in its alphabet that way, each right below the one before, starting
      within four characters of the label's row or the value's start, while each row it leaves
      reaches the block's right edge. `ID` and `Key` take the word before them as part of the label
      when it is letters alone, 16 or fewer, within one and a half characters; a qualifier not in the
      families (`Secret ID:`, `Access Key:`) makes no label. Recognition's usual misreads of the label
      count: a doubled `x` (`xX-amz-id-2`), and `i` read as `t`, `1` or `l` before the final `d`
      (`x-amz-td-2`, `tracelId`, `traceld`, `requestiId`);
    - a W3C `traceparent` value as a word of its own; and a word starting with a subresource
      integrity prefix (`sha256-`, `sha384-`, `sha512-`, the number as read too: `shaS12-`,
      `sha51i2-`) with the base64 words joined after it while their total stays within the digest's
      length plus four (the first word itself has no cap). Separately, a run that holds an exact
      `sha256-`, `sha384-` or `sha512-` and one base64 character after it, at the run's start or after
      any character that is not an ASCII letter or digit, is public as a whole, as before #1940: the
      rest of the run is not checked, so it may hold more than the digest; so is one that does after up to two short label words (`4` `integrity`).
    Fallback, failing closed: a segment of the line as it would be without skipping, holding a
    skipped word, is matched as if nothing in it were skipped when it also holds a word that is not
    skipped, not a label or evidence for one, and looks like a secret of its own (eight letters and
    digits or more with both, after any label in the word; a timestamp, a date, a `0x` address of up
    to 16 hex and a word containing an algorithm's name, `sha2`, `md5`, `rsa`, `ed25519`, `x86_64` or
    `aarch64`, are not such words), or when the segment joined holds a known prefix's token or a JSON
    web token. The same holds for a segment that ends its row when the next row's first segment
    starts at that row's first word and holds such a word. This restores the ordinary matching of
    those words; it doesn't by itself guarantee a mark.
  - Not keys either (#1940):
    - the repeated-run rule (above) skips a run whose commonest letters as read (up to four, making
      85 percent of its letters, when letters are four in five of its letters and digits) are all
      from a meter's bars (`I l L i T | E F t f j J`, htop and btop) or all from a receipt's dotted
      leaders in lowercase (`c e o s n m r u v w t`); a run whose letters and digits are 85 percent
      `0` and `1`, or 60 percent with `0b` followed by at least 16 binary digits in it; and a run
      whose letters and digits, lowercased and with everything else removed, have eight in ten,
      after the first two, repeating the character two before them and differing from the one before
      (`hahaha`);
    - a hex run needs one character in eight to be a digit as read, not counting a lowercase `o`, so
      a receipt's leaders (`ccooecoo`) are no hex;
    - a hash word names a digest only in the run's own segment or in up to three words and 32
      characters on each side of it, stopping at a word with seven hex characters in a row or a gap
      wider than 30 characters, so a `commit <sha>` in another window on the same row leaves a hex
      secret on the far side covered, while a hash word in another window within those limits still
      counts;
    - a secret-named setting's value (never a URL's password) shows no secret when it is a whole call
      that reads the secret by its quoted name, `std::env::var("…")`, `std::env::var_os("…")`,
      `os.Getenv("…")`, `os.LookupEnv("…")`, `Environment.GetEnvironmentVariable("…")`,
      `ENV.fetch("…")` or `Deno.env.get("…")`, then at most `.unwrap()` or `.expect("…")` and `?`,
      `;`, `,` or `)`, or `import.meta.env.NAME`; this is apart from the older reference prefixes
      (`os.getenv` and the like, above), which accept anything after them. It shows none when it is a
      type after `:` and unquoted (`api_key: Option<String>`; `API_KEY=Bytes` and `"Bytes"` are
      values), or, for a model's token count (`total_tokens`, `prompt_tokens`, `max_tokens` and the
      like) and a name that isn't a password's, an unquoted number with thousands commas
      (`total_tokens: 12,450`);
    - an AWS key ID's prefix read across two words (`AK` `IA…`) is a key, except a card receipt's:
      its first word exactly `AID`, the next word `A` and 9 to 31 digits (`O`, `Q` and `D` read for
      `0`), and at least half of the body's first 16 characters `0`, `1`, `8` or `9`
      (`AID A0000000031010`);
    - a Telegram bot ID can't start with `0`, so an `xxd` row whose offset starts with zeros is no bot
      token.
  - Limits (#1940): these get no special exemption, so ordinary matching decides them (they are
    usually covered, since their shape is a secret's too): bcrypt hashes, `0x` and 64 hex (a
    transaction hash and a private key look the same), sample passwords in `.env.example`, bare
    base64 or hex with no label, kubeconfig and Kubernetes certificate data (the same field can hold a
    private key), mangled C++ symbols, and base64 of plain text (a decoded sentence can be a
    passphrase). A label or a hex dump's groups that recognition misreads past these rules get no
    exemption either.
  - Where a kind's shape fixes the alphabet, the usual confusions are allowed: `O` for `0`, `l` and
    `I` for `1`, `S` for `5` and `B` for `8` inside numbers, and a misread character or two inside
    a token. A number separated by dots alone is a phone number only as 3-3-4 or in pairs, so a
    version such as 26.1112.4660 is not suggested. A group of more than ten unbroken digits counts as a
    phone number only with a leading `+`, or as an 11-digit mobile written whole (below), so the
    tail of a UUID is not taken for one. A number
    grouped as a card is printed (4-4-4-4 with an optional fifth group of one to three digits, or
    4-6-5, with one separator and a first digit from 1 to 9) is never a phone number, so a card
    number that fails the Luhn check is not suggested as one. A phone candidate of more than 15
    digits is retried as shorter numbers, except an evenly spaced card-grouped one, or (#1939) an
    evenly spaced run of three groups or more, each but the last of four digits, with no hyphen;
    the retry tries the widest gap between its words first (two phone numbers in table columns)
    and can fall back to another word boundary (#1820). A card number doesn't start right after a
    digit and a dot (`198.51.100.7 5555-…` reads the card from `5555`), but may after a label such
    as `No.`, or after a `+` or `{+` (as before), or after a diff's `-` at the start of its word when
    three digits or more follow. Between
    two digits a hyphen-like dash or a minus sign reads as a hyphen (`4111–1111–1111–1111`); an em dash, and dashes anywhere else, stay as read.
  - Number columns (#1939). Phone and card numbers stop at a gap of more than two and a half
    characters (the wider glyph of the two words) between two words of a line, so table and log
    columns, calendar cells and year headers are not read as one number. A word space, and two
    spaces in a monospace font, still join. A card grouped as printed (4-4-4-4, 4-6-5, a fifth group
    of one to three) is still found across wider gaps when they are even (within a quarter of the
    narrowest plus 2 px) or no wider than four characters.
  - A card candidate that fails as one span (a reference or line number before it, the expiry
    after it, a phone in the next cell) is tried again from each word start inside the span, over
    spans of 15 to 19 digits in the run of digit groups it sits in (the run crosses a separator less than
    48 characters past the span and reads the group after it to its end), longest
    first; spans stay on one
    line except across a wrap the wrapped view accepted. The other words in the run must be other
    fields: a group of four right before it or any group right after it, on the same line and
    spaced like the card's own groups (no closer than its narrowest gap less a quarter and 2 px, no
    wider than its widest gap plus a quarter and 2 px), makes one longer number of it (a tracking number such as
    `9362 0664 7516 8861 60`), unless a slash follows (an expiry, `4242 4242 4242 4242 12 / 34`) or
    the group of four ends a phone whose group before it has one to three digits
    (`+39 380 719 1173`). A box that overlaps the card or sits closer than its own groups is another
    cell. When no span is a card, matching goes on after the failed span, as before.
  - A card read across several words is grouped as a card is printed, or close to it: groups of
    four or more with a shorter last group, plus at most one group of two or three digits where
    recognition moved a digit across a space (three when it is the first: `4111 11 111111 1111`,
    `411 11111 1111 1111`). A run of more than eight digits is the end of a card after groups of
    four or more (`4111 111111111111`), or the start of one with a tail of up to three digits, or four
    after twelve, split off (`25285945833551 94`, `411111111111 1111`); a phone's `273` before a
    16-digit number and an EAN-13 before a date are no card. A number read as one word keeps the
    earlier rule. A card number is never all zeros.
  - A counted row is no card or phone number:
    - three groups or more of up to nine digits, stepping up or down by the same one or multiple
      of five, all of them, or four in a row (calendar days after a week number, `2023 2024 2025
      2026`, axis ticks `0 20 40 60 80`);
    - days (1 to 31, one or two digits each) that start with three or more in a row, then at most
      one smaller day (a month grid beside another, whose rows read as wrapped: `16 17 18 17`), or
      after one week number (1 to 53, in four groups or more) run to the end (`48 28 29 30`).

    A phone number that starts with `+` and a country code of one to three digits set apart from
    the next digits is never a counted row (`+44 20 21 22 23 24`; a diff's `+2023 2024 2025`
    still is, and so is a country code glued to its area code, `+4420 70 71 72 73`). A step of
    zero (`5555 5555`), other steps (`01 23 45 67 89`) and a phone in pairs with a run
    inside it (`06 12 13 14 78`, `06 42 43 44 36`, `06 12 13 14 08`) stay numbers. The check runs on the match's text and, when the match
    has three words or more, on those whole words in their digit view joined by spaces, since the wrapped view glues a row's last cell to the next row's first (`18` and `17`
    as `1817`). A number only a card label lets through is exempt.
  - After a card label up to two words before it on its line (`Card:`, `Visa`, `Mastercard`,
    `Amex`, `CC`, `PAN`, `Credit`, `Debit` and their kin), a number needs only the earlier card
    check (13 to 19 digits, Luhn, no ISBN, not all zeros), so its grouping and a counted look don't
    count against it (`Card: 4110 4111 4112 4113`).
  - A phone number is also not:
    - a number immediately after a currency sign (`€ $ £ ¥ ₹`, touching it), or with a currency sign or code
      (`EUR`, `USD`, `GBP`, `CHF`, `JPY`, `kr`, `zł`, `₽`) as the next word on its line (`1 234 567 €`);
    - an unbroken number immediately after `#` (touching it, or with the `#` read as its own word
      on the same line) that follows, on that line, an order-type word (`Order #10234567`,
      `Invoice`, `Ticket` and the like, past a colon read as its own word) or a time (a receipt line,
      `19:54:46 #4091291`); `Phone #`, `Telefon #` and `Account #` keep the rule;
    - a browser's build date in a user agent: an unbroken number in a word that starts with
      `Gecko/` and a digit, optionally after `(` or `[` (`Gecko/20100101`, `Gecko/20100101)`);
    - a date with a dashed time (`2026-11-22 14-03-57`, a GNOME screenshot name);
    - an EAN-13 as printed under its bar code (1-6-6);
    - three groups of at most three digits up to 255, with no leading `+` and no `(` (colour
      channels);
    - a count with spaced thousands (`1 912 615`, one or two digits then two or three groups of
      three);
    - an unbroken size in an `ls -l` row (the row starts with a file mode, a month and day follow);
    - an unbroken number in a `free` row: the row starts with `Mem`, `Swap` or `Total` (with its
      colon attached, read as its own word, or missing) or `-/+`, only counts lie between, the row holds three counts or more, and free's header (`used`,
      `free`) is within three lines above;
    - a longer evenly spaced run of four-digit groups (a tracking number), which is not split into
      shorter numbers.

    An 11-digit mobile number written whole is a phone number (`1[3-9]` and 9 digits for China,
    `0[1-9]` and 9 digits for the trunk-0 countries such as `07700…`, `090…` and `010…`), and so is
    a German area code that ends at a slash (`+49 30 / 813854`, `030 / 813854`).
  - An IPv4 address is not taken after a standalone package epoch of one or two digits
    (`1:8.17.4.28`; the IPv4 tail of an IPv6 address stays one), before a release suffix of a dash,
    digits, an optional `.`, `~` or `+`, and a letter (`8.17.4.28-3.fc45`, `-1ubuntu2`), or right after a section or version
    label on its line (`Section 1.2.3.4`, `Version 2.11.7.54`); a range (`10.0.0.1-10.0.0.255`)
    stays two addresses. An address recognition split at a dot on one line (`148`
    `.98.250.123:443`, `148.98.250.` `123:443`) is joined when the boxes are no more than one and a
    half characters apart and the two make one whole address.
  - In a CSV row one word can hold the end of one field and the start of the next
    (`alice@example.com,+1` then `202 555 0143`). A later match keeps its other words instead of
    being dropped when at least one of them is unclaimed and every word it shares with earlier
    matches either holds a comma or a semicolon (a literal check, with no CSV parsing; for any kind
    but email) or, for a card or phone, belongs to a key match. Both apply together, so a phone
    between a glued address and a key keeps its middle.
  - Known limits of these rules (#1939):
    - A card number whose matched text has spaces (a wrap can join two words without one) with more
      than one short group, a one-digit group, or a
      two-digit first group (its last group aside) isn't recognised unless a card label precedes
      it. Card numbers aren't printed that way; calendars and tables are.
    - A card whose grouping recognition shifted and that also has a gap wider than four characters
      inside it (`4111 111 11111 1111` with a wide gap) can be missed, even after a card label,
      since looser groupings across columns are what tables of numbers look like.
    - A phone number in a tiled window that lines up exactly with a `free -t` Total row beside it
      is taken for a count.
    - A cropped calendar row with a week number and the next month's first day
      (`05 29 30 31 01`) can be suggested as a phone; the same shape is a mobile number in pairs
      (`06 12 13 14 08`).
    - Eight two-digit groups with `ff:fe` in the middle (an EUI-64) are still covered as an IPv6
      address.
    - A garbled address (`219..31.175.7`) next to other numbers in `ss` output is no longer covered
      by chance as part of a phone number.
  - Categories: `email`, `phone` (international and national forms, at least 7 digits), `ip`
    (IPv4 and IPv6), `card` (13 to 19 digits with spaces or dashes, Luhn-valid), `key`, `name` and
    `list`. Keys and tokens means these shapes, listed in the tool's help: AWS access key IDs;
    GitHub, GitLab, Slack and Stripe secret tokens; OpenAI, Anthropic, Google, Hugging Face, npm,
    PyPI, SendGrid, DigitalOcean, Shopify, Twilio, Mailgun, Square, Docker Hub, Grafana, Vault,
    Postman, New Relic, Linear, Atlassian and Sentry tokens (shapes as in gitleaks' default rules);
    passwords in connection strings; values of secret-named settings; webhook URLs and Telegram bot
    tokens; JWTs; PEM private-key blocks; unbroken hex or base64 runs of 32 characters or more,
    except digests beside a hash word; Discord bot tokens, age secret keys, Tailscale keys and kubeadm
    tokens; passwords in command-line flags and cookies; values beside or below a password, key or
    Wi-Fi label, in several languages; 2FA setup keys, recovery codes and numbered seed phrases; and QR
    codes holding a 2FA secret, a Wi-Fi password or a key. `name` and `list` are described below.
  - Speed (#1897): each segment is scanned once with one pattern of every prefix's head, and a
    prefix's own pattern runs only where its head occurs; the run search looks only as far as 256
    characters reach from each word and rules a window out from running counts before reading it.
    A line of thousands of token-shaped words takes well under a second.
  - A word belongs to one match at most. Where kinds overlap the more specific wins, in the order
    key, email, card, ip, phone, name, list. A login inside an email address stays part of the
    email match. Where `name` or `list` would take a word that an earlier match already has,
    the hit keeps the words the earlier match left, so a name is not left bare because a
    neighbouring word was taken.
  - **Names and your own words (#1618).** `name` and `list` match literal text, not shapes, and
    words and names work on the same unfiltered candidates, lines and wrapped lines as the other
    kinds. Patterns see lines as described under Patterns.
    - **Where the name comes from.** `name` reads the account of the process that runs the editor
      (`putgd-ui --edit-image`): the login name from `getpwuid_r(getuid())`, and the real name
      from the text before the first comma of its GECOS field, split on spaces into parts. It
      does not read `$USER`, which `sudo -E` or a changed environment can make wrong. If the
      GECOS name is empty it asks AccountsService for `RealName` on the system bus, without
      starting the service, with 250 ms for each of two calls; any failure leaves the real name
      empty and the login still matches. The lookup runs on a thread of its own, once at a time for the
      whole process, and only when `name` is chosen, so it never waits on the interface thread
      (`getpwuid_r` retries with a buffer doubled up to 1 MiB when the entry is larger than
      expected, and a directory service can be slow). The other categories wait for it at most 1
      second. Without an answer by then they run on without `name`, the lookup goes on in the
      background, and the result says so: `nameUnchecked: true`, and the line "My username and
      name was not checked: the account lookup did not answer in time." A later run that starts
      while a lookup is still going waits for that one rather than starting another. Checked on
      2026-11-18: GECOS is filled on f44, d69, k72 and m74, and empty on aurora-amd, where
      AccountsService has no name either, so there only the login is matched. The editor keeps
      the lookup result in memory only and sends no separate account-name field to the matcher.
      The recognized text, which may contain the name, does go to the pattern child as a subject
      when the user has patterns. Nothing about the account is stored or logged.
    - **Where the list comes from.** `list` matches the saved words and patterns
      (`preferences.quickRedactTerms` and `preferences.quickRedactPatterns`, see
      [configuration](configuration.md)), plus the `terms` and `patterns` of one `redactText`
      command. The saved ones reach the editor in the bootstrap frame as `redactTerms` and
      `redactPatterns`, each sent only when it is not empty. An agent's own `terms` and `patterns`
      stay in that request. They are never written to a log or a diagnostics bundle (the bundle drops both
      keys wherever they appear, keeping only a count). The operation journal's copy of the settings
      snapshot holds them, as local state with the same trust as `config.json`.
    - **How a word or phrase is compared.** Both the entry and the recognized text go through
      NFKC, case folding and one apostrophe for curly and straight. Case folding is the default
      one, so `kır` and `kir` stay apart, plus `ß` and `ẞ` as `ss` (`Straße` matches `STRASSE`). Accents are stripped only from Latin, Greek and Cyrillic letters; in other
      scripts, such as an Indic vowel sign, a combining mark is a letter of its own and stays.
      Lengths and edges count Unicode code points, and a combining mark belongs to its word. Text
      is never compared with a regular expression, so `*`, `?` and brackets are only characters. An
      entry splits into parts at spaces, `-`, `_`, `.` and lower-to-upper case changes between its
      letters and digits, so `PatrickHudson` has the parts `patrick` and `hudson`, and `srv.data`
      also matches `srvdata`, `srv_data`, `srv-data` and `srv data`. Punctuation
      before the first letter or after the last stays part of the entry, so `.env` is not `env`
      and does not mark it; a space inside that edge punctuation is a join, so `# 42` matches
      `# 42` and `#42`. In the text the parts must come in order, joined by nothing, by up to
      two characters from space, `-`, `_` and `.`, or by a space, one of `-`, `_`, `.` and a
      space. A gap between two recognized words counts as a space, and a wrapped line joins
      where the wrap evidence above joins lines (a line that ends or starts with `-`, `_` or `.`,
      or a wrapped column). So `Patrick Hudson` matches `Patrick Hudson`, `patrick-hudson`,
      `patrick_hudson`, `patrick__hudson`, `PatrickHudson`, `Patrick - Hudson` and the part of
      `patrick.hudson@x.io` before the `@`, but not `Patrick -- Hudson` or `Patrick - - Hudson`.
      "Last, first" takes a comma with an optional space on either side.
      Every occurrence is found, overlapping ones too, and a word belongs to one match.
    - **Edges.** A term starts and ends at a word edge or beside a character that is not a letter
      or digit. `patrick` matches `/home/patrick/x`, `patrick@d69:~$`, `Patrick's` and
      `Patrick,`, but not `patricks` or `pattern`.
    - **Misreads.** A term with 6 letters or more also reads `I`, `l`, `1` and `|` as one
      character, `O` and `0` as one, and `rn` as `m`, on both sides. A shorter term must match
      exactly, since folding it hits ordinary words. There is no edit-distance matching, so a
      misread beyond these folds is missed.
    - **A list entry matches only whole.** `Patrick Hudson` in the list does not match `Patrick`
      alone; the user adds `Patrick` as its own line for that.
    - **`name` knows which part is which.** It matches the whole real name as an entry would,
      "last, first" (`Hudson, Patrick`, with the first name only: a middle name is ignored), the
      first initial and the last name (`phudson`, `p.hudson`, `P. Hudson`, `p-hudson`), each part of
      3 characters or more on its own (`Patrick`, `Hudson`), and the login name. The login follows
      the same part rules as an entry, so a login `jane.doe` also matches `jane-doe` and
      `janedoe2`. A form made of one part needs 3 characters. A first name of one letter makes no
      initial form of its own, since the whole name covers it. The login and the initial-plus-last
      forms may end in up to 4 digits, so `phudson2` matches. The longest form is tried first, so
      a whole name is one match and not one per part.
    - **Never matched.** A part shorter than 3 characters on its own (`Li`, `Al`), though it
      still counts inside a whole-name match; a single initial; and anything that needs
      edit-distance fuzziness. With the name `Patrick Hudson`, `Patricks`, `pattern`, `pat` and
      `hudsonia` get no mark. `Hudson` in `Hudson Bay` does, since it is the user's surname.
    - **Patterns.** A pattern is ordinary PCRE2 search (`QRegularExpression`) with Unicode
      properties on. Case is as written, and `(?i)` turns it off, which is the usual regex
      expectation and unlike a word, which always ignores case. At run time the matcher puts
      `(*LIMIT_MATCH=10000)(*LIMIT_DEPTH=10000)` in front of it, which can stop a search but never
      change a match, and the child's kill supplies the wall-clock bound (below); the save-time
      check compiles the pattern as written. A pattern sees the OCR word text as shown, with no
      NFKC, digit or case folding (those are for words and names only). Its subjects are each
      visual line on its own, its words joined by one space, and each pair of consecutive visual
      lines in reading order joined with `\n`. A pair's hit counts only if its matched span
      includes the joining newline; a hit inside one line is found in that line, once. A use of
      the break that is not part of the match, such as `foo(?=\nbar)`, matches the pair but adds no
      mark. So `quick\sredact` matches across a
      wrapped line, a literal space does not, and nothing spans three lines. Each subject is
      searched once, from its start, as global matching does. Examples: `\bJIRA-\d+\b` and
      `(?i)\b[a-z0-9-]+\.corp\.example\b`. A hit marks every word box it touches. An empty hit
      marks nothing. A pattern that matches everything (`.+`) is allowed, and the result says
      how many marks it added.
    - **Limits and the save-time check.** Up to 200 words and up to 50 patterns. A word is 1 to
      100 code points after trimming. A pattern is up to 500 code points as written: it is not
      trimmed when it runs, so its spaces can matter, and trimming only decides whether it is
      blank. A word or pattern may not hold a line feed, a carriage return, U+2028 or U+2029.
      Trimming is exactly Qt's (`QChar::isSpace`), so U+001C to U+001F are kept. A word needs a
      letter or digit. A pattern must not be blank, must compile, and must not match empty text.
      That is all: the shape of a pattern is not judged, so `(*` verbs, `\C`, lookarounds, back
      references and nested repeats are allowed. All of it is checked when the setting is saved
      and when an agent sends it, with the same rules in Settings, the editor and the MCP helper
      (a shared table in `tests/fixtures/text/redact-validation.json`). The MCP helper checks the
      line break, trim, length and letter rules and leaves compiling and the empty-match test to
      the editor, whose answer is authoritative. The schemas keep only the line-break rule, a
      minimum length of 1, and 500 characters for a pattern; they have no length cap for a word.
    - **The pattern child.** Nothing bounds a regular expression's time inside a process, so user
      patterns run in a child, `putgd-ui --match-patterns`, started only when there are patterns,
      and the editor kills it at the deadline (2 s by default, counted from the start of the
      matching worker, which includes the account lookup's wait). The editor is already
      `putgd-ui`, so no new binary or package change is needed. The child gets the subject
      texts, the line-break offsets of the pair subjects and the patterns as JSON on stdin and
      writes one line per pattern as each ends: the pattern's index, whether it finished, and
      (subject, start, length) ranges. Word ownership and the one-match-per-word rule stay in
      the editor. Patterns that finished, the saved words and every other category still land. A
      pattern the child did not finish, that did not compile, or that stopped on PCRE2's own
      match or depth limits (`(a+)+$` stops in about 1 ms, before the deadline, so the next
      pattern still runs) is named as skipped: "Pattern 3 didn't finish and was skipped." (the
      first pattern is number 1). The result never says the page was fully checked. A missing
      `putgd-ui` skips only the patterns.
    - **Confinement of the child.** Before it reads its input from stdin it has no new
      privileges and is not dumpable, `RLIMIT_FSIZE` 0 (no file it can write), 16 file
      descriptors, no core dump and a 30 s CPU limit, and a best-effort Landlock ruleset denies
      all file access (an older kernel keeps the other limits). Descriptors above 2 are marked
      close-on-exec where the kernel has `close_range`. Its environment is replaced, not
      inherited: it holds only `LC_ALL=C.UTF-8`. It never logs, and the text crosses only the
      pipe. It dies with the editor (`PR_SET_PDEATHSIG`).
    - **Timing.** Measured on m74 (i5-6500T, Qt 6.4.2, Debug build): starting the child takes
      about 20 to 30 ms (the slowest of five runs 30 to 40 ms with one pattern, against 10 ms
      without patterns), and killing it at the deadline about 10 ms to kill and reap. The matcher
      answers within its budget plus 1 second: the editor takes the worker's answer when the
      worker ends, or at that moment, whichever is first, with what it had by then. Applying
      the marks then waits separately for an active drag (see Applying the marks).
      `(?:(?=.*+)){3000}\w{12}`, `(?:(?=.*+).)+\d` and `(a+)+$` on a long line were killed or
      stopped and named, and the email, the word and the `JIRA-\d` pattern before them still
      landed.
    - **Exit.** Closing the editor kills the child: `QApplication`'s destructor (a post routine)
      cancels every worker and waits at most 1 second, then lets a stuck worker end on its own.
      A worker reads the cancel every 50 ms while the child runs and kills it, and while it
      waits for the account lookup. The lookup is told the process is exiting after the password
      entry and before the AccountsService call, so a lookup that gets that far never contacts
      the system bus; one stuck in the directory service is left behind. Measured with the child
      running: a wait of 6 ms; with the lookup stuck past the limit, the process ends 960 ms
      after quitting.
    - **Geometry.** A hit marks the whole word box it touches, as every kind does.
      `/home/patrick/project` is covered entirely, not only `patrick`. A sub-box estimated from
      character counts could leave part of a name visible in a proportional font, so none is made.
    - **No list saved.** "My redaction list" is disabled in the menu with the tooltip "Add words
      or patterns in Settings → Capture → Text recognition." when the saved words and patterns are
      both empty. A `redactText` whose `categories` is `list` alone with nothing to match (no
      saved list, no `terms`, no `patterns`) answers `created: []` and `counts: {list: 0}`
      at once, without starting recognition, so it cannot fail for lack of a recognizer. The
      result line says "Your redaction list is empty. Add words in Settings → Capture → Text
      recognition." With other categories chosen, recognition runs as usual.
  - A match becomes Redact marks over its word boxes, one box per line (or per run of a line, below), padded by a quarter of the
    line height above and below and by one and a half characters on each side, clamped to the
    image. A line's matched words are taken left to right, and a new box starts where the gap from
    the right edge of the run so far is more than three times the median height of the line's
    matched words, as between two windows side by side or the columns of a table. Each run gets its
    own box, as tall as the line's, with the usual margins. The shortening towards a neighbouring
    word described below applies only on a side that doesn't face another run: a box there can
    stop short of a matched glyph, so that margin stays whole and can reach into a label beside
    the column, or into the gap when a recognized box runs wide. The runs get separate boxes, though
    their margins, sized from the edge words, can still cover a narrow gap (#1846). Matching is unchanged: when a line already forms one key
    match, as `Card 4111 1111 1111 1111 Card 5555 5555 5555 4444` does (test card numbers of
    repeated digits), its box covers the labels between the numbers, such as "Card", and a
    misread number stays covered as long as it is part of that key match. A character's width comes from the edge word's own box, not from the line's span with
    its spaces. Recognition boxes can stop a whole glyph short, which the side padding covers.
    Towards the nearest other recognized word on the same line, when the padding plus one
    character could reach it, the padding ends at the far side of the blank space before that
    word, leaving the space's last column uncovered, so the word stays readable (#1838). The
    space can only shorten the padding. Boxes can be off by about a glyph either way, so the
    space is found in the pixels of every row the mark covers, up to a character past the
    neighbour's box edge. A column is blank when every pixel in it is within 8% of the text's
    contrast of the background (4 to 24 levels in any colour channel). The background is the
    median colour of the line spacing above and below the mark. The neighbour's box edge counts
    only where ink starts right after a blank column within a pixel of it, and that blank is at
    least as wide as the next one after the ink, because a word space is usually wider than a
    gap between glyphs. Otherwise the mark goes past what lies at that edge and the ink after
    it, to the next blank run. This is a heuristic: it keeps matched glyphs covered on the
    measured fonts and sizes. A matched glyph outside its recognized box can still show when the
    gap before it is wider than the space after it. The whole padding stays in five cases: there
    is no blank run in reach; there is no line spacing to sample (a mark as tall as the image);
    more than a quarter of the line-spacing pixels differ from the background by over 24 levels;
    any pixel looked at is not fully opaque; or the image ends before the matched box's edge. A
    covered neighbour is better than a matched glyph showing. A cover can take the last letter of
    a short word right before a match, as the "t" of "at" before an address, when that word's box
    ends inside the letter: from the pixels alone it looks the same as a neighbour's box that
    starts on a matched glyph (#1849). A neighbouring word joined to the match by dropped punctuation
    is covered with it (#1854): `jane` and `doe@example.com`, read from `jane.doe@example.com` with
    the dot left out and a word space's gap between the boxes. Quick redact checks the nearest
    unmatched word on that side of the row whose box lies wholly beyond the words covered so far and
    that the padding plus one character could reach. It is joined when,
    between the two boxes, there is a separate piece of ink: pixels connected edge to edge that stay
    strictly between the two boxes' columns and are at most half the row's height, such as a dot,
    comma, hyphen or underscore. The pieces are looked for at half and at a quarter of the text's
    contrast; at half, a piece needs at least two pixels. No blank run from the matched ink to the
    word's box may be wider than a quarter of the edge word's average character width (rounded
    down, at least one pixel), where a word space would be. The background is found as for the
    padding above, and nothing is joined without the picture or where those background checks fail.
    The check repeats from each word it joins, and the padding then goes on from the last one. It
    never applies on a side that faces another run, and it only adds to a mark, which grows to take
    in each joined word's whole box. A dot of a single pixel at half contrast is missed when, at a
    quarter, its fainter edge links it to ink that reaches either box's columns; with no other piece
    that qualifies, the word is not joined, though the padding can still reach part of it. A mark never shrinks inside the
    matched words' own boxes. A mark can also take in an ordinary word joined to a token, such
    as a label with digits ("sha256"), or the rest of a PEM BEGIN or END line. A prefixed token of a
    kind whose length varies (JSON web tokens, Slack `xox…` and `xapp-`, Stripe API keys, GitLab
    tokens) takes the token pieces of its run (the adjacent words that count as token pieces under the
    key rules above, misread characters and quotes allowed as described there, measured in their
    text joined without spaces) that start within half again
    its usual length of the first piece it matched, and anything further its prefix pattern's body
    ran through. A kind with
    one length (an AWS key ID, a classic GitHub token, a fine-grained `github_pat_` token, a
    Stripe `whsec_` secret) ends at that length counted from its prefix: it takes the pieces of
    its run that start before the length is reached, and a piece of one or two characters just past it, which a
    doubled character leaves. Words after that are treated like any words at the end of a run of
    token pieces: a run of ordinary words at the end (under 12 letters or digits, no digit, no
    capital after the first letter, as "was leaked today") is left out, and the rest stay covered
    (#1839). A token that runs to the end of its line goes on
    over each following line that starts with the rest of it (a JWT soft-wrapped in an editor),
    but only where a wrap leaves one (#1856, #1859). The position check uses both lines' boxes;
    the checks on what the remainder holds look at the next line's first run of token pieces, the
    run that would continue the token, not the rest of that line:
    - The next line sits right below: its top is below the top of the line above by at most three
      times the taller of the two lines' boxes (recognized boxes are shorter than the line pitch).
      It starts no further right than the line above, plus four and a half characters for an
      editor's wrap indent; a character's width is the line above's word-box widths over its
      characters, so the gaps between its words don't count. Nothing limits how far left it
      starts. A line far down the page, or in a column to the right, is not taken.
    - A prefixed kind of bounded length (an AWS key ID, a GitHub, GitLab, Slack or Stripe token)
      takes remainders only while the token's run, counted from its prefix, plus every remainder
      taken stays within half again its usual length. A JSON web token has no such bound. Each
      remainder also stays within the token's own run's length plus two, as before.
    - The run is not taken when it holds a card number or an email address as the card and email
      rules read them (for an address, with a dot recognition lost restored, but without the
      gap-based joins of split addresses or the message-header checks of the email kind; for a
      card, the card pattern, including a diff's `-` at the start of a word before three digits or more,
      with the basic card check: 13 to 19 digits, Luhn, no ISBN, not all zeros, without the
      grouping and counted-row checks of the card kind), or a prefixed
      token. This only stops the token taking that line: the card rule decides on its own whether
      the line is a card (a counted row such as `4110 4111 4112 4113` without a label is not one).
      Finding an address or a prefixed token there likewise stops the token taking that run; what
      covers it is left to the kinds chosen. A hex or base64 run there has no prefix and can be taken. One
      exception is a heuristic for a JWT wrapped just after one of its dots: when the part taken
      so far ends with a dot (or a comma read for one), a JWT-shaped match on the next line, such
      as an `eyJ` payload, is taken as the same token's next part.
    - Card lines that recognition reads as one unprefixed match (a hex, base64 or repeated-character
      run), as `Card 4111 1111 1111 1111 Card 5555 5555 5555 4444` can be, follow by another rule:
      when the match holds a valid card and the text after its last card is not itself a hex or
      base64 run, the next line's run is taken only when it holds 13 digits or more (after the card
      rule's digit misreadings, Luhn not checked), so a misread card number below stays covered
      and a line such as `SIGNING_HEX =` stays out. When that text after the last card is a hex
      fragment (after any opening quote, read with the usual hex misreadings), a remainder of
      ASCII hex digits with at least one digit, followed by any number of closing quotes,
      brackets or separators, is taken too.
    The short closing-quote tail below (#1850) is checked before all of these but the first, and
    ends the follow.
    A remainder of 8 characters or more counts by its shape; a shorter one (3 to 7 characters)
    counts only as a single recognized word with a digit, no run of four lowercase letters, and
    a closing quote, bracket or separator at its end, so `08edc"` is taken and a bare `08edc` is
    not (#1820). When the token's words leave a quote open (`"6311…343`), the next line's first
    recognized word is the token's tail too when it is 1 to 7 ASCII letters, digits or `+/=_-`, then
    a closing quote, then only `)`, `]`, `}`, `;` or `,`: letters alone included, as in `edc"`,
    `edc",` or `edc”` (recognition often reads the closing quote curly). A quote is open when its
    family appears an odd number of times in the token's words, the first such family counting:
    straight and curly double quotes, then straight and curly single quotes with the prime, then
    the backtick. The quote is the evidence, so a word such as `add` under a digest with no open
    quote is not taken (#1850).
    Recall wins over that precision. One invocation is one undo step. The result says "Added 3
    suggested redactions" or "No matches found. Text can still be there.", with a line for each
    skipped pattern. There is no "safe" verdict and no upload confirmation.
  - **Each mark remembers why it was added.** A mark Quick redact adds carries its category
    (`name`, `list`, `email`, and so on), never the recognized text, and the box it was suggested
    at. Moving or resizing it keeps both. A selected suggested mark shows "Suggested: My username
    and name" (or the label of its category) in the Redact panel, and `editor.inspect` shows
    `suggested` on the shape. A mark drawn by hand, or added with `editor.apply add`, has no such
    field, and `update` keeps it.
  - **Applying the marks.** Starting Quick redact while a mark is being dragged answers
    `editor_busy` at once. If the matcher's result lands while a drag is in progress, the marks
    wait for the drag to end and then join the document as an undo step of their own, so Undo
    removes the suggestions and the drag separately. The wait is up to 20 seconds counted from the
    start of Quick redact (the longest recognition deadline). If the drag is still active then,
    the command returns `editor_busy` ("A mark was still being dragged while Quick redact
    waited"), says "A mark was still being dragged, so nothing was added. Try again." and adds
    nothing. Staleness uses a counter that rises with every Redact mark or crop change, so a
    change that was then undone is still a change, and the result adds nothing (`edit_stale`).
    Switching tools, or a middle-button or space pan, while a mark is being drawn, moved or
    resized abandons that drag and its document gesture together, and the half-drawn mark is
    dropped. While a pointer gesture is open, a document-changing agent command answers
    `editor_busy` and changes nothing: every `editor.apply` op (`add`, `update`, `remove`,
    `crop`, `resetCrop`, `undo`, `redo` and `redactText`) and the editing and selection
    `editor.view` actions (`removeMark`, `removeSuggestions`, `place`, `geometry`, `applyCrop`,
    `blurStyle`, `stepNumber`, and selection changes). Views that change nothing still answer.
  - **Deleting a suggestion.** Deleting a suggested mark, with the Select tool and Delete or
    `remove {shapeId}`, records the category and the unpadded box of the recognized words it was
    built from in the document's history. Running Quick redact again on the same edit does not
    add a mark of the same category whose unpadded box overlaps that one by at least half of
    their union, since a later run reads the page with the other marks blacked out and its
    boxes move a few pixels. A different category, or an OCR box that differs enough, can give a
    new mark. Two neighbouring one-character words are not taken for one suggestion. Undoing the
    delete forgets it. It is kept in memory only, for that edit, and `counts` counts only matches
    that added a mark.
  - **Remove suggestions** removes every suggested mark that is still there as one undo step, and
    leaves marks drawn by hand alone. Agents have it as `editor.view` `removeSuggestions`, which
    answers `edit_shape_missing` when there are none and `editor_busy` mid-drag. It is off when there are none, and it does not record
    anything as deleted. It says "Removed 3 suggested redactions". Removing a mark gives its
    pixels back to the export through the same redaction path as any other mark.
  - Choosing a category with no result for the current revision starts recognition and waits for
    it, or joins the job already running, under that job's deadline. If a Redact mark or the crop
    changes while it waits, or while the matcher runs afterwards, it ends with `edit_stale` and
    adds nothing.
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

## Checking captures before uploading

[#1804](https://github.com/patrick-hudson/put.gd-desktop/issues/1804) adds **Check captures for
sensitive text before uploading** to Settings → Capture → Text recognition, with the kinds it looks
for. It is off by default. With it on, put.gd recognizes the text of an image that is about to upload.
Kinds set to `redact` are covered automatically. It asks when `warn` matches are left, or when the
check couldn't finish. An image with nothing found uploads as it does with the option off, with the
check's time added. Automatic covering acts only on what recognition detected, and recognition can
miss text.

The feature adds no confirmation step before an ordinary upload. An explicit upload is consent. The
panel is a targeted warning that the user turns on, and it opens only for a finding or a check that
couldn't finish. Don't extend it into a general "are you sure?".

| Part | Rule |
| --- | --- |
| Setting | `preferences.sensitiveTextCheck` `{enabled, kinds}`, see [configuration](configuration.md). The kinds are Quick redact's categories: `card`, `key`, `list`, `email`, `phone`, `ip`, `name`. Each is `off`, `warn` or `redact` ([#1820](https://github.com/patrick-hudson/put.gd-desktop/issues/1820)). The default is `card`, `key` and `list` on `warn`. #1804's `categories` list still reads, each listed kind as `warn`. Settings' **Apply to all kinds** is a shortcut that sets all seven; "redact everything" is every kind on `redact`, not a separate mode. Email addresses, phone numbers, IP addresses and the user's own name appear on most ordinary chat and terminal screenshots, so they are offered but off by default. `list` matches nothing until the user saves words or patterns. Settings won't turn the option on while text recognition is unavailable |
| What is checked | Only uploads: an action whose output is a destination. Region, window, monitor, desktop, Repeat last region and scrolling captures; the clipboard action with an image, or with copied files; and the file manager's **Upload with put.gd**. Among files, those whose signature is PNG or JPEG are checked, one after another, including files expanded from a folder when folders upload file by file. The contents of a folder uploaded as a .zip are not checked |
| Not checked | Capture region & edit (the user reviews that image before Upload), recordings, files that are not PNG or JPEG, a folder's .zip, clipboard text, diagnostics packages, captures going only to the local archive, and retries. A retry, including one after a failed upload, sends the stored artifact without checking it again. A new capture or file selection is checked |
| Where it runs | In the editor helper, `putgd-ui --edit-image`, in check mode. The resident stages the image as it does for Edit & upload, and keeps its own copy for the upload. The helper runs the editor's recognition (`putgd-ocr`) and Quick redact, including the account name lookup and the pattern child, on a document it does not show. No editor or matching code is in the resident. Recognition, its deadline, its limits and its confinement are exactly the editor's |
| One at a time | Checks run one after another. A capture that needs a check, taken while a prompt is open, waits in the `checking` phase. So does every file-manager or clipboard file selection, because which of its files are PNG or JPEG is read only when its turn comes; a selection with none then goes on unchecked. Other captures go on as usual |
| Nothing found | The helper ends at once, and the capture goes to the upload queue unchanged: same format, name, copy and history |
| With capture style | The check reads the capture as it was taken, never a backdrop. A covered or unchanged capture is then styled like any capture ([capture style](capture-style.md#captures-the-image-job-style-task)); the original kept with Keep a local copy is the one from before covering. After Redact and upload, the editor opens with the style on and its export is not styled again |
| Overlaps | `redact` kinds are matched first, together, then each `warn` kind alone, on the same recognized words, so no `warn` kind's match hides another's. A word both find is covered. A `warn` match whose words the covers took entirely asks nothing; one they took only part of still asks. With no `redact` kind, matching is #1804's single pass |
| Only `redact` kinds found | The helper's Quick redact marks for those kinds are the covers. It exports the document as a sanitized PNG and hands it off as an edit's PNG, with a `covered` count per kind and no prompt. The operation uploads that PNG, and `sensitive` holds `{found: {}, covered: {card: 1}, complete: true}`. The notification adds "Covered 1 card number." |
| Found | `warn` kinds found: the operation's phase becomes `sensitive_text_found`, and its `sensitive` member holds `{found: {key: 1}, covered: {card: 1}, complete: true, prompt: 1}`, kinds and counts only. `found` lists the `warn` kinds left to decide; `covered` the `redact` kinds already covered. A person sees the panel. An agent decides with `operation.decide` |
| Couldn't finish | Checking that couldn't finish asks too, and never passes silently. That covers: recognition failing or timing out; a result cut at its time or size limit (second reads never cause this); an image over 24 megapixels; an image the helper can't decode; text recognition being unavailable; a pattern from the list that didn't finish; an account lookup that didn't answer; matching cut at its deadline; and a language chosen in Settings with no data installed. `sensitive` then has `complete: false` and a `reason`: `recognition_failed`, `truncated`, `too_large`, `unavailable`, `patterns_skipped`, `name_unchecked`, `matching_unfinished` or `language_missing`. It also lists whatever was found. This holds with every kind on `redact`: what was found is covered first and the panel opens, focused on **Redact and upload** when a `warn` kind was found (`sensitive.found` not empty) and on **Cancel** otherwise; in report mode an unanswered decision times out to Cancel. **Upload anyway** then keeps the covers |
| No decision possible | A check that fails before it can find anything or ask uploads nothing. That happens when the image can't be staged (over the staging size limit, or a failed disk write), or when the helper is missing, crashes or gives no result within 40 seconds of starting. With **Keep a local copy** on, put.gd then tries to save the capture locally, as Cancel does, and its History item says "Not uploaded: put.gd couldn't check it for sensitive text."; if that save fails too (a full disk), the operation fails. Otherwise the operation fails with `sensitive_check_failed`, whose `details.cause` names what failed. If the helper fails after **Open in editor** opened the editor, the operation also fails with `sensitive_check_failed`, `details.cause` holding the editor's error, and there is no local-copy fallback |
| Covered export fails | When the helper can't export or stage the covered PNG (an encoding failure, an image over the output limit, a failed disk write), including after **Upload anyway** or **Redact and upload**, the original is never sent in its place: the operation ends as under "No decision possible", saved locally as the original with **Keep a local copy** on, otherwise failed with `sensitive_check_failed` |
| Local copy | Only a covered upload, with no prompt, after **Upload anyway** or after **Redact and upload**, keeps two copies. With **Keep a local copy** on, its History item and archive copy are the covered PNG. The original is saved to the archive too, beside it, with `-original` added to its name, and `sensitive.original` holds `{status, paths}`. It goes straight to the archive, outside the artifact store and its Waiting uploads budget, and is never uploaded. For a file-manager selection, the original is the user's file, copied only while its device, inode, size and modification time still match the check; a changed file isn't copied. A change in the moment between that comparison and the copy is not caught, and affects only the local copy. A failed save of the original is logged, `status` is `failed`, the notification adds "The original before covering couldn't be saved locally.", and the upload goes on |
| File selections | Each file is checked and covered on its own. A covered file uploads as its covered PNG (named after the file, with `.png`), staged privately under the resident's `covered/` directory until its upload has copied it, then removed. A file still on `warn` asks as before |
| Files that change | A file-manager upload is checked, then copied from its path for the upload. put.gd records each checked file's device, inode, size and modification time when it checks it, and compares them again just before the files are copied into put.gd's artifact store for the upload. A file covered at check time (automatically, after **Upload anyway** with covers, or with **Redact and upload**) is not compared: its covered PNG, made at check time, is what uploads, and a change to the file only keeps its optional local original from being saved. A file chosen more than once is compared if any of its places still uploads the file itself, whatever another place chose; if it changed between two of its checks, that comparison fails. If any other checked file differs, the whole upload fails with `sensitive_check_failed` (`details.cause` `file_changed`), and nothing is uploaded or archived. A change in the moment between that comparison and the copy is not caught |

### The prompt

The prompt is put.gd's own frameless panel, drawn by the helper from the theme tokens: the raised
surface, a strong border, a gold accent edge, the `alert` glyph and the `putgd::Ui` roles. It has no
desktop title bar or frame, and it moves by dragging its body, through the desktop's own move. It is an
ordinary window: it opens raised and focused, and another window can cover it. On Wayland a window
can't ask to stay on top, and put.gd keeps it an ordinary window everywhere rather than an overlay that
can't be moved. While a capture waits, the tray and History say "Waiting for your decision", and a click
on the tray icon brings the panel back instead of opening History, where the tray host sends put.gd the
click. Ubuntu's AppIndicator tray opens put.gd's menu on a click instead. The tray menu's **Open
put.gd** still opens History. On every desktop put.gd also sends one notification for each prompt a
person is shown, "A capture is waiting for your decision", while notifications are on ([#1829](https://github.com/patrick-hudson/put.gd-desktop/issues/1829)).
It has **Show**, which brings the panel back, where the desktop's notifications support buttons. Where Qt
shows a tray balloon instead (no notification server, on the Qt tray), clicking it does the same. During a scrolling capture the
notification waits until the capture ends, and goes out then only if the prompt still waits. Its size is fixed, so tiling compositors (Hyprland, Budgie's labwc)
float it. Where the desktop lets a window choose (X11), it opens centred on the screen with the pointer.
Its four buttons sit in one row, **Cancel** and **Open in editor** on the left, **Upload anyway** and
**Redact and upload** on the right ([#1898](https://github.com/patrick-hudson/put.gd-desktop/issues/1898)). Where that row is wider than 90% of the screen (large text on a
small display), **Upload anyway** and **Redact and upload** move to a second row under the other two;
where even a pair doesn't fit, each button takes a row of its own, **Redact and upload** last. The panel
is never wider than that 90%: the preview shrinks to fit, and a long file or destination name is
shortened in the middle.
Focus starts on **Redact and upload**, or on **Cancel** when it isn't offered.

- The title is "Sensitive text found", or "put.gd couldn't finish checking this capture". The body
  names what was found by kind and count: "This capture shows a card number and a key or token. It
  hasn't been uploaded." It never shows the text, except as pixels in the preview.
- A preview shows the image with every suggested Redact and automatic cover applied, when the helper could decode it.
  Then come "Suggestions only. Recognition can miss text." and the destination.
- **Redact and upload** (the default button) keeps every suggestion, the `warn` kinds found and the
  automatic covers, exactly as the preview shows them. The helper exports and hands off that PNG as an
  automatic cover does, marked as redacted, with no editor step. The resident accepts it only when its
  `covered` counts equal the prompt's `covered` plus its `found`; the operation records them in
  `sensitive.covered`; once every check finished and the upload completed, the notification says
  "Covered …", and **Keep a local copy** keeps the original. It is offered when a `warn` kind was found
  (`sensitive.found` not empty), for each file of a selection too. A check that couldn't finish and found
  only automatic covers, or nothing, doesn't offer it.
- **Open in editor** opens the editor on the same document, with the
  suggested marks already placed and the Redact tool chosen. Nothing is recognized a second time.
  From there it is an ordinary edit: its Upload sends the edited PNG, and its Cancel stores and uploads
  nothing. It is offered for one decodable image within the editor's 24 megapixels. It is not offered
  for an image too large to edit, one the helper couldn't decode, or a selection of several files.
- **Upload anyway** uploads the capture without covering the `warn` kinds. When `redact` kinds were covered, it keeps those covers: the helper takes off the `warn` suggestions, exports and hands off the covered PNG. Otherwise it uploads the original.
- The body adds "put.gd already covered a card number." when `redact` kinds were covered, and the preview shows those covers with the `warn` suggestions.
- **Cancel**, Escape or closing the panel uploads nothing. With **Keep a local copy** on, the capture is
  saved to the archive and History as a local item. Otherwise nothing is kept.
- For a file-manager selection, the panel names the file ("File 2 of 3: receipt.png", counted among
  the PNG and JPEG files of the selection) and offers **Redact and upload**, **Upload anyway** and
  **Cancel**, without **Open in editor**. Redact and upload covers that file; Upload anyway keeps its
  automatic covers and leaves the `warn` suggestions off. Either goes on checking the rest, which can ask
  again. Cancel uploads none of the files. A file chosen twice is checked at each place, and a place that
  asks keeps its own answer. If it changed between those
  two checks and a place stays uncovered, the upload fails as a changed file (`file_changed`). A selection
  of one decodable image within 24 megapixels offers all four; one that is too large or couldn't be
  read offers only Upload anyway and Cancel.

### Agents

`action.invoke` and `upload.files` take `sensitiveText`: `prompt` or `report`. The tray, hotkeys,
the main window and the file manager's menu entry (which names its installation) ask a person, with
`prompt`. Any other request defaults to `report`, which covers `putgd-cli`, MCP and scripts. In report
mode no panel opens: the operation waits in `sensitive_text_found` for `operation.decide {id, choice,
prompt?}`, where `choice` is `redact_upload`, `edit`, `upload` or `cancel` (MCP `decide_upload`). The earlier
`redact` is refused with `invalid_request`. The same method also
settles a panel that a person is looking at.

- **Waiting.** Each prompt waits up to 5 minutes in report mode, then is taken as Cancel. With **Keep a
  local copy** on, put.gd saves the capture locally: the operation ends `completed` with `local_only`
  items, or `failed` if that save fails. Otherwise it ends `cancelled`. `cancel` from `operation.decide`
  does the same. `operation.cancel` on a waiting capture is different: it cancels the operation and
  saves nothing.
- **The reply.** `operation.decide` acknowledges the choice (`{decided: choice}`); poll
  `operation.inspect` for what follows. After `redact_upload`, `upload` or `cancel` the phase leaves
  `sensitive_text_found`, to `checking` while more files are checked (its `sensitive` can still show
  the last prompt's findings for a moment) and then on to the upload or the end. After `edit` the
  phase becomes `editing`, the `editor.*` methods drive the editor by the same ID, and `editor.submit`
  (MCP `submit_editor`) sends the edited image.
- **File selections.** A selection can ask once per file. `sensitive.prompt` numbers each prompt. Pass
  it as `prompt` when answering, so a decision never reaches a later file's prompt.
- **Refusals.**
  - `sensitive_check_running` (`details.operationId`) while the check runs.
  - `sensitive_decision_unavailable` when nothing waits. It has `details.operationId` when the resident
    answers, and no details when the decision named another prompt.
  - `editor_finalizing` for a second decision that reached the panel after the first.
  - `editor_busy` in report mode for a decision sent while another is still being answered: the
    resident forwards one at a time, and accepts only the outcome of the decision it forwarded. Wait for
    the first reply, inspect the operation, then answer its current prompt.
  - `sensitive_choice_unavailable` for `edit` where Open in editor isn't offered, or `redact_upload`
    where no `warn` kind was found (`sensitive.found` is empty).

No method waits on the decision, so nothing hangs.

### Privacy

The automatic check keeps recognized text inside its helper processes: the editor helper, and the
pattern child, which gets the text over a private pipe when there are patterns. It doesn't store, log
or upload the text as text. The image itself is what uploads or is saved locally, depending on the
choice. The resident gets findings only: kinds with counts, whether the check finished and why not, and
the prompt number. It logs the check's start, outcome, findings and duration. The journal,
observations, `operation.inspect` and diagnostics carry the same findings and nothing more, up to
the decision; `operation.decide` answers only `decided` and `operationId`. The panel shows kinds and counts. After **Open in editor** the
check's document becomes an ordinary editor. When the check placed no marks (it couldn't finish, and
found nothing), the editor still holds the check's recognition result, and `editor.inspect` can return
those words to the same-user caller that asks, without another recognition run. Placing any suggested
mark starts a new recognition revision, which clears the result. The notification says only that a capture is waiting.
Tests and evidence use only test values.

### Cost

Each checked capture adds the time to stage it, start the helper, recognize it and match it,
before its upload starts. The table gives the time this added to an uploaded link with today's
engine, measured on the five lab desktops on 2026-11-21 with #1806's final candidate builds. Region
times are medians of five warm runs; full screen is one run each.

| Desktop | Region | Extra time | Full screen | Extra time |
| --- | --- | --- | --- | --- |
| Mint 22.3 Cinnamon, X11 (m74) | 676 × 138 | +0.38 s | 1920 × 1080 | +0.50 s |
| Kubuntu 26.04 Plasma, Wayland (k72) | 676 × 138 | +0.34 s | 1920 × 1080 | +0.42 s |
| Fedora 44 GNOME, Wayland (f44) | 676 × 178 | +0.23 s | 1920 × 1080 | +0.46 s |
| Omarchy Hyprland guest (d69) | 676 × 178 | +0.29 s | 1280 × 800 | +0.31 s |
| Fedora 45 Budgie labwc guest (d69) | 676 × 178 | +0.25 s | 1280 × 800 | +0.33 s |

A screen full of dense text takes longer, as recognition does ([time budget](#time-budget)). The work on
a faster recognizer, [#1808](https://github.com/patrick-hudson/put.gd-desktop/issues/1808) and
[#1812](https://github.com/patrick-hudson/put.gd-desktop/issues/1812), will shorten it. A capture that is
not checked (the option off, Edit & upload, a local-only capture) costs nothing.

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
| `text.inspect` | `id` | `state`, `source`. Once ended: `characterCount`, `codeCount`, `copyStatus`, `truncated`, `banded`, `languagesUsed`, `languagesSkipped`, `timings` (`spawnMs`, `engineStartMs`, `recognizeMs`, `copyMs`, `totalMs`) and `error`. While the result is kept also `text`, `words` `[{text, rect: [x,y,w,h], confidence}]` and `codes` `[{format, kind: "text", "binary" or "withheld", text, length, quad}]` in source pixels (a withheld code, #1941, names its secret's kind in `withheld` and has no `text`; `schemas/text-recognition.schema.json` gives it as `withheldCode`, which only session replies carry). Afterwards `resultExpired: true` |
| `text.cancel` | `id` | The terminal record. Answers once the running job has stopped, up to 35 s. Nothing is copied |
| `capabilities` | none | A `text` block: `available`, `error` with the install hint or the missing confinement, `engines`, `landlockAbi`, `languages` `[{id, bytes, kind}]`, and `sources` with `available` and `error` for each source. It starts no helper |
| `editor.recognize` | `id` | `state` and `result`. Starts recognition for the current revision. `editor.inspect` gains a `text` block |
| `editor.selectText` | `id`, `result`, and `from` with `to` (word indexes in reading order), or `all`, or `none` | The selection |
| `editor.copyText` | `id`, `result`, `all?` (default false, the current selection) | `copyStatus` and the character count |
| `editor.copyCode`, `editor.openCode` | `id`, `result`, `index` | `copyStatus`, or the launch result. Open is refused for anything but a valid http or https URL |
| `editor.apply` command `redactText` | `categories` (1 to 7 of email, phone, ip, card, key, name, list); optional `terms` (1 to 200 words or phrases of 1 to 100 code points on one line, each with a letter or digit) and `patterns` (1 to 50 regular expressions of up to 500 code points on one line), both only with `list` in `categories` | Created shape IDs and counts per category, and `skippedPatterns` (the 0-based indexes in the combined saved and given patterns of any that did not finish at the deadline, did not compile or hit PCRE2's limits) and `nameUnchecked: true` when `name` was chosen and the account lookup did not answer within 1 second when there are any. `terms` and `patterns` add to the saved list for this command only. A term or pattern that is empty, too long or has a line break, or a pattern that is blank, does not compile or matches empty text, refuses the whole command with `edit_command_invalid` naming the entry's index, never its text (through MCP, an invalid field or value is refused first as `invalid_request`, and only a pattern that does not compile or matches empty text reaches the editor's indexed error). It must be the only command in its batch, or the batch is refused with `edit_command_invalid` before any recognition. It answers when it is done, not at once, within the recognition deadline plus a margin |
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
| Command band | **Copy text**, at the far end of the band's second group, before Upload clipboard; its label never shortens. The glyph `copy-text` is a letter T inside selection corners. Its tooltip says "Select a region and copy the text in it. Nothing is saved or uploaded." It shows no output line, and its unavailable badge follows `text.sources.region` |
| Tray, both implementations | **Copy text from region**, in the last command group before Upload clipboard. Native menu ID 41, with no output row |
| Settings, Hotkeys | A row **Copy text from region** in the last group, before Upload clipboard |
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
| pacman | `tesseract-data-eng`, `tesseract`, `zxing-cpp`, the data first so that pacman does not ask for a `tessdata` provider ([release](../release/release.md#text-recognition-dependencies-1428)) | The same |

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
- **Emoji templates** (#1748) are built into `putgd-ocr` from
  `src/ocr/emoji/templates.bin` (16,576,768 bytes), so no package installs a separate file and
  the helper opens none. Every package carries their notices under
  `/usr/share/doc/putgd/emoji/`: `OFL.txt` for Noto Color Emoji, `CC-BY-4.0.txt` for Twemoji
  Mozilla, `UNICODE-LICENSE.txt` for the emoji sequences of Unicode's `emoji-test.txt`, and
  `SOURCE.md`. The AppImage's notices list them as parts of put.gd.
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

**Memory after the workload, measured on candidate 357210d2f** (CI run 43448444706) on m74, with the
workload of 20 monitor jobs and 5 driven region jobs and a 180 second wait. All 25 helpers exited 0 and
none remained, and the lists of forbidden libraries were empty in the resident's maps (`libgomp` is
allowed). Both packages passed the unchanged frozen Gate C evaluator:

| Package on m74 | Resident RSS | Resident PSS |
| --- | --- | --- |
| Native DEB | 10,112 KiB | 6,990 KiB |
| AppImage | 10,804 KiB | 8,973 KiB |

The same workload on nine other cells, run on the earlier candidate 8a3294c3c (CI run 43423721988) and
carried forward, also saw 25 of 25 helpers exit 0, none of `libtesseract`, `libleptonica`, `libZXing`,
`libcurl` or `libarchive` mapped in the resident, and passed the same evaluator. Their resident RSS ranged
from 8,980 to 11,580 KiB and PSS from 4,703 to 7,751 KiB (f44 RPM and AppImage, k72 DEB and AppImage,
Aurora AppImage, an Ubuntu GNOME DEB and AppImage, and d69 DEB and AppImage). They are not a new
180 second run on the final build.

**Helper peak.** Every peak recorded on these candidates, in the table below, was below 131,072 KiB (128 MiB, the budget then), before the allowance for extra
languages, with English and with three languages, on every engine build. The highest corpus peak was
126,064 KiB (Ubuntu 26.04). Arch's original 23 MB model is measured separately, and its corpus value is a
conservative upper bound from a fresh child's rusage. Everything else is the kernel's VmHWM after exec.
Peaks in KiB:

| Engine build | Corpus | Dense 24 MP, English | Dense 24 MP, three languages | Editor output, English | Editor output, three languages |
| --- | --- | --- | --- | --- | --- |
| Mint 22 and Ubuntu 24.04 | 123,404 | 104,776 | 102,756 | 105,272 | 102,836 |
| Ubuntu 26.04 | 126,064 | 106,680 | 105,552 | 108,108 | 105,180 |
| Debian 13 | 125,304 | 106,200 | 104,740 | 106,824 | 104,644 |
| Fedora 44 | 106,812 | 91,240 | 105,512 | 91,148 | 105,008 |
| Arch, original model | 113,552 | 102,432 | 111,456 | 101,640 | 111,600 |
| AppImage | 81,740 | 89,364 | 111,512 | 95,988 | 112,228 |

**Latency, recorded and not gated.** On m74, each batch is 30 runs with separate spawn, engine-start,
recognition and copy clocks. A region run starts at the release of the selection and a file run at its
dispatch. The host was a quiet four-CPU machine (95.74 to 99.25 percent idle before timing). p95 in
milliseconds:

| m74 batch | Native package | AppImage |
| --- | --- | --- |
| Paragraph | 658.8 | 520.2 |
| Dense region (8,739 characters) | 3,167.5 | 2,033.3 |
| 4K capture, light | 6,383.2 | 4,128.7 |
| 4K capture, dark | 7,939.2 | 2,749.5 |

The desktop pass recorded five runs of the dense region (994,976 pixels) and of a paragraph on each
cell, as end of selection to the observed clipboard acknowledgement, which includes the client's
polling. The range of the five runs in milliseconds, with the host and the candidate:

| Cell | Candidate | Dense region | Paragraph |
| --- | --- | --- | --- |
| f44 GNOME, RPM | 357210d2f | 2,441.8 to 2,525.6 | 368.5 to 372.8 |
| f44 GNOME, AppImage | 8a3294c3c | 2,926.9 to 3,464.0 | 296.5 to 300.4 |
| k72 Plasma, DEB | 8a3294c3c | 4,814.4 to 6,175.2 | 496.5 to 543.9 |
| k72 Plasma, AppImage | 8a3294c3c | 4,203.9 to 4,842.1 | 458.5 to 528.2 |
| Aurora Plasma, AppImage | 357210d2f | 856.6 to 941.8 | 141.2 to 147.5 |
| Ubuntu GNOME, DEB | 357210d2f | 5,481.5 to 5,588.7 | 496.6 to 552.2 |
| Ubuntu GNOME, AppImage | 8a3294c3c | 4,612.2 to 4,776.0 | 390.8 to 428.1 |
| d69 Plasma, DEB | 357210d2f | 3,038.6 to 3,258.6 | 398.7 to 454.5 |
| d69 Plasma, AppImage | 357210d2f | 1,829.4 to 1,910.7 | 292.0 to 296.7 |

The k72 AppImage rows ran beside another worker's four-CPU build and are qualified. Those hosts differ
in processor and core count, so the rows do not compare packages across hosts.

**Other captures' hotkey-to-selector time.** An A/B of the nightly code baseline (466449a4c) against the
candidate on m74, native, with the frozen probe unchanged, gave two results. The first run, 30 presses
each, measured an on-screen p50 of 181.01 ms against 179.27 ms and a p95 of 199.35 ms against
215.17 ms, which is 15.82 ms (7.94 percent) more. The larger study (A and B in alternating blocks, each
a fresh resident, five warmups, 3 seconds between presses) measured:

| Condition and build | Presses | p50 | p95 | p99 (ms) |
| --- | --- | --- | --- | --- |
| Warm, baseline | 180 | 142.08 | 158.09 | 175.54 |
| Warm, candidate | 180 | 142.01 | 157.87 | 176.97 |
| After 40 s of quiet, baseline | 30 | 137.16 | 158.84 | 168.76 |
| After 40 s of quiet, candidate | 30 | 144.95 | 165.49 | 166.14 |

The difference at p95, candidate minus baseline, is -0.21 ms warm (95 percent bootstrap interval -11.46
to +5.56 ms) and +6.65 ms after quiet (interval -13.66 to +15.46 ms). Both intervals include zero. The study
does not reproduce the first run's +15.82 ms, and it does not show the two builds are equal. After quiet the
candidate's resident took five more minor faults at the median, with no major faults, and its executable is 567,184
bytes larger (11,827,296 against 11,260,112). The study did not resolve a tail shift to one stage. It used an owned
override-redirect X11 window as the canvas because the managed one failed pixel admission, so its absolute times
are not comparable with the first run's.

### Measurements

Recognition costs roughly a millisecond a character on each core. A paragraph takes about half a
second. A region packed with text or a whole 4K screen takes several seconds. These are
measurements of the helper alone, taken on the engine round four code (#1462), not acceptance of a
put.gd build. Times are `TextJob`'s `totalMs` (spawn to the result read back), so they leave out
the parent's grey conversion and the clipboard. Release builds, up to four engines (the cap before
#1812), peak is the helper's VmHWM. m74 is an i5-6500T on Mint 22.3 with distribution Tesseract 5.3.4. k72 uses
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

- Every input in this table stays inside the 128 MiB budget it was measured under. The 320 MiB
  budget of #1808 gives large screens more engines and higher peaks; see Bounds and budgets.
- The dense terminal with a different number of engines on m74: 1 engine 6.72 s (86 MiB), 2
  3.51 s (87 MiB), 3 2.52 s (104 MiB) and 4 2.55 s (120 MiB). Four take no less time than three
  because 8 strips do not divide evenly. With four engines about 3,400 characters are read a
  second on m74 and 6,600 on k72.
- End to end on m74 on the final candidate: see the latency table above.
- Engine start takes 115 to 153 milliseconds on m74 and d69, which is the model load.
- With three languages (eng, deu and fra, 2.6 MB beyond English) the final peaks are in the table above.
  English alone peaked at 106 MiB of `RLIMIT_DATA`'s data against its then 192 MiB limit (m74, measured on the code of #1452; the limit is 480 MiB since #1808).
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

Final result: candidate 357210d2f (CI run 43448444706), all 178 images through the real `putgd-ocr` of each
packaged engine build, in containers and on the native Mint package and the AppImage on m74. Median character
error rate per class, with the bar in brackets:

| Class and size (bar) | Captures | Mint 22, Ubuntu 24.04 | Ubuntu 26.04 | Debian 13 | Fedora 44 | Arch, 23 MB model | AppImage |
| --- | --- | --- | --- | --- | --- | --- | --- |
| browser-github, body (2%) | 24 | 0.147% | 0.147% | 0.147% | 0.147% | 0.147% | 0.147% |
| browser-local, body (2%) | 24 | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% |
| browser-wikipedia, body (2%) | 24 | 1.874% | 1.874% | 1.874% | 1.874% | 1.533% | 1.874% |
| editor-code, body (2%) | 24 | 1.481% | 1.374% | 1.374% | 1.545% | 1.270% | 1.182% |
| monitor, body (2%) | 26 | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% |
| small-ui, body (2%) | 8 | 0.421% | 0.443% | 0.443% | 0.417% | 0.088% | 0.399% |
| small-ui, small (5%) | 16 | 0.089% | 0.089% | 0.089% | 0.067% | 0.067% | 0.067% |
| 24 px pages (2%) | 2 | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% | 0.000% |
| terminal, body (2%) | 30 | 0.000% | 0.000% | 0.000% | 0.000% | 0.262% | 0.000% |
| URLs exact (90%) | | 52 of 52 | 52 of 52 | 52 of 52 | 52 of 52 | 50 of 52 (96.15%) | 52 of 52 |
| Emails exact (90%) | | 52 of 52 | 52 of 52 | 52 of 52 | 52 of 52 | 52 of 52 | 52 of 52 |

Aurora uses the AppImage. Fedora 45's own RPM ([#1509](https://github.com/patrick-hudson/put.gd-desktop/pull/1509)) was not measured separately. Every class median is within its bar on every engine build.

- **Wikipedia.** All 24 Wikipedia transcripts said "squares in a square grid" where the captured pixels
  say "squares within a square grid". The transcripts were corrected before this scoring, and the
  citation markers are not the cause. Against the old transcripts the class scored 2.573%.
- **Whole 4K screens.** The two GNOME 4K captures went from 63 to 64 percent error to about 9.2 to 9.3
  percent once screens were read panel by panel. What remains is words run together in small
  navigation text, a dropped `#` and the order of panels inside the Wikipedia window. The monitor
  class median hides these two, which are 2 of its 26 captures.
- **Before the method changed.** Page segmentation 3 with layout blocks, the first method, scored
  terminal 54.4%, browser-local 31.0%, editor-code 13.6% and Wikipedia 2.6%, and read 24 of 48 URLs and
  34 of 48 emails exactly, on the 174 real captures.
- **German.** On 12 captures (816 words), with Mint's distribution model, the median error was 0%. An
  earlier run of 12 synthetic pages (14, 16 and 20 px, DejaVu Sans and Serif, light and dark, with
  umlauts and ß) scored `deu` 0.00%, `eng+deu` 0.43% and English alone 5.7%.
- **Hidden and hostile content.** Near-white text and 3 px text each returned zero characters, hostile
  prose had no forbidden code points, and eighteen hostile code fixtures returned only a binary format
  and a 41-byte length.
- **A tall two-column page** read with all 90 lines intact, no truncation and the accepted column order.
- **English models.** `tessdata_best` meets every bar at about 3.3 times the time and 15.4 MB, and
  put.gd ships `tessdata_fast`.

Quick redact was scored separately. On candidate 357210d2f it covered 431 of 439 acceptance items: all
eight 14 px pages covered 16 of 17, missing the synthetic GitHub token. The hashes page covered 30 of 30, the
clean pages got no marks, and all 736 marks across the acceptance and held-out pages were opaque black in the
exported PNG. Pull request #1465, which came after that candidate, makes a long run of two or three repeated
characters count as a token and requires a leading "+" for a group of more than ten unbroken digits to count as
a phone number. With it, through the real editor and helper on k72 (Tesseract 5.5.0, offscreen, not a
desktop), every one of the 24 item pages covered 17 of 17, the hashes page 30 of 30, the clean pages and the
24 negative pages got 0 marks, the realistic random tokens 456 of 456, and the held-out pages 303 of 304.

### Emoji quality and cost

Measured for [#1748](https://github.com/patrick-hudson/put.gd-desktop/issues/1748) through the
whole pipeline (release build, Tesseract 5.5.0, d69) on PR #1751's build at `648946bf3`. The
commits after it keep a scaled capture's pixels, leave emoji inside decoded codes out, give a
line joined at a band seam its later band's direction, and change where emoji go in edge cases;
at `a1f63c3fc` the named emoji are the same on these sets. "Common" is a fixed
list of 200 emoji people use most; the rest were drawn at random from all 3,781.

| Set | Emoji | Common named right | Common wrong | Others right | Named where there was none |
| --- | --- | --- | --- | --- | --- |
| 60 pages drawn by Pango, Noto 2.051 and Twemoji, 11 to 32 px, light and dark, grey and subpixel antialiasing | 579 | 361 of 413 | 9 | 105 of 166 | 0 |
| 40 Firefox pages (Twemoji), five themes, five fonts, 12 to 32 px | 444 | 270 of 330 | 4 | 77 of 114 | 0 |
| 20 Firefox pages on saturated chat-bubble colours (blue, green, purple) | 224 | 138 of 169 | 3 | 39 of 55 | 0 |
| The 179 corpus captures, which hold no emoji | 0 | | | | 0 |

The emoji scan alone, on 181 common emoji in "Ab 😂 cd" lines at 13, 16, 20 and 26 px, light
and dark (1,448 emoji per set), as each desktop draws them:

| Drawn by | Named right | Wrong |
| --- | --- | --- |
| Qt 6.11 from Noto 2.057 (Plasma on Fedora 44 and Aurora) | 1,289 | 17 |
| Pango from Noto 2.057 (GNOME on Fedora 44) | 1,290 | 20 |
| Pango from Noto 2.047 (Linux Mint 22) | 1,282 | 24 |

With only the Noto 2.051 and Twemoji templates, the first two sets had 1,195 and 1,199 right
and 54 and 56 wrong, and the third 1,268 right.

- On the Pango pages the character error of the copied text fell from 20.0% (median) to 4.6%.
- What is missed is mostly emoji with no colour (👀, 💀, 🖤, 🤍, 💬), plain shapes (🔴), small
  flags, and some rare ones. What is named wrong is mostly a neighbouring skin tone, the man,
  woman or person form of the same emoji, ❣️ read as ❤️, and 🇺🇸 as 🇺🇲, which Noto draws alike.
- The limit of 0.09 named 16 more emoji right than 0.08 on the three page sets, and 4 more
  wrong; 0.10 named a 🌐 on three corpus captures.
- The scan took a median 26 to 38 milliseconds on these sets, at most 0.15 seconds on an emoji
  page, and a median 40 milliseconds and at most 0.8 seconds on the corpus (a dense coloured
  terminal and full screens). The corpus's copied text was the same with emoji on and off.
- **The helper's peak.** On k72 (Kubuntu 26.04, Tesseract 5.5.0, release builds, the median of
  three runs each), the peak resident size against the nightly head, in MiB: a paragraph with
  emoji 48.4 against 48.2, a Firefox page 78.5 against 78.4, coloured code 102.3 against 102.2,
  the dense terminal 123.0 against 122.7, a GNOME 4K screen 93.3 against 92.0, a 4K page of about
  600 emoji 91.5 against 89.1, 24 megapixels of colour noise 111.8 against 124.0, and a dense
  24-megapixel page of text 134.9 against 134.3 (127.5 to 134.9 run to run). The largest increase
  among these medians is 2.4 MiB. The time added is 34 ms on a
  paragraph, 0.3 seconds on coloured code, 0.5 seconds on the dense terminal, 0.6 seconds on a
  4K screen and 1.4 seconds on the 4K emoji page. The dense 24-megapixel page is above the 128
  MiB budget on the nightly head too.

## Known limits

- Recognition can misread. Small, unusual or low-contrast type, superscripts and a busy background
  produce errors. Check what you paste.
- A capture with a lot of text is returned cut at the 16 second budget and marked as cut.
- A rotated Micro QR code at 3 pixels a module is not read. The same code unrotated at 3 pixels, or rotated at
  5 pixels, is read.
- A code with no quiet zone, such as a dark one that a white page touches, is not read. The same
  code with a black frame is. On the busy multi-code page the original read 3 of 4 codes and the framed
  page 4 of 4. Of 183 contract-safe code fixtures, 182 decoded.
- Quick redact suggests only. `name` and `list` find only the spellings described above, so a
  nickname or a misread beyond the folds is missed. A hit covers the whole word box, so more than the name is covered. A pattern that
  backtracks too long is skipped and named. A number made of repeated digits is miscounted by recognition (a
  16-digit card number of repeated digits at 13 px was counted wrong at every scale tried), and a card
  number read with one digit too many fails the Luhn check, so those can be missed. Held-out pages
  covered 303 of 304 items.
- On d69's real-recognition corpus (660 pages with a token ending a line above two card lines,
  in 12 fonts, 11 sizes and 5 backgrounds), 139 more second card lines were left uncovered after
  #1863 (#1856) than before. Recognition had misread those card numbers so they fail the Luhn check, and the
  token's match used to take them in. The same misread card elsewhere on a page was missed
  before too. In that corpus, the pages where one match spanned two kinds fell from 660 to 41,
  and the items not found as their own kind from 1,360 to 285.
- A mark's padding, about a quarter of the matched row's recognized box height above and below,
  can touch ink of a tightly set line below. In the #1863 terminal fixture on m74 and k72 it
  touched 8 ink pixels at the top of "later", and "later" stayed readable on both hosts.
- A tall two-column page below 4 megapixels is read as one block, and a banded image has no global
  multi-column order.
- One extra timing run of the AppImage on k72 had a selector that did not finish within 40 seconds before
  any helper started. No frame was kept and the cause is unresolved, product or selector readiness. It did
  not recur in a repeat of five paragraph runs with selector frames kept, which completed in 273.7 to 440.2
  ms.
- The hotkey-to-selector time of the other captures was measured twice with different results (see
  [performance and memory](#performance-and-memory)), and the larger study does not resolve a difference.
- On d69, Copy text from the first KDE session after installing the package returned `capture_permission`
  until the session was set up again: a fresh login and a refresh of the KDE application menu cache in the
  session's own environment (`XDG_MENU_PREFIX=plasma-`), and a restart of the KDE portal services. The driver
  had omitted that variable and generated the wrong `applications.menu` cache. Which of the steps was needed
  was not isolated.
- Forced delivery of an obsolete editor result, one that arrives after the revision changed, was not
  driven on a live desktop. A test in CI covers it.
- `history.qr` with `show` has no agent close for the window. A second show replaces it, and
  closing History closes it.
- With the matching kind chosen, Quick redact and the upload check can leave these visible. Each was found by
  the [sensitive-text fuzz harness](../lab/redact-fuzz.md) (#1931) and kept as a limit:
  - An IP address that recognition misreads: a comma read for a dot (`192.168,36.135`), a digit
    inserted (`192.168.408.230`), or a character inserted into an IPv6 group so that it is longer than four
    (`fdOO0::2efb` for `fd00::2efb`). In 6,500 rendered scenes this left 270 addresses visible, mostly in 11 to
    13 px monospace logs.
  - Text that recognition doesn't find at all. Grey text on light grey at about 2.6:1 contrast is often not
    read, though people can read it (it is below the WCAG minimum), so nothing in it is checked.
  - A phone number in a right-to-left paragraph with no direction marks. It is shown with its digit groups in
    reverse order (`5388 555 (210) 1+` for `+1 (210) 555 5388`), and only part of it is covered.
  - One-time codes, licence and product keys, a card's CVC, and the path of a share link. No built-in rule
    looks for them, since none has a shape of its own; a word or pattern in the user's redaction list can
    still find one.
- Emoji with no colour (👀, 💀, 🖤, 🤍 and 💬 as Noto draws them) are not found. In desktop
  acceptance, 🇬🇧 at the tested editors' default size and Noto's ☕ on white at 150% zoom were
  missed too. Plain-shape emoji (🔴, 🟩, keycaps, ➡️) are never named, and emoji that an app
  draws in its own design are named only where they look like Noto's or Twemoji's. Neighbouring
  skin tones, and the man, woman and person forms of one emoji, are sometimes swapped at text
  size. See [emoji](#emoji).
- Searching History by recognized text, recognition in the background, in-app language downloads,
  translation, handwriting, linear barcodes and PDF417 are out of scope as final decisions.
