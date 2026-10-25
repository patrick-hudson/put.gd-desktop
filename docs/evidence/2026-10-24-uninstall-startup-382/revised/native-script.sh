#!/bin/sh
# put.gd start at login. put.gd writes this file while
# Launch at login is on and rewrites it at every start; edits are replaced.
# X-putgd-Managed=true
# X-putgd-StartupId=c9ffb1f5fff96ebd516bb4a511e17c6d9661cb83e03e487a384cbe8d60165270
#
# Updaters that follow the image's .zsync write the new version under the
# .zsync's versioned file name, beside the old image (AppImageLauncher), and may
# remove the old one (appimageupdatetool --remove-old). So start the newest
# put.gd AppImage in the configured image's folder that embeds the same update
# information. Gear Lever replaces the configured file in place, which needs
# nothing extra. Only the user's own files in the user's own folder qualify
# as newer images.
set -u
configured='/tmp/i382-9ncc8fe7/bin/putgd-launcher'
update_information=''
kind='native'
entry='/tmp/i382-9ncc8fe7/c/autostart/putgd.desktop'
script='/tmp/i382-9ncc8fe7/d '\'' % $ ` " \ 	 __PUTGD_KIND__/putgd/login/start.sh'
startup_id='c9ffb1f5fff96ebd516bb4a511e17c6d9661cb83e03e487a384cbe8d60165270'
watch_unit='putgd-login-startup-c9ffb1f5fff96ebd516bb4a511e17c6d9661cb83e03e487a384cbe8d60165270'
watch_description="put.gd managed login startup $startup_id"
cleanup_only=false

say() {
    printf 'put.gd login start: %s\n' "$*" >&2
}

# The user manager owns the watch, independently of the resident. No package
# hook visits home directories, and no extra process polls while the app is off.
manager() {
    env -u LD_LIBRARY_PATH -u LD_PRELOAD systemctl --user "$@"
}

unit_exists() {
    load_state=$(manager show --property=LoadState --value "$1" 2>/dev/null) || return 1
    [ -n "$load_state" ] && [ "$load_state" != not-found ]
}

stop_watch() {
    [ -n "$watch_unit" ] || return 0
    for suffix in "$@"; do
        # Never stop a foreign unit.
        unit_exists "$watch_unit.$suffix" || continue
        description=$(manager show --property=Description --value "$watch_unit.$suffix" 2>/dev/null) || continue
        [ "$description" = "$watch_description" ] || return 1
        manager stop "$watch_unit.$suffix" >/dev/null 2>&1 || return 1
    done
}

install_watch() {
    [ -n "$watch_unit" ] || return 0
    if unit_exists "$watch_unit.path"; then
        description=$(manager show --property=Description --value "$watch_unit.path" 2>/dev/null) || return 1
        [ "$description" = "$watch_description" ] || return 1
        manager is-active --quiet "$watch_unit.path" && return 0
        stop_watch path service || return 1
    fi
    watched=$configured
    [ "$kind" != appimage ] || watched=${configured%/*}
    env -u LD_LIBRARY_PATH -u LD_PRELOAD systemd-run --user --quiet --collect \
        --expand-environment=no --unit="$watch_unit" --description="$watch_description" \
        --path-property="PathChanged=$watched" --property=Type=oneshot \
        /bin/sh "$script" --cleanup-if-missing >/dev/null 2>&1
}

cleanup_missing() {
    # Bind both files to this exact installation. A replaced or foreign entry,
    # symlink, or script belongs to its owner, not to this older callback.
    if [ -O "$entry" ] && [ ! -L "$entry" ] && [ -O "$script" ] && [ ! -L "$script" ] &&
        grep -Fxq -- 'X-putgd-Managed=true' "$entry" &&
        grep -Fxq -- "X-putgd-StartupId=$startup_id" "$entry" &&
        grep -Fxq -- '# X-putgd-Managed=true' "$script" &&
        grep -Fxq -- "# X-putgd-StartupId=$startup_id" "$script"; then
        rm -- "$entry" || return 1
        rm -- "$script" || return 1
        stop_watch path || return 1
    fi
}

start_target() {
    target=$1; shift
    [ "$cleanup_only" != true ] || exit 0
    install_watch || say 'cannot arm the user startup-removal monitor'
    exec "$target" "$@"
}

case ${1-} in
    --install-watch) install_watch; exit $? ;;
    --stop-watch) stop_watch path service; exit $? ;;
    --cleanup-if-missing)
        cleanup_only=true; shift
        # File replacements during an update must settle before checking.
        sleep 2
        ;;
esac

if [ "$kind" = native ]; then
    if [ -f "$configured" ] && [ -x "$configured" ]; then
        start_target "$configured" "$@"
    fi
    if [ ! -e "$configured" ]; then
        cleanup_missing
        exit $?
    fi
    say "cannot start $configured: it is not executable"
    exit 1
fi

# The user's private group: named after the user, with no other members. With
# umask 002 (the desktop default on Debian 13, Kubuntu 26.04 and Mint 22.3) the
# user's files are group-writable.
own_group=
user=$(id -un 2>/dev/null) && group=$(id -g 2>/dev/null) && group_entry=$(getent group "$group" 2>/dev/null) &&
    case $group_entry in
        "$user":*:"$group": | "$user":*:"$group":"$user") own_group=$group ;;
    esac

# Owned by the user and writable by nobody else.
private() {
    [ -O "$1" ] || return 1
    set -- $(stat -L -c '%a %g' -- "$1" 2>/dev/null)
    [ $# -eq 2 ] || return 1
    case $1 in
        *[2367]) return 1 ;;
        *[2367]?) [ "$2" = "$own_group" ] || return 1 ;;
    esac
}

# A private executable regular file, not a symbolic link, with the AppImage
# type 2 magic at offset 8.
appimage() {
    [ -f "$1" ] && [ ! -L "$1" ] && [ -x "$1" ] && private "$1" || return 1
    [ "$(head -c 11 -- "$1" 2>/dev/null | tail -c 3 | od -An -tx1 | tr -d ' \n')" = 414902 ]
}

bounded() {
    if command -v timeout >/dev/null 2>&1; then
        timeout 20 "$@"
    else
        "$@"
    fi
}

# The version in putgd-VERSION-x86_64.AppImage, as the .zsync names the image,
# or in AppImageLauncher's putgd-VERSION-x86_64_<md5>.AppImage.
name_version() {
    name=${1##*/}
    case $name in
        putgd-?*-x86_64.AppImage | putgd-?*-x86_64_*.AppImage) ;;
        *) return 1 ;;
    esac
    version=${name#putgd-}
    version=${version%%-x86_64*}
    case $version in
        [0-9]*) ;;
        *) return 1 ;;
    esac
    case $version in
        *[!0-9A-Za-z.+-]*) return 1 ;;
    esac
    printf '%s\n' "$version"
}

# Prereleases order before their release: 26.1101.960-nightly < 26.1101.960 (sort -V, ~).
sortable() {
    case $1 in
        *-*) printf '%s~%s\n' "${1%%-*}" "${1#*-}" ;;
        *) printf '%s\n' "$1" ;;
    esac
}

# Is version $1 newer than version $2?
newer() {
    [ "$1" != "$2" ] && [ "$(printf '%s\n%s\n' "$1" "$2" | sort -V | tail -n 1)" = "$1" ]
}

best=
best_version=
directory=${configured%/*}
if [ -n "$update_information" ] && [ -d "$directory" ] && private "$directory"; then
    for candidate in "$directory"/putgd-*.AppImage; do
        [ "$candidate" != "$configured" ] || continue
        version=$(name_version "$candidate") || continue
        version=$(sortable "$version")
        if [ -n "$best" ] && ! newer "$version" "$best_version"; then
            continue
        fi
        appimage "$candidate" || continue
        [ "$(bounded "$candidate" --appimage-updateinformation 2>/dev/null)" = "$update_information" ] || continue
        best=$candidate
        best_version=$version
    done
fi
# The configured image is the one put.gd last ran from; like the autostart entry
# before #307, it only has to be an executable file.
if [ -f "$configured" ] && [ -x "$configured" ]; then
    [ -n "$best" ] || start_target "$configured" "$@"
    # Ask the image which version it is. put.gd's own update (#293) and Gear
    # Lever replace the file in place, so its name can be older than what it
    # holds, or carry no version; the name is only the fallback.
    version=$(bounded "$configured" --version 2>/dev/null | sed -n '1s/^putgd //p')
    [ -n "$version" ] || version=$(name_version "$configured") || version=
    if [ -z "$version" ] || ! newer "$best_version" "$(sortable "$version")"; then
        start_target "$configured" "$@"
    fi
    [ "$cleanup_only" = true ] || say "starting $best, which is newer than $configured"
    start_target "$best" "$@"
fi
if [ -n "$best" ]; then
    [ "$cleanup_only" = true ] || say "$configured is missing; starting $best"
    start_target "$best" "$@"
fi
# Remove only this installation's managed startup files, keeping configuration.
if [ ! -e "$configured" ]; then
    cleanup_missing
    exit $?
fi
say "cannot start $configured: it is missing or not executable, and no put.gd AppImage with its update information is beside it"
exit 1
