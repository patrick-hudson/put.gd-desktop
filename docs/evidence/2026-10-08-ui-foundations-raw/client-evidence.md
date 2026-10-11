# U11 Wave 1 A evidence

- Base verified before writes: bcbe63f0d1e1ad00f36ac110fb596d0245869617, branch codex/ui-process-split, clean inventory.
- Native implementation, parent-owned build/cache/CMake/index/commits.
- Evidence strategy: first add the frozen public API with a compiling unimplemented stub plus a socket-boundary test; observe behavioral red before implementing transport.
- Existing tests inspected: tests/integration/test_local_ipc.cpp, tests/integration/test_cli.cpp. Existing schema read in src/app/LocalServer.cpp and src/cli/main.cpp.
- Initial test: fragmentedReplyKeepsEventLoopResponsive checks asynchronous delivery, schema/session/request ID, fragmented responses and timer progress during delayed reply.
- No credentials.txt access, external provider calls or desktop/lab activity.

- Behavioral red witnessed before production transport: root-owned build/dev/test_session_client fragmentedReplyKeepsEventLoopResponsive, exit 1, Qt 6.4.2. `!error` fails with client_unavailable; 2 passed, 1 failed. Receipt: work/u11/client/red.log and red-exit-code. The stub compiled, so this is a behavior failure rather than an absent-API failure.

- First implementation root-owned build/run passed: `cmake --build --preset dev --target test_session_client`; `ctest --preset dev -R '^session_client_integration$' --output-on-failure`, 1/1 test target passed in 17.17 s. Receipts: build-green.log, green.log, green.xml.
- Follow-up inspection: avoid reads on an empty/unopened socket; search newline only in newly appended bytes; preserve the validated wire result even on a resident error; explicitly cover exact 1 MiB request acceptance and abandonment before connecting. Final rebuild/run pending with root.

- Targeted review red witnessed before ownership/uncertainty fix: root rebuilt tests and ran `build/dev/test_session_client invalidReply:wrong-id clientDestructionWithOwnedReceiver callbackMayDeleteClient`. `invalidReply:wrong-id` failed the outcomeUnknown assertion, and `clientDestructionWithOwnedReceiver:client-is-receiver` then terminated with SIGSEGV (signal 11). Receipt: review-red.log.
- Fixed the demonstrated lifetime error by disconnecting receiver-destruction delivery when transport stops; QPointer still suppresses queued callbacks after receiver death. Tests also cover a client-owned child receiver and deletion of the client inside its own reply.
- At parent direction, invalid responses after request submission conservatively report outcomeUnknown too: malformed/wrong-ID/oversized replies cannot establish resident acceptance. Raw request/response bytes never enter transport errors.

## Owned implementation and verification scope

- behavior_changed: true. New SessionClient is an asynchronous one-request-per-QLocalSocket UI transport with the frozen public API, schema 1/session path/random wire ID, 1 MiB request and 16 MiB response limits, a precise 15 second deadline and 64 concurrent requests.
- All delivery is asynchronous and at most once. Abandon stops only transport/delivery; receiver/client destruction prevents later delivery. No operation cancellation or replay is sent. Submitted requests with lost, timed-out or invalid replies report outcomeUnknown.
- Existing tests inspected: test_local_ipc.cpp and test_cli.cpp; neither modified. New integration test exercises actual local sockets with synthetic requests/replies, including fragmentation, wire/error validation, exact/over byte limits, asynchronous rejection, timeout, no retry, out-of-order responses, concurrency limit and lifetime/teardown paths.
- Source scan found no waitFor*, processEvents, logging calls, CLI edits or business-pipeline implementation in SessionClient. Newline scanning starts at the new bytes to avoid repeatedly scanning earlier fragments.
- No-test exceptions: none for this unit. These checks prove socket/client behavior only; graphical desktop acceptance, memory measurements, UI migration and supervisor/private-epoch acceptance remain caller-owned U11 gates.
- Considered and not built: automatic retry (forbidden by mutation uncertainty), operation cancellation on abandon (forbidden by contract), persisted drafts/credential state, private epoch/version supervision and production view migration (outside unit A).
- Worker writes: the three paths below plus work/u11/client/evidence.md only. CMake, build caches, other source files, test launchers/logs and commits are parent-owned. No worker staging/commit, credential input access, provider requests, dependency installs or lab mutations.

Final source fingerprints before final parent verification:
- `src/client/SessionClient.h`: `3f2692e680fd09644c47a6a0f6569a586b386802fd4582e551e079250b2e63d0`
- `src/client/SessionClient.cpp`: `17287ab4d2287ad3be041861b060453b2ae3f9434515ea09fcb75d6aac97e4c1`
- `tests/integration/test_session_client.cpp`: `bf968b95a5339da8228cd4522f46f847f20d919da5bf5e1b4ca7c0d96872c4ce`

## Final verification receipt

Read directly from parent-owned work/u11/transport-final/run.sh, exit-code, dev.log, asan.log and build logs after the final source edits:

- `cmake --preset dev`; `cmake --build --preset dev --target test_session_client test_ui_control`; `ctest --preset dev -R '^(session_client|ui_control)_integration$' --output-junit work/u11/transport-final/dev.xml`: 2/2 targets passed, SessionClient 17.04 s, total 17.91 s.
- `cmake --preset asan` with the existing provisioned SDK paths; `cmake --build --preset asan --target test_session_client test_ui_control`; `ctest --preset asan -R '^(session_client|ui_control)_integration$' --output-junit work/u11/transport-final/asan.xml`: 2/2 targets passed, SessionClient 18.49 s, total 19.29 s. No sanitizer failure reported.
- The combined parent verification launcher exited 0. No compiler warning/error lines found in either final build log.
- Read-only `git diff --check` passed. Final source remained stable throughout parent verification.
- Unit A complete with no implementation blocker; root owns canonical integration, review, commits and broader U11 acceptance.
