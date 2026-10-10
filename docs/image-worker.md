# Temporary image worker source contract

This contract describes implementation and repeatable checks. Desktop feature parity and memory acceptance require the exact current build on the supported compositor; host tests alone do not establish either.

`putgd-ui --image-job` initializes the native `QApplication` and private parent pipes, then branches before VisualStyle, resident/session communication, configuration, providers, credentials, or windows. It uses the existing `UiControl` transport. The executable is the installed/build sibling of the calling binary; tests may explicitly supply a helper path and a shorter deadline through the `ImageJob` constructor. Production reads no helper-path or fault-mode environment override.

The Core caller is synchronous and belongs on an acquisition/background worker. Discovery returns the Qt-supported MIME list in its original order, including aliases and duplicates. Decode returns a `QImage`, using explicit PNG decoding in the parent after the GUI child has converted the input. Preview returns PNG bytes with the existing 256×256 aspect-preserving smooth scaling, including upscaling. The original-image pixel admission limit does not incorrectly limit the resulting thumbnail pixel count.

Each invocation creates a fresh 0700 `/tmp/pgi-XXXXXX` directory. Input is a single owned 0400 regular file; output is an owned 0600 regular file, initially empty. Both require one hard link. The parent retains descriptors, and the child opens them through an owned pinned directory descriptor with `O_NOFOLLOW`. File identities, modes and named paths are checked; input is hashed before work and before success. Replacing an input pathname while retaining the original open descriptor therefore fails closed. The parent validates final output identity, length, digest, PNG signature and pixel metadata only after authenticated completion and normal zero child exit. Cleanup refuses to remove a replacement directory.

The outer UiControl frame retains its existing version, strict increasing sequence, known message types, size and rate limits. Payloads have an additional fixed role `image-job`, a canonical UUID nonce and generation string `1`.

1. `bootstrap`: exactly eight keys: role, nonce, generation, build, task, input, output, limits. Task is discover/decode/preview. Build equals the current compiled manifest. Input is null for discovery; otherwise input/output descriptors contain path, device, inode, bytes and sha256. Numeric descriptor and limit values are canonical nonnegative decimal strings. Limits contain imagePixels, imageBytes and outputBytes.
2. `ready`: exactly four keys: the three-key envelope plus matching build.
3. `activate`: exactly the three-key envelope.
4. `checkpoint`: success has exactly eight keys: envelope, ok=true, bytes, sha256, width, height. Discovery uses width=height=0. Failure has exactly five keys: envelope, ok=false, code. Permitted child failure codes are input_excessive, source_unsupported and image_job_io.
5. `checkpointAccepted`: exactly the three-key envelope. Only then does the child return normal zero. A valid checkpoint followed by a crash, nonzero exit or no exit remains a failure.

Image bytes and MIME lists are stored in the private output file; control frames contain metadata only. Errors have fixed messages, without paths, payloads or plugin diagnostics. Stderr is discarded and capped at 1 MiB. Discovery is capped at 1024 entries, 4096 bytes per MIME value and 32 MiB output. Preview output is capped at 256 pixels per axis. Private file writes fail before crossing their admitted output byte budget.

The independent parent deadline starts before staging and remains fixed for the entire operation, at most ten seconds. Atomic cancellation is checked between bounded staging/hash chunks and at most 50-ms process waits. On failure, the parent stops the protocol, terminates the exact child, waits 500 ms, then kills and waits 1500 ms if necessary. The child timer does not claim to interrupt blocking codec work; the parent is authoritative. An uninterruptible kernel process cannot be guaranteed to reap within an arbitrary userspace interval, and QProcess ownership retains that limitation rather than abandoning an active helper.

Qt's existing image allocation policy is retained. Only generic discovery/decode/preview go through this worker; the approved direct PNG path remains in the callers. Current RSS improvement is unknown until measured. No standalone region protocol, native output geometry or sibling capture route changes are part of this five-file implementation.

## Reproduce the checks

Build with the standard `cmake --preset dev` and `cmake --build --preset dev`. Run `ctest --preset dev -R '^(image_job_integration|payload_contract|kde_ownership_integration)$' --output-junit image-job-results.xml`. Save the command, clean Git commit and tree, generated build manifest, Qt version, test registration, complete stdout/stderr and JUnit. Never include credentials or production image bytes in those artifacts.

The integration test uses a Core-only parent and the actual product helper for PNG, BMP, JPEG, content-based regular-file previews and font-bearing SVG. A missing optional JPEG encoder is an explicit skip. Host Qt installations without SVG may skip both SVG tests; compositor acceptance requires these cases to pass on the actual distro codec installation. No dependency installation or substitute codec is performed by the test.

An independent C++ fixture exercises invalid envelopes/builds, duplicate results, crashes, nonzero exits, timeouts, private-file replacement/permissions/hard links/digests and cancellation. Cancellation waits for a recorded child readiness barrier. Each fixture case checks that its exact child and private job directory are absent after return. Test-only fault environment variables are read exclusively by that fixture, never the production worker. The suite does not claim that a Qt timer can interrupt codec code.

## KDE integration

Standard text, local URI and PNG offers retain their direct paths. Unknown MIME aliases trigger discovery on an acquisition thread. Non-PNG bytes use the temporary GUI worker, including a PNG-labelled offer whose actual content is SVG. Request identity and generation are checked before and after Wayland event dispatch; cancellation or a newer owner vetoes the result. No codec or GUI font service is initialized by this generic path inside the Core resident.

KDE full-output composition shares a single immutable frame only when selection equals its entire valid output geometry, density/identity/native size are valid, the pixel budget admits the image, and the original outward crop would cover every pixel. Partial, fractional or invalid cases delegate to the unchanged shared compositor. QImage copy-on-write keeps later source changes from changing the captured result. A 1280×800 ARGB frame avoids one 4,096,000-byte allocation; retained RSS savings must still be measured. Cinnamon and GNOME composition are unchanged.

Repeat native KDE tests with the visible VNC desktop, synthetic payloads and independent paste/pixel witnesses. Include SVG clipboard, SVG bytes advertised as PNG and a regular SVG file preview, then the unchanged native workload and normal helper-exit checks. Drain all fixtures before measuring. Record every added operation in the workload and keep the frozen sampler/evaluator unchanged. Once KDE passes its functional and memory gates, run the exact candidate's Cinnamon and GNOME regressions.

The native worker uses Unix file descriptors. Non-Unix residents retain their existing in-process generic preview route. The new process-cleanup integration suite is registered on Linux, where its `/proc` child-absence assertion is valid. These guards preserve source portability; they do not establish macOS or Windows desktop acceptance.
