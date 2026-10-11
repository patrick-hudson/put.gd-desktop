# Startup reclamation strategy digest

One memory-lifecycle experiment kept on optimize/resident-startup-reclaim as 68e5549 and integrated into the product as fc69d387003e25dd2f7a4a38bafd15fd413ecaf4. Exact two-file hashes match the measured and reviewed source. The unchanged five-minute physical Cinnamon cold workload fell from 48,431,104 bytes (46.1875 MiB) to 47,149,056 bytes (44.96484375 MiB): 1,282,048 bytes / 1.22265625 MiB / 2.647% lower. The predeclared >1 MiB threshold passes; CPU0.0133236% of one core and the original100 MiB/0.5% budgets pass. The40 MiB target is not met.

Forecast was unknown product savings, plausibly zero to a few MiB, informed by a1.77 MiB standalone native Qt probe. Product result supports the direction but is smaller; the probe was not product proof. Stable mode uses one five-minute sample per source; no statistical confidence interval or long-session plateau is claimed. Native GTK remains, no recurring timer/cache purge/font/dialog change.

Source review found no actionable issue. Test-first red shows three missing-request failures, failed-init passes; candidate Debug/Release whole operation contracts pass. Integrated Dev28/28, ASan/UBSan operations1/1 and instrumented Qt TSan operations1/1 pass. Three native empty-view cycles pass; final hidden RSS73.8242 MiB remains a separate post-use concern. Post-use/S3/populated history and live capture latency stay separate.

No runnable second implementation hypothesis exists in the approved two-file, once-only startup scope. Further savings require another attributed scope; do not add repeated trimming or global allocator changes. Preserve worktree and binaries referenced by evidence. No paid infrastructure/new dependencies; model cost unavailable and not separately dollar-capped.
