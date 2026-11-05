# History preview blanking on reloads (nightly flake, #285)

`history-preview-reload-blank.png` explains the fix for the nightly CI failure in
`HistoryGui::missingPreviewIsMadeOnSelection` (linux 26.04, run 39496330154).

- Cause: every reload of the selected item cleared the inspector, showed
  "Preview unavailable" and read the preview again. The test's 150 ms reload
  timer could land between "the preview is shown" and the pixel check, which
  then read an empty pixmap (`QImage::pixelColor: coordinate (0,0) out of range`).
- Measurement: on k72 (Kubuntu 26.04, Qt 6.10.2), headless and isolated, under
  three nice-15 busy loops (load 2.7–4.1), the test with a new 800 ms no-blank
  check ran 50 times against each build:
  - nightly 0912f0c (unfixed window): blank in 50/50 runs, 5–10 blank 5 ms samples each;
  - the fix: 0/50.
- The original single pixel check passed 50/50 on the unfixed window: it fails
  only when a reload lands between two test lines, as it did once in CI.
