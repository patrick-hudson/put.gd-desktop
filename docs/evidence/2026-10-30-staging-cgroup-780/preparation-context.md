# Preparation attempts

- Both Boat sandboxes were created from the specifically requested snapshots,
  with a two-hour TTL and automatic snapshots disabled. No third sandbox was
  created. Ubuntu's 89 GB snapshot took approximately twelve minutes to report
  `hydrated: true`; no guest was started while hydration was incomplete.
- The first host-only capacity command omitted the CLI's required `--run` and
  was refused before any VM. The correction calls the same module's
  `classify_filesystem`, `measure_filesystem` and `host_capacity_passes` for the
  pre-allocation host check (12 GiB reserve), then runs the full host/guest CLI
  after launch. No capacity limit was lowered.
- Read-only distro probes initially used `systemd --version`, which is absent
  from the SSH PATH on the four builder hosts. The initial receipts are kept;
  the correction uses `systemctl --version`. No physical desktop was operated.
- The first Ubuntu preparation controller used a 300-second outer subprocess
  timeout. It expired while the unchanged runner hashed its golden, before a
  run directory or QEMU process existed. The controller terminated its exact
  subprocess. The original output files are retained privately in
  `work/e780-launch-timeout`; absence of the run and QEMU was checked before
  retrying. The corrected command is `nice -n 10 timeout 960 python3
  /home/user/prepare780.py ubuntu`, with a 900-second subprocess bound. The
  runner's `launch --timeout 240` guest boot budget is unchanged. This reuses
  the same sandbox and the still-unused `c1` path.
- The first successful Cinnamon regression observed `populated 1` followed by
  systemd scope removal before the millisecond observer sampled zero. Recovery
  correctly used the authenticated gone-scope path. The entire first evidence
  directory is retained. A second run in the same guest uses a tight external
  observer with a ten-second hard deadline and records `populated 1 → 0`.
  The production recovery code and its safety conditions are unchanged.
