# Issue 37: settings drafts

Fix branch: `codex/fix-settings-drafts`. Implementation commit:
`a24b79a226cdaf19bff7358ad718662795da2da3`. Final tested candidate:
`337773f4d5cef719b3f54d59bdb336ba3121f453` (adds the explicit Qt elapsed timer include).

Destination and provider switches retain independent raw field drafts. Credential
inputs stay in helper memory and are excluded from serialized drafts. Regression
coverage includes Save, Store, helper recreation, provisional destinations,
external revision conflicts, and checkpoint validation.

## Completed checks

- Qt 6.4.2 settings suite: 102 QtTest results passed, including 24 new regression functions.
- Implementation commit a24b79a: two full development suite runs passed 59/59.
- Final candidate host development run: 58/59; operations_contract reached its
  unchanged 60-second watchdog. A disk-backed isolated retry also timed out.
  A private tmpfs retry passed in 48.91 seconds. The same target passed on normal
  VM storage in KDE (49.12 seconds) and GNOME (49.44 seconds). Host disk contention
  is the working diagnosis, not a proven product defect or an unconditional host pass.
- Final candidate Cinnamon KVM: visible settings switch/edit/Save/reopen witness
  passed five steps; 24 issue regression functions passed; all 12 native capture,
  clipboard and cancellation scenarios passed; six view/helper lifecycle cases passed.
- Cinnamon memory: two serial 300-second post-use guest observations passed;
  peak RSS 29.421875 MiB, peak PSS 19.3447265625 MiB, no guest swap or remaining
  helpers. These synthetic VM observations do not establish physical acceptance.
- Final candidate KDE: Qt 6.10.2, development suite 59/59 passed; native release
  package built and installed with matching source/package receipts.
- Final candidate GNOME: Qt 6.10.2, development suite 60/60 passed (includes the
  additional native GJS contract); release package built and installed.
- Local code review and adversarial review completed; the identified test coverage
  gaps were addressed. External review transport failed before material was sent.

## Incomplete checks when testing was stopped

The user stopped further testing and requested disk cleanup. There is no complete
current-candidate three-desktop graphical acceptance claim.

KDE's first six native capture/Qt clipboard scenarios passed. The Escape cancellation
scenario timed out while acquisition was pending; the operation later settled.
Selector readiness timing was suspected but was not verified. The first standalone
Wayland widget attempt also aborted because the isolated runtime directory did not
resolve the relative compositor socket. The harness was corrected but not rerun.
Remaining KDE graphical/memory checks and GNOME graphical/memory checks were not
completed. No further test runs were started after the stop request.

## Cleanup

Task-created VM overlays, independent issue-37 image copies (after their referencing
overlays were removed), build outputs, source archives, packages, screenshots and
scratch directories were removed at the user's request, reclaiming 60.05 GiB.
Existing original bases,
other tasks' artifacts, SDK/runtime dependencies, source commits and unrelated user
changes were preserved. Compact machine-readable receipts are in the adjacent JSON;
the cleanup allocation manifest is `work/issue37-cleanup.json` in the primary checkout.
