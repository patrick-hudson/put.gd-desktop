# Job summaries of run 40008409878

Each install step printed its `$GITHUB_STEP_SUMMARY` into its log, because GitHub shows summaries only to signed-in viewers. The tables below are copied from the job logs with the timestamps removed.

## stalled attempt, then the retry passes (job 120602161071)

### Package install retries (stall demo, retry, apt-get)

| Attempt | Outcome | Detail |
| --- | --- | --- |
| 1 of 4 | **stopped**, retrying in 30 s with refreshed metadata | `printed nothing for 60 s after: Get:1 http://archive.ubuntu.com/ubuntu noble InRelease [1049 kB]` |
| 2 of 4 | **passed on retry** |  |

## every attempt stalls, the snapshot installs (job 120602160473)

### Package install retries (stall demo, snapshot, apt-get)

| Attempt | Outcome | Detail |
| --- | --- | --- |
| 1 of 2 | **stopped**, retrying in 30 s with refreshed metadata | `printed nothing for 60 s after: Get:1 http://archive.ubuntu.com/ubuntu noble InRelease [1049 kB]` |
| 2 of 2 | **stopped** | `printed nothing for 60 s after: Get:1 http://archive.ubuntu.com/ubuntu noble InRelease [1049 kB]` |
| snapshot | **installed offline from the package snapshot** saved 2026-10-23T19:04Z |  |

## every attempt stalls, no snapshot (expected red) (job 120602160906)

### Package install retries (stall demo, no snapshot, apt-get)

| Attempt | Outcome | Detail |
| --- | --- | --- |
| 1 of 4 | **stopped**, retrying in 30 s with refreshed metadata | `printed nothing for 600 s after: Get:1 http://archive.ubuntu.com/ubuntu noble InRelease [1049 kB]` |
| 2 of 4 | **stopped**, retrying in 60 s with refreshed metadata | `printed nothing for 600 s after: Get:1 http://archive.ubuntu.com/ubuntu noble InRelease [1049 kB]` |
| 3 of 4 | **stopped**, retrying in 120 s with refreshed metadata | `printed nothing for 600 s after: Get:1 http://archive.ubuntu.com/ubuntu noble InRelease [1049 kB]` |
| 4 of 4 | **stopped** | `printed nothing for 600 s after: Get:1 http://archive.ubuntu.com/ubuntu noble InRelease [1049 kB]` |
| snapshot | **failed**: no package snapshot to install from |  |
