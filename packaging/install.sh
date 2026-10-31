#!/bin/sh
# Published by put.gd-site at https://app.put.gd/install.
set -eu

main() {
    die() { printf 'put.gd: %s\n' "$*" >&2; exit 1; }
    note() { printf 'put.gd: %s\n' "$*"; }
    fetch() {
        curl --proto '=https' --proto-redir '=https' --connect-timeout 30 \
            --speed-limit 1024 --speed-time 60 --retry 2 --fail --silent --show-error --location "$@"
    }
    usage() {
        cat <<'HELP'
Install put.gd for x86_64 Linux (curl and sha256sum required).
Usage: curl -fsSL https://app.put.gd/install | sh -s -- [OPTIONS]
  --channel stable|nightly  Release channel (default: stable; nightly if none exists)
  --appimage                Install ~/.local/bin/putgd.AppImage without sudo
  --dry-run                 Show the plan; only checks channel availability online
  --help                    Show this help
Native packages use sudo only for apt/dnf install, with prompts on your terminal.
AppImage requires glibc 2.39+ and a working AppImage/FUSE runtime.
HELP
    }

    channel=stable
    package=
    dry_run=false
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --channel)
                [ "$#" -ge 2 ] || die '--channel needs stable or nightly'
                channel=$2; shift
                case "$channel" in stable|nightly) ;; *) die '--channel needs stable or nightly' ;; esac
                ;;
            --appimage) package=appimage ;;
            --dry-run) dry_run=true ;;
            --help|-h) usage; return ;;
            *) die "Unknown option: $1 (see --help)" ;;
        esac
        shift
    done
    [ "$(uname -m)" = x86_64 ] || die 'Unsupported architecture; only x86_64 is published'
    command -v curl >/dev/null 2>&1 || die 'Missing curl; install it with your package manager first'
    command -v sha256sum >/dev/null 2>&1 || die 'Missing sha256sum; install coreutils first'

    if [ -z "$package" ]; then
        ID='' VERSION_ID='' ID_LIKE='' UBUNTU_CODENAME=''
        if [ -r /etc/os-release ]; then
            # Local, distro-owned configuration, never downloaded shell code.
            # shellcheck source=/dev/null
            . /etc/os-release
        fi
        if [ -e /run/ostree-booted ]; then
            note 'OSTree/Atomic system detected; using AppImage to avoid changing the system image.'
        else
            case "$ID:$VERSION_ID" in
                ubuntu:24.04|kubuntu:24.04|linuxmint:22|linuxmint:22.*) package=ubuntu24.04 ;;
                ubuntu:26.04|kubuntu:26.04) package=ubuntu26.04 ;;
                debian:13|debian:13.*) package=debian13 ;;
                fedora:44) package=fedora44 ;;
                *)
                    case " $ID_LIKE :$UBUNTU_CODENAME" in
                        *' ubuntu '*:noble) package=ubuntu24.04 ;;
                    esac
                    ;;
            esac
            [ -n "$package" ] || note 'No matching distro package is published; using AppImage.'
        fi
        package=${package:-appimage}
    fi
    if [ "$package" = appimage ]; then
        libc=$(getconf GNU_LIBC_VERSION 2>/dev/null) \
            || die 'AppImage requires glibc 2.39+; use a supported distro or build from source'
        case "$libc" in 'glibc '*) ;; *) die 'AppImage requires glibc 2.39+' ;; esac
        awk -v version="${libc#glibc }" 'BEGIN {
            split(version, p, "."); exit !(p[1] > 2 || (p[1] == 2 && p[2] >= 39))
        }' || die "AppImage requires glibc 2.39+ (found $libc); upgrade your distro or build from source"
    fi

    base=https://app.put.gd/dl
    # Check the exact download before fetching anything or asking for privilege.
    status=$(curl --proto '=https' --connect-timeout 30 --max-time 60 --silent \
        --show-error --output /dev/null --write-out '%{http_code}' "$base/$channel/$package") \
        || die 'Could not check release availability'
    if [ "$status" = 404 ] && [ "$channel" = stable ]; then
        note "No stable $package download exists yet; falling back to nightly."
        channel=nightly
        status=$(curl --proto '=https' --connect-timeout 30 --max-time 60 --silent \
            --show-error --output /dev/null --write-out '%{http_code}' "$base/$channel/$package") \
            || die 'Could not check nightly release availability'
    fi
    [ "$status" = 302 ] || die "No usable $channel $package download (HTTP $status)"
    note "Installing $channel / $package; verifying the release SHA256SUMS before installation."

    case "$package" in
        appimage) suffix=-x86_64.AppImage ;;
        fedora44) suffix=-fedora44-x86_64.rpm ;;
        *) suffix=-$package-x86_64.deb ;;
    esac
    if "$dry_run"; then
        note "Would download $base/$channel/checksums and $base/$channel/$package, then verify SHA-256."
        case "$package" in
            appimage) note 'Would install executable ~/.local/bin/putgd.AppImage (no sudo). Start: ~/.local/bin/putgd.AppImage' ;;
            fedora44)
                note 'Would run sudo dnf install ./putgd-VERSION-fedora44-x86_64.rpm; package adds the signed pkg.put.gd update repository. Start: putgd'
                note 'Then, in the same sudo step, if noopenh264 is installed or either codec package is missing and fedora-cisco-openh264 is enabled: dnf install -y openh264 gstreamer1-plugin-openh264'
                note 'If the codec is needed but the repo is disabled, print: sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1; sudo dnf install -y openh264 gstreamer1-plugin-openh264'
                ;;
            *) note "Would run sudo apt install ./putgd-VERSION$suffix; package adds the signed pkg.put.gd update repository. Start: putgd" ;;
        esac
        # Keep the native command legible without relying on a known release version.
        return
    fi

    umask 077
    work=$(mktemp -d "${TMPDIR:-/tmp}/putgd-install.XXXXXX") || die 'Could not create a private download directory'
    staged=
    cleanup() {
        rm -rf "$work"
        [ -z "$staged" ] || rm -f "$staged"
    }
    trap cleanup 0
    trap 'exit 1' 1 2 3 15
    fetch --output "$work/SHA256SUMS" "$base/$channel/checksums" || die 'Could not download SHA256SUMS'
    # Select exactly one safe basename and digest; never execute release metadata.
    entry=$(awk -v suffix="$suffix" '
        NF == 2 && length($1) == 64 && $1 !~ /[^0-9a-fA-F]/ &&
        $2 ~ /^putgd-[A-Za-z0-9._+-]+$/ &&
        substr($2, length($2) - length(suffix) + 1) == suffix { line = $1 "  " $2; count++ }
        END { if (count != 1) exit 1; print line }
    ' "$work/SHA256SUMS") || die "SHA256SUMS must name exactly one $package package"
    filename=${entry#*  }
    printf '%s\n' "$entry" > "$work/selected.sha256"
    fetch --output "$work/$filename" "$base/$channel/$package" || die 'Package download failed'
    (cd "$work" && sha256sum --check selected.sha256) \
        || die 'SHA-256 verification failed; nothing installed. A release may have changed; retry.'

    case "$package" in
        appimage)
            mkdir -p "$HOME/.local/bin"
            [ ! -d "$HOME/.local/bin/putgd.AppImage" ] || die "$HOME/.local/bin/putgd.AppImage is a directory"
            staged=$(mktemp "$HOME/.local/bin/.putgd-install.XXXXXX")
            cp "$work/$filename" "$staged"
            chmod 755 "$staged"
            mv -f "$staged" "$HOME/.local/bin/putgd.AppImage"
            staged=
            # AppRun manages its temporary desktop entries at launch; no install flag exists.
            note 'Installed. Start: ~/.local/bin/putgd.AppImage'
            ;;
        *)
            command -v sudo >/dev/null 2>&1 || die 'Missing sudo; install sudo or use --appimage'
            if [ "$package" = fedora44 ]; then manager=dnf; else manager=apt; fi
            command -v "$manager" >/dev/null 2>&1 || die "Missing $manager; use --appimage"
            note "Using sudo only for $manager install; sudo and $manager may ask for confirmation on your terminal."
            if [ "$package" = fedora44 ]; then
                # Fixed local commands only; the verified basename is a positional argument.
                (cd "$work" && sudo sh -c '
                    set -eu
                    dnf install "$1"
                    if rpm -q noopenh264 >/dev/null 2>&1 || ! rpm -q openh264 gstreamer1-plugin-openh264 >/dev/null 2>&1; then
                        if LC_ALL=C dnf -q repolist --enabled | awk '\''$1 == "fedora-cisco-openh264" { found=1 } END { exit !found }'\''; then
                            printf "%s\n" "put.gd: Installing Cisco OpenH264 for recording; replacing the Fedora placeholder if present."
                            dnf install -y openh264 gstreamer1-plugin-openh264
                        else
                            printf "%s\n" "put.gd: Recording needs Cisco OpenH264; fedora-cisco-openh264 is disabled or unavailable. Run:"
                            printf "%s\n" "sudo dnf config-manager setopt fedora-cisco-openh264.enabled=1" "sudo dnf install -y openh264 gstreamer1-plugin-openh264"
                        fi
                    fi
                ' sh "./$filename") < /dev/tty
            else
                (cd "$work" && sudo "$manager" install "./$filename") < /dev/tty
            fi \
                || die 'Package installation failed (a terminal is required); use --appimage for a user install'
            note 'The package adds the signed pkg.put.gd repository for updates. Start: putgd'
            ;;
    esac
    if command -v gnome-shell >/dev/null 2>&1; then
        case "$(gnome-shell --version 2>/dev/null || true)" in
            'GNOME Shell 50'*) note 'GNOME 50: log out and back in, then run: gnome-extensions enable putgd@put.gd (AppImage users must install the host extension first; see the README).' ;;
        esac
    fi
}

main "$@"
