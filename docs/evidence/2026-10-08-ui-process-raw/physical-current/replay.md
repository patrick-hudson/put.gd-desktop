# U11 measurement workload

Do not start while desktop acceptance or builds are active. The three existing
VMs are reused; no additional QEMU instance is required. A separate frozen source
archive and exact executable/version manifest identify the candidate.

`protocol.json` freezes the matched three-view parameters. Run the retained
baseline and split candidate serially on physical Cinnamon using the production
`tests/performance/bench_ui_process.py` CLI documented in
`docs/testing-ui-process.md`. Repeat each build twice with new output directories,
using the same `/tmp/putgd-startup-reclaim-exp001` validated empty local fixture.
Pass the three current native acceptance receipts to the candidate. Do not claim
four-view results are matched against a three-view baseline.

For the independent cold target run, use:

```sh
python3 work/u11/memory/cold_family.py --output /absolute/new/private/output \
  --seconds 300 --acceptance-receipt /absolute/native/receipt.json
```

Repeat `--acceptance-receipt` for all three native receipts. This workload imports
the tested family sampler and fixture validator, launches one private resident,
and preserves native Cinnamon fonts/dialogs. It checks family identities, absence
of helpers/owned mapped windows, source/build agreement, tray availability and
zero operations. It samples total RSS/PSS/private memory and actual CPU ticks.
It never captures, reads the clipboard, uploads, or opens a view. Its only cleanup
is the process group it launched. The raw sampler retains a sampled-visibility
limit: a short-lived process/window wholly between polls can be missed.

A five-second warmup precedes a >=300-second measured interval. The older idle
harness remains unchanged and its results remain historical evidence. This newer
observer adds family checks, so its CPU number is an absolute budget observation,
not a paired CPU improvement over the older observer. Empty cold and empty-view
measurements do not establish populated-history, post-image, or post-S3 plateaus.

Keep private XDG data/logs local. Export explicit result JSON, sample JSONL,
controlled commands, checksums and this replay script; never export credentials,
user clipboard/screen contents or whole work directories.

The cold workload also records a post-interval mapping breakdown, grouped by
basename using the existing `bench_idle.footprint` reader. That read occurs after
the timed CPU interval and before final CLI diagnostics; it is attribution, not a
second workload or an added saving. The workload fingerprints this dependency.
