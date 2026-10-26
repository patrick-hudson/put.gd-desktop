# History response limits: approval receipt

Patrick explicitly replied **“Approve these limits”** to this proposal in the
integrated acceptance thread on 2026-10-25:

> For the 10,000-entry History response gates, may I use these proposed limits: open ≤1,000 ms, scroll p95 ≤100 ms, search p95 ≤250 ms, preview p95 ≤1,000 ms? The prior completed run measured 627/50.4/197/872 ms; current-build rows will remain unassessed until you approve.

The unchanged [canonical decision receipt](history-thresholds-approved.json) is
copied from the shared first-stable coordination directory. The quote is also
retained there in `COORDINATION.md`; it is a record of that user message, not a
new approval. The earlier measurements motivated the proposal and are not
current-build evidence. Open, scroll and search still lack witnessed content
endpoints in this campaign; this approval does not turn repaint timing into a pass.
