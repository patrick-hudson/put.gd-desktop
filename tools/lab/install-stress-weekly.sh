#!/bin/sh
# Install or update the weekly builder stress lane (#360) for the current user
# on a builder (k72). Run it on the builder from any put.gd checkout:
#
#   sh tools/lab/install-stress-weekly.sh
#
# It copies tools/ci/stress_weekly.py and tools/lab/desktop_lock.py to
# ~/.local/libexec/putgd-stress/, installs and enables putgd-stress-weekly.timer
# (Sundays 03:00), enables lingering so the timer also fires while nobody is
# logged in, and creates ~/.config/putgd-stress (mode 700) for the GitHub
# token. It never creates, reads or prints the token. Re-run it after the
# wrapper or the units change.
set -eu
source=$(cd "$(dirname "$0")/../.." && pwd)
libexec="$HOME/.local/libexec/putgd-stress"
units="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
config="${XDG_CONFIG_HOME:-$HOME/.config}/putgd-stress"

install -d -m 700 "$libexec" "$config"
install -d "$units"
install -m 755 "$source/tools/ci/stress_weekly.py" "$libexec/stress_weekly.py"
install -m 644 "$source/tools/lab/desktop_lock.py" "$libexec/desktop_lock.py"
install -m 644 "$source/tools/lab/putgd-stress-weekly.service" "$units/putgd-stress-weekly.service"
install -m 644 "$source/tools/lab/putgd-stress-weekly.timer" "$units/putgd-stress-weekly.timer"
echo "installed from $source ($(git -C "$source" rev-parse --short HEAD 2>/dev/null || echo 'not a git checkout'))"

if [ "$(loginctl show-user "$(id -un)" -p Linger --value 2>/dev/null)" != yes ]; then
    if loginctl enable-linger "$(id -un)" 2>/dev/null || sudo -n loginctl enable-linger "$(id -un)" 2>/dev/null; then
        echo "lingering enabled: the timer fires while nobody is logged in"
    else
        echo "WARNING: could not enable lingering; run 'sudo loginctl enable-linger $(id -un)' or the timer" \
             "fires only while $(id -un) is logged in" >&2
    fi
fi

systemctl --user daemon-reload
systemctl --user enable --now putgd-stress-weekly.timer
systemctl --user list-timers putgd-stress-weekly.timer --no-pager

token="$config/github-token"
if [ -f "$token" ]; then
    echo "GitHub token: present, mode $(stat -c %a "$token") (needs 600)"
else
    echo "GitHub token: missing; runs keep their local results and skip the flaky-test comments until" \
         "Patrick installs a fine-grained token (patrick-hudson/put.gd-desktop only, Issues: read and write)" \
         "at $token with mode 600"
fi
