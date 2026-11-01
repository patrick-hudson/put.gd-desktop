#!/bin/sh
# Upload with put.gd (#1105): a Nautilus, GNOME desktop icons or Caja script.
# put.gd writes this file and rewrites it at every start; edits are replaced.
# X-putgd-Managed=true
set -u
launcher=__PUTGD_LAUNCHER__
self=__PUTGD_SELF__
kind=__PUTGD_KIND__
icon=__PUTGD_ICON__

# Removing put.gd leaves this per-user script behind. Then it removes itself
# and does nothing, instead of running a missing launcher.
if [ ! -x "$launcher" ]; then
    if [ -f "$self" ] && [ ! -L "$self" ] && [ -O "$self" ] && grep -Fxq -- '# X-putgd-Managed=true' "$self"; then
        rm -f -- "$self"
    fi
    exit 0
fi

# No terminal is attached; best effort, and never a reason to fail.
notify() {
    if command -v notify-send >/dev/null 2>&1 &&
        notify-send --app-name=put.gd --icon="$icon" -- put.gd "$1" >/dev/null 2>&1; then
        return 0
    fi
    command -v gdbus >/dev/null 2>&1 || return 0
    quoted_icon=$(printf '%s' "$icon" | sed "s/[\\\\']/\\\\&/g")
    gdbus call --session --dest org.freedesktop.Notifications --object-path /org/freedesktop/Notifications \
        --method org.freedesktop.Notifications.Notify "'put.gd'" 0 "'$quoted_icon'" "'put.gd'" "'$1'" \
        '@as []' "{'urgency': <byte 1>}" 10000 >/dev/null 2>&1 || :
}

if [ "$#" -eq 0 ] && [ "$kind" = caja ] && [ -n "${CAJA_SCRIPT_SELECTED_URIS-}" ]; then
    # Caja passes no arguments once any selected item is not local, but still
    # lists every item here, one percent-encoded URI per line; the launcher
    # resolves them.
    old_ifs=$IFS
    IFS='
'
    set -f
    set -- $CAJA_SCRIPT_SELECTED_URIS
    set +f
    IFS=$old_ifs
    [ "$#" -eq 0 ] || exec "$launcher" --upload -- "$@"
fi
# Nautilus's Recent and Starred views pass item names that are not files, with
# no file paths, but list each item's URI; the launcher resolves its target.
if [ "$kind" = nautilus ] && [ -z "${NAUTILUS_SCRIPT_SELECTED_FILE_PATHS-}" ] && [ -n "${NAUTILUS_SCRIPT_SELECTED_URIS-}" ]; then
    old_ifs=$IFS
    IFS='
'
    set -f
    set -- $NAUTILUS_SCRIPT_SELECTED_URIS
    set +f
    IFS=$old_ifs
    [ "$#" -eq 0 ] || exec "$launcher" --upload -- "$@"
fi
# Nautilus passes no arguments when any selected item has no local path.
if [ "$#" -eq 0 ]; then
    notify 'These items have no local path'
    exit 0
fi
# Names may be relative to the folder the file manager runs this script in.
# Each argument stays one argument; the newline-separated
# *_SCRIPT_SELECTED_FILE_PATHS variables are never split.
directory=$(pwd -P && printf x) || exit 1
directory=${directory%?x}
[ "$directory" != / ] || directory=
for argument do
    shift
    case $argument in
        /*) set -- "$@" "$argument" ;;
        *) set -- "$@" "$directory/$argument" ;;
    esac
done
exec "$launcher" --upload -- "$@"
