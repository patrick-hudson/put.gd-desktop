#!/bin/sh
# put.gd start at login for an AppImage (#307). put.gd writes this file while
# Launch at login is on and rewrites it at every start; edits are replaced.
# X-putgd-Managed=true
#
# Updaters that follow the image's .zsync write the new version under the
# .zsync's versioned file name, beside the old image (AppImageLauncher), and may
# remove the old one (appimageupdatetool --remove-old). So start the newest
# put.gd AppImage in the configured image's folder that embeds the same update
# information. Gear Lever replaces the configured file in place, which needs
# nothing extra. Only the user's own files in the user's own folder qualify.
set -u
configured=__PUTGD_CONFIGURED__
update_information=__PUTGD_UPDATE_INFORMATION__

say() {
    printf 'put.gd login start: %s\n' "$*" >&2
}

# Owned by the user and writable by nobody else.
private() {
    [ -O "$1" ] || return 1
    mode=$(stat -L -c %a -- "$1" 2>/dev/null) || return 1
    case $mode in
        *[2367]? | *[2367]) return 1 ;;
    esac
}

# A private executable file with the AppImage type 2 magic at offset 8.
appimage() {
    [ -f "$1" ] && [ -x "$1" ] && private "$1" || return 1
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

# Prereleases order before their release: 0.1.2-rc.1 < 0.1.2 (sort -V, ~).
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
if appimage "$configured"; then
    [ -n "$best" ] || exec "$configured" "$@"
    # Gear Lever's put.gd.appimage carries no version in its name; ask the image.
    version=$(name_version "$configured") ||
        version=$(bounded "$configured" --version 2>/dev/null | sed -n '1s/^putgd //p')
    if [ -z "$version" ] || ! newer "$best_version" "$(sortable "$version")"; then
        exec "$configured" "$@"
    fi
    say "starting $best, which is newer than $configured"
    exec "$best" "$@"
fi
if [ -n "$best" ]; then
    say "$configured is missing; starting $best"
    exec "$best" "$@"
fi
say "cannot start $configured: it is missing or not a private AppImage, and no put.gd AppImage with its update information is beside it"
exit 1
