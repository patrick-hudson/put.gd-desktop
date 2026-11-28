#!/bin/sh
# put.gd file manager launcher (#1105). Every per-user "Upload with put.gd"
# entry of the AppImage runs this file, which starts the current put.gd
# AppImage. put.gd writes it and rewrites it at every start; edits are replaced.
# X-putgd-Managed=true
# X-putgd-IntegrationId=__PUTGD_ID__
set -u
configured=__PUTGD_CONFIGURED__
update_information=__PUTGD_UPDATE_INFORMATION__
script=__PUTGD_SCRIPT__
record=__PUTGD_RECORD__
own_directory=__PUTGD_DIRECTORY__
user_runtime=__PUTGD_USER_RUNTIME__
cgroup=__PUTGD_CGROUP__
integration_id='__PUTGD_ID__'
watch_unit=__PUTGD_WATCH_UNIT__
watch_description="put.gd file manager integration $integration_id"
cleanup_only=false

say() {
    printf 'put.gd file manager launcher: %s\n' "$*" >&2
}

# The user manager listens in the login's runtime directory, and systemctl and
# systemd-run look for it only in XDG_RUNTIME_DIR (systemd 259 no longer falls
# back to the session bus). A process given a private XDG_RUNTIME_DIR still
# belongs to that manager, and the unit outlives the process anyway.
manager_runtime() {
    for directory in ${XDG_RUNTIME_DIR:+"$XDG_RUNTIME_DIR"} "$user_runtime"; do
        if [ -S "$directory/systemd/private" ]; then
            printf '%s' "$directory"
            return 0
        fi
    done
    return 1
}

# The user manager owns the removal watch, separately from login startup's: it
# exists while the integration is on, whether or not Launch at login is.
manager() {
    runtime=$(manager_runtime) || return 1
    env -u LD_LIBRARY_PATH -u LD_PRELOAD XDG_RUNTIME_DIR="$runtime" systemctl --user "$@"
}

unit_exists() {
    load_state=$(manager show --property=LoadState --value "$1" 2>/dev/null) || return 1
    [ -n "$load_state" ] && [ "$load_state" != not-found ]
}

# Stop this integration's units: the current one, or one armed for an earlier
# image path (the same unit name, another description). Never a foreign unit.
stop_watch() {
    for suffix in "$@"; do
        unit_exists "$watch_unit.$suffix" || continue
        description=$(manager show --property=Description --value "$watch_unit.$suffix" 2>/dev/null) || continue
        case $description in
            'put.gd file manager integration '*) ;;
            *) return 1 ;;
        esac
        manager stop "$watch_unit.$suffix" >/dev/null 2>&1 || return 1
    done
}

# Exit status 3: no user manager to arm a watch in. Nothing is wrong then; a
# menu entry used after the image is gone removes the entries itself. Any other
# failure says why on stderr, which the resident logs.
install_watch() {
    runtime=$(manager_runtime) || {
        say "no user service manager is reachable"
        return 3
    }
    if unit_exists "$watch_unit.path"; then
        description=$(manager show --property=Description --value "$watch_unit.path" 2>/dev/null) || {
            say "cannot read $watch_unit.path from the user manager"
            return 1
        }
        if [ "$description" = "$watch_description" ] && manager is-active --quiet "$watch_unit.path"; then
            return 0
        fi
        stop_watch path service || {
            say "cannot replace $watch_unit, described as: $description"
            return 1
        }
    fi
    # Updates write the next image beside this one and may remove the old one.
    failure=$(env -u LD_LIBRARY_PATH -u LD_PRELOAD XDG_RUNTIME_DIR="$runtime" systemd-run --user --quiet --collect \
        --expand-environment=no --unit="$watch_unit" --description="$watch_description" \
        --path-property="PathChanged=${configured%/*}" --property=Type=oneshot \
        --property=SuccessExitStatus=SIGTERM \
        /bin/sh "$script" --cleanup-if-missing 2>&1 >/dev/null) || {
        say "systemd-run: $failure"
        return 1
    }
    # PathChanged does not report an image removed before the watch was armed.
    failure=$(manager start --no-block "$watch_unit.service" 2>&1 >/dev/null) || {
        say "systemctl start: $failure"
        return 1
    }
}

# A private regular file of ours, not a symbolic link.
ours() {
    [ -f "$1" ] && [ ! -L "$1" ] && [ -O "$1" ]
}

