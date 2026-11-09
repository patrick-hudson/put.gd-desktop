# Screen recording: history posters

Part of [Screen recording](../recording.md).

## History poster frames

While stored previews are on (`retention.previewCacheBytes` above 0, the
default), History shows a still for every stored video (#112): screen
recordings and MP4, WebM, Matroska, QuickTime, AVI and Ogg files shared from
the clipboard or a file manager. At 0 no poster is made for a new operation or on request; an operation already running keeps the value it started with. The list draws a play glyph and the length over it, so a still
is never mistaken for a screenshot; the inspector shows it larger, with the
length beside the size. GIF and WebP recordings are images and keep the image
preview (their first frame, through the image job). Animated WebP reads
through Qt's WebP image plugin, which Debian, Ubuntu and Fedora package apart
from Qt GUI, so the DEB requires `qt6-image-formats-plugins` and the RPM
`qt6-qtimageformats` (#189); the AppImage bundles `libqwebp.so` from the
pinned Qt.

The poster is made after the operation ends, so a slow or failing decode
never delays the upload, the copied link or the copied result. The resident
queues one item at a time and, for each video item without a preview, runs
`putgd-record --poster MAX_PIXELS` (`PosterJob`, `src/core/PosterJob.*`):

- the resident opens the stored artifact itself (`O_NOFOLLOW`, regular files
  only) and hands that descriptor over as the child's standard input; the
  child reads only it, through `/proc/self/fd/0`;
- the child runs at nice 10 with 2 GiB of address space (none under
  sanitizers), 21 s of CPU time, 64 file descriptors, no core file and a
  zero file-size limit, and is killed at the 10 s deadline or on shutdown;
- FFmpeg may use only the `file` protocol and the MP4/QuickTime,
  Matroska/WebM, AVI and Ogg demuxers, so a crafted file can't make it open a
  playlist, a URL or another file; frames over `limits.imagePixels` are
  refused before decoding (`max_pixels` also bounds every later frame);
- the PNG on standard output (long edge 256 px, the length in the
  `putgd-duration-ms` text chunk) is capped at 2 MiB and checked for the PNG
  signature and size before it is cached.

The poster comes from the file as stored, so blur and cover masks baked into
an export are in it (`poster_contract` checks a covered quarter). The frame is
1 s in (half-way into shorter clips); when that frame is flat (black, a fade
or a solid fill) the helper tries 25% and 50% in, then the first decodable
frame. Posters share the image previews' cache (`<data>/previews/ID.png`,
owner-only, `retention.previewCacheBytes`, cleared by cleanup), are recorded
as the item's `previewPath` and reach `history.list`, `history.detail`, CLI
`inspect` and MCP like image previews. A missing helper or codec, or a corrupt,
truncated past its first frame or oversized file, leaves the item without a
poster and a `No preview` log record with the failure; the AppImage's FFmpeg has
no H.265 decoder, so H.265 MP4 files have no poster there.

### Previews made on selection

A video stored before posters existed, one whose poster failed and one whose
poster cleanup removed get their poster when History selects them (#190). So
does an image whose preview was never made or was removed, such as a WebP
recording stored before the WebP plugin was installed. History waits 400 ms
for the selection to settle, shows "Making a preview…" and sends
`history.preview {id}`, one request at a time. History reloads the selected
item whenever the resident reports a change; the preview it already shows
stays until a reload brings a different preview file, so the inspector never
flashes empty (#285). The resident:

- answers `ready` at once when the item's preview file exists;
- makes it from the retained artifact, or else from the local copy when that
  is still the same file (regular, not a symlink, the artifact's size and
  SHA-256, checked within 10 s), through the same poster queue: a selection
  goes ahead of after-operation posters, at most 3 wait, and one that waited
  20 s is refused as `preview_busy`, so the reply comes within 45 s
  (`historyPreviewReplyMs`);
- records the preview on the item, as an after-operation poster is, and
  answers `made` with `previewPath` and `source` (`artifact` or `local`).

Every failure is soft: the inspector's preview area says "Couldn't make a
preview" with the error's message, or names the cause ("Previews are turned off"). For a file type
with no preview, the area is replaced by "No preview for this file type". For
`preview_source_missing` it is hidden, and the inspector's local-copy hint
explains what is gone. The refusals are `preview_source_missing` (neither
file is left), `preview_source_changed` (the local copy differs),
`preview_unsupported` (not an image or video), `previews_disabled`
(`retention.previewCacheBytes` is 0), `operation_active`, `preview_busy` and
the decoder's own errors (`poster_failed`, `poster_timeout`,
`input_excessive`, `poster_helper_missing`, `preview_failed`). A selection
that failed is not retried automatically: the resident remembers the failure
until it restarts, and History does not ask again in that window. The
inspector's **Retry preview** button sends `history.preview {id, retry: true}`,
which forgets the failure and tries again in the same session; it is not
offered for `preview_unsupported`, `preview_source_missing` or
`previews_disabled`. The CLI (`putgd-cli history.preview '{"id":"..."}'`) and
MCP (`make_history_preview`) make the same request and take the same `retry`.

Measured on d69 (Debian 13, FFmpeg 7.1, i5-6500T, load 1.2), three runs each
with identical results, through the same limits:

| Clip | Wall | Peak RSS | Peak address space |
| --- | --- | --- | --- |
| 1080p, 60 s, H.264 (x264 ultrafast) | 0.17 s | 78 MiB | 515 MiB |
| 1440p, 20 s, VP9 (libvpx realtime) | 0.29 s | 100 MiB | 509 MiB |
| 2160p, 20 s, H.265 (x265 ultrafast) | 0.62 s | 186 MiB | 570 MiB |

A 720p recording's poster took about 0.25 s on f44, most of it process
start-up. The PNG is 12 KiB for these clips.
