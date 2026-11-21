# #1805 research harness (not for merge)

bench.py replays the corpus one process per capture (wall, CPU, peak RSS, CER/WER vs hand transcripts and vs a reference run). run.sh/matrix.sh/tmatrix.sh drive bench_ocr_method on the builders; *.txt are the setting lists; compare.py prints a table. Bench-only knobs live in tests/integration/ocr/ocr_method.cpp and small research globals in src/ocr/.
