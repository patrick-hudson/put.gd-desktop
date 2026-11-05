# Evidence moved

Evidence records live in
[put.gd-evidence](https://github.com/patrick-hudson/put.gd-evidence), with an
[index of records](https://github.com/patrick-hudson/put.gd-evidence/blob/main/records/README.md).
Nothing new may be added here.

`docs/evidence/<name>` in this repository's earlier commits is `records/<name>` there.
Commit [`874703b`](https://github.com/patrick-hudson/put.gd-evidence/tree/874703b35562c732268c6fe0bc9b42614ffda928/records)
is a byte-for-byte import of this directory at put.gd-desktop commit
`6677938767e732247f9097e0ae5f063cb63140ef`. Documents link to records at that commit.

To add a record, publish its directory:

```sh
python3 tools/maintenance/evidence.py publish <dir>
```

The command validates the record, commits it, pushes it and prints commit-pinned
links. See [evidence retention](../testing/evidence-retention.md).

Two directories stay here because the repository's own checks read them:

- [live-s3-memory/source114](live-s3-memory/source114/): CI replays it (`python3 tools/ci/contracts.py evidence`).
- [cold-family-metadata-race](cold-family-metadata-race/): `tests/performance/test_observe_cold_family.py` loads `metadata_churn.py` from it.
