# U11 Wave 1 B: resident setup and snapshot endpoints

Base: bcbe63f0d1e1ad00f36ac110fb596d0245869617 on codex/ui-process-split.
Implementation route: native Codex worker, bounded return-to-caller unit. Root owns CMake, builds, canonical checks and commits. No shared cache or index mutations by this worker.

## Evidence selected before implementation

Behavior changes: resident setup job starts/inspection, snapshot and complete-document settings save over schema-1 session transport, clearing final-frame bytes.
Existing tests inspected: tests/integration/test_cli.cpp, test_local_ipc.cpp, test_keychain.cpp; fixtures/U2Fixtures.h. Existing wire methods and session-cache behavior are retained.
New coverage: tests/integration/test_ui_endpoints.cpp, exercising the real SettingsService and CredentialStore session cache. Synthetic credentials and a controlled destination-test callback only; no provider or secret-store requests.

Root configured and built the dev test target, then ran the ui_endpoints_integration CTest selection; configure-red.log, build-red.log, red.log and red.xml are the authoritative output records. Root owns exact command reporting. Read red.log after execution, before editing LocalServer or implementing SetupService. Three intended behavioral failures: ui.snapshot returned unknown_method; settings.save returned invalid_request; setup endpoint returned invalid_request rather than setup_unavailable. Build passed; these were runtime assertions, not missing API compilation.

A compiling SetupService stub and six additional service tests were then supplied for a second root-owned behavioral red before implementing jobs. Read service-red.log after its successful build: nine intended runtime failures (the original three plus all six service scenarios returned setup_unavailable or lacked the expected accepted job). No production endpoint/job implementation preceded either applicable red run.

## Scope retained

No main/view/CMake edits. Destination-test checkpoint journaling remains in root-owned main callback. No parallel journal, retry, persisted setup jobs, ui.state/wait/lease endpoints, credential draft persistence, package/cloud/dependency or graphical lab work. Headless tests are not native desktop acceptance evidence.

## Implemented behavior

SetupService resolves saved profile/credential references after requiring matching accepted and observed revisions with successful validation. Starts return only setupId, retain no credential input, reject concurrent profile+kind work with setup_busy, and keep at most 32 active and 64 completed records. CredentialStore owns resident cache data. Callback delivery uses an owner-thread queue with a mutex-protected lifetime context; repeated/late callbacks do not overwrite finished jobs or dereference a destroyed service.

LocalServer exposes ui.snapshot (compiled manifest, stable resident epoch, readiness, accepted settings/metadata, augmented diagnostics), settings.save (existing asynchronous persistence), setup starts and setup.inspect. Final-frame request bytes are cleared before dispatch and for malformed/oversized frames. Existing settings.patch/reload and CLI schemas remain intact.

Extra integration scenarios cover the real socket across accepted setup and disconnected/reopened clients, strict parameter shapes, saved profile selection, stable/distinct epochs, diagnostics injection, and a destroyed setup service.

Considered and not built: duplicate setup ID reuse (rejecting busy is contract-compliant and avoids accepting a new secret or revised target as an old job); setup persistence/retry (explicitly excluded and destination journaling already belongs to the tester callback); native keychain dialogs (this unit is specified to use session-storage tests; graphical acceptance belongs to later integration).

Worker read-only verification: git diff --check passed after implementation. Root built the implemented target and existing regression targets successfully (build-green.log). Read green.log: ui_endpoints_integration passed in 0.40 s; keychain_integration passed in 0.13 s; local_ipc_contract passed in 13.24 s; 3/3 selected CTest suites passed, total 13.84 s. New suite contains 12 behavior slots. These root-owned outputs are the authoritative green evidence. CLI regression and sanitizer/whole-wave checks remain root-owned; no worker claims those from the focused run. Final-frame buffer clearing was inspected in the LocalServer diff; the passed local IPC suite covers fragmented and duplicate frames. Existing malformed-JSON/oversize cases are in cli_integration, which remains a root-owned regression check. No test-only buffer inspection API was added.
