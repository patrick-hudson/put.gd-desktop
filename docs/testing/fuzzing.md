# Fuzzing the process boundaries

libFuzzer targets for the code that decodes what another process sends put.gd: the update
signature check, the framed JSON on helper pipes and the session socket, the worker and
helper protocols, the files read back after a crash and the replies of compositors and
the GNOME Shell extension. They live in `tests/fuzz/` and are built only with clang.

## What is covered

| Target | Code under test | Properties checked beyond "does not crash" |
| --- | --- | --- |
| `fuzz_minisign` | `src/cli/Minisign.cpp` | A genuine signature verifies. One changed byte of the message, the key, the signature, the global signature or the trusted comment is refused. Whatever is accepted is exactly what was signed. |
| `fuzz_frames` | `UiControl`, `CaptureControl`, `StdioDevice`, `SessionProtocol::decodeReply` | Input cut at any point, repeated past the frame limit, over real pipes; a failure is reported once and no frame follows it; a reply built for a request id decodes. |
| `fuzz_localipc` | `src/app/LocalIpc.cpp` | Both ends of the session socket against a peer that sends the bytes in pieces: a reply is one frame within its limit, or an error and nothing. |
| `fuzz_s3wire` | `S3WorkerProtocol`, `S3Journal.h`, `PublicUrl` | A decoded value encodes and decodes to the same bytes. |
| `fuzz_quickredact` | `src/editor/QuickRedact.cpp` (`find`, `marks`) | Any text laid out as recognized words: every match names existing words, in order and once; no word is in two matches; every mark stays inside its bounds. |
| `fuzz_textjob` | `TextJobProtocol` | Sanitized text has no control, bidirectional or invisible characters; what decodes stays inside the image and the byte caps. |
| `fuzz_settings` | `Settings::validate`, `fromValidated`, `normalized` | A document that validates builds settings that validate again. |
| `fuzz_journal` | `Operation`, `OperationItem`, `StorageReceipt` | What a damaged journal row reads as is written out and read again unchanged. |
| `fuzz_gnome` | `GnomeProtocol::decodeTransfer` | Image bytes always start with a PNG header, so the decoder never starts the image helper process. |
| `fuzz_hyprland` | `HyprlandIpc` | `luaString` always yields one Lua string literal; replies and event lines of any shape. |
| `fuzz_httpimport` | `src/storage/http`: `importSxcu`, `validateSettings`, `namedCredentials`, the placeholder engine, `scrubStoredValues`, and `importUploaderFile` end to end against a real settings file | A refused `.sxcu` hands nothing on. What an import returns passes `validateSettings` (once plain HTTP is allowed) and `namedCredentials`. A credential-like stored value (20 to 200 letters, digits, `-` and `_`, with a letter and a digit), a key the import took out of an address and the value of a row that is not sent included, does not appear whole, at word boundaries, anywhere in the settings, notes or name. Text written with `escapeTemplate` renders back unchanged. A stored credential does not survive `scrubStoredValues`. An import answers once, and the profile it names is in the configuration. |

What `fuzz_httpimport` does not check:

- A stored value that is short or has other characters (a password with spaces, a
  six-letter key), or one longer than 200 characters, is not looked for in the settings.
  A value is found only when it appears whole, at word boundaries: part of it, or the
  whole value glued to other letters or digits, is not looked for.
- Row values are taken from the `Headers`, `Parameters` and `Arguments` objects the import
  reads, left-out rows included. A query value in the request URL that a `Parameters` row
  replaces, and a `Data` body that is not sent, are not looked for separately; the unit
  tests cover them.
- "Answers once" means one answer by the time the import has answered and one more pass
  of the event loop has run, not that no answer can ever follow.
- `importUploaderFile` runs with session credentials only, in a new scratch directory
  for each input, and reads only files in that directory. The keychain path is not
  fuzzed; a run that reaches for the keychain stops.

#1570 (a key the import took out of an address kept in clear text in the thumbnail URL
or error message) was found by this target and is fixed. Its file and the shapes found in
review (a key in a host or a placeholder, a replaced query value, a row name holding its
value) are in the corpus.

## Results so far

| Target | Run | Edges at start, at end | Findings |
| --- | --- | --- | --- |
| `fuzz_journal` | 2 h 04 min, 7.6 million inputs, 3 workers | 1625 to 1638, flat for more than 90 minutes | #1564 (first round, fixed) |
| `fuzz_settings` | 2 h 04 min, 122 million inputs, 3 workers | 2099 to 2113, flat for more than 90 minutes | none |
| `fuzz_httpimport` | about 5.5 worker-hours, 9.6 million inputs | 7870 to 9420, still rising slowly | #1570 |

More time on `fuzz_journal` and `fuzz_settings` is not worth it: their edges stop rising. Spend it
on `fuzz_httpimport` and on a new target before re-running these two.

## Build and run

```
cmake --preset clang-asan -DPUTGD_FUZZ_INSTRUMENT=ON
cmake --build build/clang-asan --target fuzz_minisign   # or any fuzz_<name>
mkdir -p run/minisign && cd run/minisign
../../build/clang-asan/fuzz_minisign -dict=../../tests/fuzz/dict/minisign.dict \
  -max_len=2048 -timeout=5 -rss_limit_mb=2048 -malloc_limit_mb=1024 \
  corpus ../../tests/fuzz/corpus/minisign
```

`PUTGD_FUZZ` (on in the `clang-asan` preset) builds the targets and registers a ctest per
target. `PUTGD_FUZZ_INSTRUMENT` also instruments every library the targets link, which a
fuzzing run needs and the replay does not. Neither exists in other builds. Use at most
the cores you are lent: `-jobs` and `-workers` write `fuzz-N.log` into the working
directory, so give each target its own directory.

## Corpora and regression tests

`tests/fuzz/corpus/<target>/` holds the seed and regression inputs and
`tests/fuzz/dict/<target>.dict` the dictionary. From the repository root,
`ctest --preset clang-asan -R '^fuzz_'` replays each corpus once in about five seconds (`-runs=0`), under the `clang-asan` preset's sanitizers and with a 5 s
per-input limit, so an input that crashed or hung once keeps failing the build if it
returns.

A run must stop on undefined behaviour: `PUTGD_FUZZ_INSTRUMENT` compiles with
`-fno-sanitize-recover=undefined`, and a run started through `toolbox run` needs
`env UBSAN_OPTIONS=halt_on_error=1 ...` because the toolbox does not pass the caller's
environment on. Without it UBSan only prints a line and the fuzzer carries on.

Keep a corpus small: merge a run back with
`fuzz_<name> -merge=1 new_corpus tests/fuzz/corpus/<name> run/<name>/corpus`, which keeps
only inputs that reach code the others do not, then copy `new_corpus` over the committed
set. A crash or hang found by a run is minimized with `-minimize_crash=1`, fixed, and
added to its target's corpus in the fix's pull request.

Seed from real messages: the encoders' own output, `tests/fixtures/` and the schemas
(a target's first byte usually selects the decoder, then the payload follows).
