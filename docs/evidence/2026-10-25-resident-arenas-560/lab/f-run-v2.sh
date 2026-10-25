#!/bin/bash
# Guarded replay of f-run.sh (#579 Cubic 4372241074). functional_ci.py uploads with the default automatic link copy,
# and no profile setting disables it, so on a shared physical desktop it writes that session's clipboard (the f1
# deviation). This runner refuses to launch anything unless the caller declares a dedicated clipboard scope that the
# lab coordinator approved for this desktop session. It never reads, saves or restores clipboard contents.
# usage: I560_CLIPBOARD_SCOPE=dedicated:<approval-reference> f-run-v2.sh LABEL
set -euo pipefail
[[ $# -eq 1 && $1 =~ ^[a-z0-9]{2,4}$ ]] || { echo "usage: I560_CLIPBOARD_SCOPE=dedicated:<approval> f-run-v2.sh LABEL" >&2; exit 2; }
scope=${I560_CLIPBOARD_SCOPE:-}
[[ $scope =~ ^dedicated:[A-Za-z0-9._-]+$ ]] || { echo "refusing: functional uploads copy links to the desktop clipboard; set I560_CLIPBOARD_SCOPE=dedicated:<approval> for a desktop whose clipboard is dedicated to this test" >&2; exit 3; }
out=/tmp/p560$1; work=/home/patrick/work/i560o/$1
[[ ! -e $out && ! -e $work ]] || { echo "output exists" >&2; exit 2; }
umask 077; mkdir "$work"; printf '%s\n' "$scope" > "$work/clipboard-scope.txt"
exec /home/patrick/work/i560o/f-run.sh "$1"
