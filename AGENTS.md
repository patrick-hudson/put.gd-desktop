# Working on put.gd

Build with C++20, Qt 6 Widgets, CMake presets, Ninja, and the official AWS C++ SDK.
The minimum Qt API is 6.4.2. Keep Qt dynamically linked to distro libraries.
Use standard system development packages; ask the user to install missing packages
when administrator authentication is required. Do not assemble a substitute toolchain.

The implementation contract is in GitHub issue #1 and
`docs/plans/2026-10-07-1926-feat-qt-linux-share-plan.md` when present locally.
Preserve its user-approved choices and explicit non-goals. Implement in dependency order.

Use `cmake --preset dev`, `cmake --build --preset dev`, and `ctest --preset dev`.
See `docs/build.md` for SDK provisioning and sanitizer builds. Keep AWS headers under
`src/storage/s3/`; shared core, GUI, history, and platform contracts remain provider-neutral.
Offscreen tests prove widget/core behavior only. Real desktop support requires graphical
acceptance evidence for each declared compositor. Never report a mock as desktop proof.

Use synthetic payloads and an isolated test destination. Never put credentials in source,
configuration, logs, history, command lines, or review packets. Never modify bucket policy,
versioning, or existing user objects during tests. Configuration contains credential references.

The user grants standing permission to send implementation and planning material to
Anthropic for review. This permission does not select an external implementation engine.
Keep review material bounded to the code/specification needed, excluding secrets and payloads.

Record what was actually built/tested and any blocked acceptance gates. Commit explicit
owned paths, preserving unrelated pre-existing changes. Do not add recording/editor/OCR
features: future video support currently means finalized video files in the shared pipeline.
