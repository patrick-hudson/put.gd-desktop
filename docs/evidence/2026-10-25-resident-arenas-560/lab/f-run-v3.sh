#!/bin/bash
# Revision of f-run-v2.sh (#579 Greptile 4372270472). f-run-v2.sh created the work directory that f-run.sh then
# creates itself under set -e, so the approved path always failed. Here f-run.sh is the only owner of the work
# directory; the scope receipt is a sibling file written before dispatch. Launches nothing without a dedicated
# clipboard scope. functional_ci.py uploads with automatic link copy (no setting disables it), so on a shared
# desktop it writes that session's clipboard; this runner never reads, saves or restores clipboard contents.
# usage: I560_CLIPBOARD_SCOPE=dedicated:<approval-reference> bash f-run-v3.sh LABEL
# Test overrides: I560_WORK_ROOT (default /home/patrick/work/i560o), I560_F_RUNNER (default $I560_WORK_ROOT/f-run.sh).
set -euo pipefail
[[ $# -eq 1 && $1 =~ ^[a-z0-9]{2,4}$ ]] || { echo "usage: I560_CLIPBOARD_SCOPE=dedicated:<approval> bash f-run-v3.sh LABEL" >&2; exit 2; }
scope=${I560_CLIPBOARD_SCOPE:-}
[[ $scope =~ ^dedicated:[A-Za-z0-9._-]+$ ]] || { echo "refusing: functional uploads copy links to the desktop clipboard; set I560_CLIPBOARD_SCOPE=dedicated:<approval> for a desktop whose clipboard is dedicated to this test" >&2; exit 3; }
root=$(realpath -e -- "${I560_WORK_ROOT:-/home/patrick/work/i560o}")
runner=$(realpath -e -- "${I560_F_RUNNER:-$root/f-run.sh}")
out=/tmp/p560$1; work=$root/$1; receipt=$root/$1.clipboard-scope.txt
[[ ! -e $out && ! -e $work && ! -e $receipt ]] || { echo "output exists" >&2; exit 2; }
umask 077; printf '%s\n' "$scope" > "$receipt"
exec bash "$runner" "$1"
