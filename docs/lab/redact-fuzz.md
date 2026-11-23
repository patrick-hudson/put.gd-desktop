# Sensitive-text fuzz harness

`tools/lab/redact_fuzz/` renders seeded, realistic screens with ground-truth labels and runs them through the
real recognition helper and the real Quick redact matcher. It records each false positive and false negative it
finds on those scenes, with the recognized text behind it (#1931). Use it to check a matcher or recognition change against
screens that ordinary content makes awkward. It is a lab tool: it shows how the rules behave on rendered scenes,
and it is not acceptance evidence for any desktop.

## What it runs

The driver runs the matcher the way the editor's Quick redact does. A scene goes to the package's own
`putgd-ocr` through `TextJob`, with words and candidates on and English only. The candidates go through
`TextLayer::exclude`. Then each profile runs one `quickRedact::find()` with the user's login, real name and
two list terms, and each match becomes `markBoxes()` over the image. Two profiles run on every scene:

| Profile | Categories | Stands for |
| --- | --- | --- |
| `all` | every category | Quick redact's All of these |
| `check` | `card`, `key`, `list` | the upload check's default kinds, all on `warn`: one pass |

The same profiles also run on the scene's true words ("oracle"): the words a perfect recognizer would return,
with their boxes from the renderer. Comparing the two runs attributes each finding to the rules or to recognition
(see [reading the findings](#reading-the-findings)); the attribution is a heuristic, not a proof.

## Parts

| File | Job |
| --- | --- |
| `driver.cpp` | The batch driver: one JSON job per line on stdin, one result per line on stdout. It is built from the source tree of the package's commit. It answers the helper's build check with that package's `build-info.json`, so it drives the packaged helper unchanged. |
| `canvas.py` | A Pango and Cairo canvas that records, while it draws, each labelled span's boxes per line and per character, and every whitespace-separated word. |
| `content.py` | Sensitive values and look-alikes. Every value is random and built at run time, and secret prefixes are joined from pieces, so no secret-shaped literal sits in the source. |
| `families.py` | The scene families, from calendars and spreadsheets to terminals, IDEs, chat, mail, forms and non-English text. Each family labels what is sensitive (`secret`), what only looks it (`decoy`) and what may be covered either way (`neutral`). |
| `fuzz.py` | `build`, `render`, `run`, `score`, `show` (findings by field) and `probe` (text lines straight through the matcher). |

Labels: `secret` spans carry the category that should find them, or `gap` for secrets no category targets
(seed phrases, recovery and TOTP codes, licence keys). The rules cover any IP address and any form of the user's
own name by design, so the scorer doesn't count those covers as false positives. It skips an `ip` cover that
holds an address and lies over no labelled look-alike, and records a `name` cover that holds a part of the user's
name as `DESIGN`. An `ip` cover over a labelled look-alike, such as a version number, stays a false positive.

## Running it

Run it on a builder, never on the workstation, under the
[sharing rules](remote-builders.md#sharing-the-lab-between-concurrent-sessions). A run keeps every core it
is given busy for minutes, the way a timing run doesn't want its host to be, so hold your own quiet lease
(`desktop_lock.py acquire HOST --resource quiet`) on that host for the whole run, with a heartbeat every 3 to
4 minutes, and release it when the run ends. Use at most 4 jobs per host. Start a short command, such as a probe,
with `desktop_lock.py guard HOST &&`. `run` checks `guard` itself before each batch and waits while another session
holds the lease.

Below, `fuzz.py` means `python3 tools/lab/redact_fuzz/fuzz.py`, run from the directory you copied the tools into,
and `PKG` is the directory the package was unpacked into.

1. Fetch the CI packages of the nightly-branch head (`release-native-*` from its CI run) and unpack the one for
   the host's distribution without installing it (`dpkg-deb -x`, or `rpm2cpio | cpio -idm`) into `PKG`. Its
   `PKG/usr/share/putgd/build-info.json` is the build object the helper checks (the CI artifact carries the same
   file beside the package).
2. Put a `git archive` of that commit's `src/` next to it, and copy `tools/lab/redact_fuzz` and
   `tools/lab/desktop_lock.py` to the host.
3. Build the driver, which takes about 40 s at 4 compile jobs: `nice -n 10 fuzz.py build --source SRC_TREE --out build`.
   On aurora, run it inside the `putgd-build` toolbox. On Mint, Debian and Kubuntu, run it with the distribution's Qt
   development packages.
4. Render: `fuzz.py render --seed S --count N --out scenes-S`. Pango, Cairo and PyGObject come from the host,
   and Pillow is used for JPEG. The same seed, start and count on the same host, with the same fonts, give the
   same images. `fonts.json` lists the font families the host had.
5. Run: `fuzz.py run --scenes scenes-S --out results-X --driver build/redact_fuzz_driver --helper PKG/usr/bin/putgd-ocr
   --build-json PKG/usr/share/putgd/build-info.json --data TESSDATA --jobs 4 --session YOUR_SESSION`. On aurora, run
   it inside the toolbox with `--data /usr/share/tesseract/tessdata`. On Debian and Ubuntu the data is in
   `/usr/share/tesseract-ocr/5/tessdata`. `--families a,b` runs only the scenes that hold those families, and
   `--oracle-only` matches the true words alone, with no recognition: a few seconds a thousand scenes, the quick
   loop for a rule change. A run started again skips every scene already in its `--out` directory, failed ones
   included, so give each driver, helper or mode its own `--out`; scenes rendered by someone else can be reused
   read-only that way.
6. Score: `fuzz.py score --scenes scenes-S --results results-X --out score-X` (add `--oracle` after an
   `--oracle-only` run). This writes `findings.jsonl` (one row per false negative, partial cover, false positive,
   over-cover and by-design cover) and `summary.json` (groups by profile, kind, decoy or tag, and cause, with
   examples). The first line it prints counts the scenes scored; `summary.json` also counts the scenes whose run
   failed (`errors`). Check both before comparing two runs.
7. Look at one group with `fuzz.py show score-S/findings.jsonl type=FN kind=card --n 10`. A filter is `FIELD=VALUE`; a
   value ending in `*` matches a prefix. To see what a rule does with a string, give it perfectly recognized words:
   `fuzz.py probe --driver build/redact_fuzz_driver --build-json PKG/usr/share/putgd/build-info.json "Reference 12   4111 1111 1111 1111"`.
   That prints every match of both profiles. Each argument is one line of one scene (with none, lines are read from
   stdin), and a line starting with `-` needs `--` before it. Probe has no recognition, so it needs no helper.

At 4 jobs, aurora runs about 200 scenes a minute and a 4-core builder about 100.

## Reading the findings

| Type | Means |
| --- | --- |
| `FN` | every character of a secret left visible |
| `PARTIAL` | some characters of a secret left visible (`visible` of `chars`) |
| `FP` | a mark over nothing sensitive (`decoy` names the look-alike under it, or `ordinary`) |
| `OVER` | a row of a true match that covers nothing sensitive (a match running on past its secret) |
| `DESIGN` | a cover the rules intend over a part of the user's name (by-design IP covers are skipped, not listed) |

`cause` is an attribution, not a proof. A missed secret is `rule` when the oracle run also leaves at least one of
its characters visible, and `ocr` when the oracle covers it all. An extra cover is `rule` when the oracle run has a
mark of the same category overlapping it, and `ocr` otherwise. `read` holds what recognition made of a missed
secret. `match` holds the matched text of an extra cover.

## Traps

- Rendered scenes and results hold secret-shaped strings: never commit them, and never copy them into an
  evidence record unscanned.
- A scene is only as realistic as its family. A finding becomes an issue once it is reproduced on a real
  application's screen, or once the rendered screen is plainly one an ordinary user would capture.
- Fonts differ between builders, so the same seed renders differently on each host. Compare engines on
  copied images, not on regenerated ones.
- Text past the image's edge, under an editor's minimap or under the next tiled pane is hidden. The canvas drops it from
  the labels, and the scorer skips a secret cut by the edge. Before that, such text counted as a visible secret that
  recognition "missed". When a new family hides text some other way, drop it the same way.
- The `lowcontrast` theme (grey on light grey, about 2.6:1) is often not read at all. Read its misses as a recognition
  limit, apart from the rule groups.
