# Real-website sweep

`tools/lab/redact_sites/` measures how often everyday public web pages make the sensitive-text check
prompt with nothing secret on them (#1975). It captures each page of a fixed list in real browsers at
several scales, runs every capture through the packaged `putgd-ocr` and the matcher, and counts the
prompts that a triage file calls false, by root cause. Rerun it after any matcher change. It is a lab
tool, not acceptance evidence for any desktop.

## Parts

| File | Job |
| --- | --- |
| `sites.tsv` | The page list, by category: news, finance, video, social, shopping, developer docs and GitHub, registries, Wikipedia, weather, sports, status pages, forums, search, maps, travel, mail and chat landing pages, cloud pricing, government, education and non-English sites. Every page loads without a login. |
| `sweep.py` | `capture`, `run`, `replay`, `report` and `compare`, described below. |
| `triage.tsv` | One rule per line: kind, a regular expression on the matched text (or `url=RX text=RX` for one site), verdict `true` or `false`, and the root-cause group. The first rule that matches decides; a match no rule decides counts as `untriaged`, which is false. |

The captures and results hold page text, so they stay out of git and off the workstation. Run on a
builder, holding your own quiet lease (see [remote builders](remote-builders.md)).

## Steps

1. **Capture.** `capture` needs Python with `playwright` and `pillow` and the browsers from
   `python -m playwright install chromium firefox`. On aurora, run it inside the `putgd-build` toolbox
   with a venv in your own work folder.

   ```sh
   venv/bin/python tools/lab/redact_sites/sweep.py capture --out cap --jobs 4 [--browsers chromium] [--part 0/2]
   ```

   Each page loads in a fresh 1440×900 context at each device scale (Chrome 100, 125 and 150 %,
   Firefox 100 and 125 %). The tool waits for the load event plus three seconds, then saves the
   viewport. Chrome runs as the full browser in its new headless mode, not Playwright's headless
   shell. A dark capture (`prefers-color-scheme: dark`) is kept only when it differs from the light
   one. The captures hold page content only: no tab strip, no address bar. `--part K/N` splits the
   list over hosts. A rerun into the same folder keeps the captures it already has.
2. **Run.** `run` feeds each capture to the [fuzz harness](redact-fuzz.md)'s driver with the
   package's helper, once with the upload check's default kinds (`card`, `key`, `list`) and once with
   every kind:

   ```sh
   python3 tools/lab/redact_sites/sweep.py run --shots cap --driver DRIVER --helper .../putgd-ocr \
     --build-json build-info.json --data .../tessdata --out res.jsonl --jobs 4 --session YOUR_LEASE
   ```

   It stops launching work while `desktop_lock.py guard` refuses. On aurora, run it inside the toolbox.
3. **Replay.** For a rule change, `replay --words res.jsonl --driver NEW_DRIVER --out rp.jsonl`
   matches the recognized words again with a driver built from your tree, with no recognition: a few
   seconds instead of minutes. It passes each word's confidence and second-read flag, so a replay
   through the driver of the recognition run's own commit gives the run's matches exactly.
4. **Report.** Pass one `--shots` and `--results` pair per host:

   ```sh
   python3 tools/lab/redact_sites/sweep.py report --shots cap --results res.jsonl [--shots ... --results ...] --out rep
   ```

   `report` writes `matches.tsv` (every match with its page, browser, scale, theme, verdict, group
   and words around it), `summary.json` and `table.md`. The table gives, per profile and scale, the
   captures with any prompt and those with a false one, the pages with a false prompt in any capture,
   and false matches per group. `compare rep-before rep-after` prints both side by side.

## Results (#1975)

Nightly 8c6d168ca against the fix in #1976, on the same recognized words:

| Profile | Pages with a false prompt, before | After |
| --- | --- | --- |
| Default kinds | 21 of 265 | 15 |
| Every kind | 66 of 265 | 53 |

The groups left are listed with the limits in
[text recognition](../design/text-recognition.md#the-editor). Some public pages show the visitor's own
public address, which the IP kind covers by design; don't paste a report's `matches.tsv` anywhere public.
