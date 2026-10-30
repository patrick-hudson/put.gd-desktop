#!/bin/sh
# Stage only in a disposable guest/sandbox, never in an immutable golden.
set -eu
if [ "${1:-}" != --disposable ]; then
  echo 'usage: provision-probes.sh --disposable (never run on goldens)' >&2
  exit 2
fi
sudo -n apt-get install -y --no-install-recommends tesseract-ocr python3-pil python3-pyatspi xdotool