remove_owned() {
    case $1 in
        # The launcher's own folder holds only put.gd's files.
        "$own_directory"/*) ours "$1" && rm -f -- "$1" ;;
        *) ours "$1" && grep -Fq -- 'X-putgd-Managed=true' "$1" && rm -f -- "$1" ;;
    esac
    return 0
}

# Remove put.gd's COSMIC context action: exactly the one line put.gd wrote,
# found exactly once. COSMIC Files never rewrites the list itself; after a
# hand edit the line may differ, and the entry is left (its command then fails
# harmlessly).
remove_cosmic_line() {
    ours "$1" || return 0
    [ "$(grep -Fxc -- "$2" "$1" 2>/dev/null)" = 1 ] || return 0
    mode=$(stat -c '%a' -- "$1" 2>/dev/null) || return 0
    staged=$(mktemp -- "$1.putgd-XXXXXX") || return 0
    if cp -- "$1" "$1.putgd-backup" && grep -Fxv -- "$2" "$1" >"$staged" && chmod -- "$mode" "$staged" &&
        mv -f -- "$staged" "$1"; then
        return 0
    fi
    rm -f -- "$staged"
}

# Remove only what this integration's record says it owns, keeping settings.
cleanup_missing() {
    ours "$record" && grep -Fxq -- '# X-putgd-Managed=true' "$record" &&
        grep -Fxq -- "# X-putgd-IntegrationId=$integration_id" "$record" || return 0
    cosmic=
    cosmic_line=
    while IFS= read -r line; do
        case $line in
            'file '*) remove_owned "${line#file }" ;;
            'cosmic '*) cosmic=${line#cosmic } ;;
            'cosmic-line '*) cosmic_line=${line#cosmic-line } ;;
        esac
    done <"$record"
    [ -z "$cosmic" ] || [ -z "$cosmic_line" ] || remove_cosmic_line "$cosmic" "$cosmic_line"
    rm -f -- "$record"
    stop_watch path
}

start_target() {
    target=$1; shift
    [ "$cleanup_only" != true ] || exit 0
    # The client's notifications show this stable icon copy: the image's own
    # icon is inside a mount that goes away with each launch.
    [ ! -f "$own_directory/putgd.svg" ] || export PUTGD_UPLOAD_ICON="$own_directory/putgd.svg"
    exec "$target" "$@"
}

# A file manager started by D-Bus activation runs in its own transient
# .service cgroup, which systemd kills, with this client and the AppImage's
# FUSE server, when the file manager exits. Only the leaf unit counts: every
# user process sits below user@UID.service.
in_service() {
    command -v systemd-run >/dev/null 2>&1 || return 1
    leaf=$(sed -n 's/^0::\(.*\)$/\1/p' -- "$cgroup" 2>/dev/null) || return 1
    [ -n "$leaf" ] || leaf=$(sed -n 's/^[0-9]*:name=systemd:\(.*\)$/\1/p' -- "$cgroup" 2>/dev/null) || return 1
    leaf=${leaf%/}
    case ${leaf##*/} in
        *.service) return 0 ;;
    esac
    return 1
}

case ${1-} in
    --install-watch) install_watch; exit $? ;;
    --stop-watch)
        # No manager means nothing is armed.
        manager_runtime >/dev/null || exit 0
        stop_watch path service
        exit $?
        ;;
    --cleanup-if-missing)
        cleanup_only=true; shift
        # File replacements during an update must settle before checking.
        sleep 2
        ;;
    --putgd-scoped)
        # Started in its own scope: tell the waiting launcher, which otherwise
        # runs the upload itself. Without that signal this copy must not run.
        shift
        { printf s >&3; } 2>/dev/null || exit 0
        exec 3>&-
        ;;
    *)
        # Leave the file manager's cgroup on the host, before the image mounts.
        # --expand-environment=no keeps a selected ${USER}.txt as it is. If
        # systemd-run cannot start the scope, run here instead.
        if in_service; then
            exec 4>&1
            signal=$( { systemd-run --user --scope --quiet --expand-environment=no -- \
                /bin/sh "$script" --putgd-scoped "$@" 3>&1 >&4 4>&-; printf '%s' "$?"; } )
            exec 4>&-
            case $signal in
                s*) exit "${signal#s}" ;;
            esac
        fi
        ;;
esac

if [ "$cleanup_only" != true ] && [ ! -e "$configured" ]; then
    sleep 2
fi

# __PUTGD_DISCOVERY__

if [ ! -e "$configured" ]; then
    cleanup_missing
    [ "$cleanup_only" = true ] || say "$configured is missing, and no put.gd AppImage with its update information is beside it"
    [ "$cleanup_only" = true ]
    exit $?
fi
say "cannot start $configured: it is not executable"
exit 1
