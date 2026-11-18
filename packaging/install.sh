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
Native packages use sudo only for apt/dnf/pacman install, with prompts on your terminal.
On an Arch-family system other than Arch Linux itself (Omarchy, Manjaro) whose OpenSSL is older than
the one the package was built against, or where the installed putgd-cli does not start, the AppImage is
installed instead.
AppImage requires glibc 2.39+ and a working AppImage/FUSE runtime.
HELP
    }

    channel=stable
    package=
    # The OpenSSL soname the Arch package links (tools/releases/arch.py fails the build when it changes).
    arch_libcrypto='libcrypto.so=4-64'
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
    requested_channel=$channel
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
                fedora:45) package=fedora45 ;;
                arch:*) package=archlinux ;;
                *)
                    case " $ID_LIKE " in *' arch '*) package=archlinux ;; esac
                    case " $ID_LIKE :$UBUNTU_CODENAME" in
                        *' ubuntu '*:noble) package=ubuntu24.04 ;;
                    esac
                    ;;
            esac
            [ -n "$package" ] || note 'No matching distro package is published; using AppImage.'
        fi
        package=${package:-appimage}
    fi
    check_appimage_host() {
        libc=$(getconf GNU_LIBC_VERSION 2>/dev/null) \
            || die 'AppImage requires glibc 2.39+; use a supported distro or build from source'
        case "$libc" in 'glibc '*) ;; *) die 'AppImage requires glibc 2.39+' ;; esac
        awk -v version="${libc#glibc }" 'BEGIN {
            split(version, p, "."); exit !(p[1] > 2 || (p[1] == 2 && p[2] >= 39))
        }' || die "AppImage requires glibc 2.39+ (found $libc); upgrade your distro or build from source"
    }
    base=https://app.put.gd/dl
    # Check the exact download before fetching anything or asking for privilege.
    check_download() {
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
        case "$package" in
            appimage) suffix=-x86_64.AppImage ;;
            archlinux) suffix=-archlinux-x86_64.pkg.tar.zst ;;
            fedora44|fedora45) suffix=-$package-x86_64.rpm ;;
            *) suffix=-$package-x86_64.deb ;;
        esac
    }
    announce_package() {
        if [ "$package" = archlinux ]; then
            note "Installing $channel / $package using the configured pacman repositories and signature policy."
        else
            note "Installing $channel / $package; verifying the release SHA256SUMS before installation."
        fi
    }
    # Arch Linux itself takes the pacman package. Any other Arch-family system (Omarchy, Manjaro, EndeavourOS)
    # first decides between it and the AppImage, so the package's asset is checked only if the package is chosen:
    # a missing Arch asset must not stop an AppImage install.
    arch_probe_needed=false
    if [ "$package" = archlinux ] && { [ "${ID:-}" != arch ] || command -v omarchy-update >/dev/null 2>&1; }; then
        arch_probe_needed=true
    fi
    [ "$package" != appimage ] || check_appimage_host
    if ! "$arch_probe_needed"; then
        check_download
        announce_package
    fi

    if "$dry_run"; then
        if [ "$package" != archlinux ]; then
            note "Would download $base/$channel/checksums and $base/$channel/$package, then verify SHA-256."
        fi
        case "$package" in
            appimage) note 'Would install executable ~/.local/bin/putgd.AppImage (no sudo). Start: ~/.local/bin/putgd.AppImage' ;;
            archlinux)
                if command -v omarchy-update >/dev/null 2>&1; then
                    note "Would ask on /dev/tty, run Omarchy's system update, then configure and sync the put.gd repository and run pacman -S putgd."
                else
                    note 'Would trust the channel signing key, add /etc/pacman.d/putgd.conf to pacman.conf, and run pacman -Syu putgd from the signed pkg.put.gd repository.'
                fi
                note 'On an Arch-family system other than Arch Linux itself, the AppImage is installed instead when the installed OpenSSL is older than the package was built against.'
                note 'If the installed putgd-cli does not start, the pacman package is removed and the AppImage is installed instead.'
                ;;
            fedora44|fedora45)
                note "Would run sudo dnf install ./putgd-VERSION$suffix; package adds the signed pkg.put.gd update repository. Start: putgd"
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
        find "$work" -maxdepth 1 -type f -delete
        rmdir "$work"
        [ -z "$staged" ] || rm -f "$staged"
    }
    trap cleanup 0
    trap 'exit 1' 1 2 3 15
    fetch_package() {
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
    }
    install_appimage() {
        mkdir -p "$HOME/.local/bin"
        [ ! -d "$HOME/.local/bin/putgd.AppImage" ] || die "$HOME/.local/bin/putgd.AppImage is a directory"
        staged=$(mktemp "$HOME/.local/bin/.putgd-install.XXXXXX")
        cp "$work/$filename" "$staged"
        chmod 755 "$staged"
        mv -f "$staged" "$HOME/.local/bin/putgd.AppImage"
        staged=
        # AppRun manages its temporary desktop entries at launch; no install flag exists.
        note 'Installed. Start: ~/.local/bin/putgd.AppImage'
    }
    # The AppImage for the channel the user asked for, whatever channel or package was checked before.
    use_appimage() {
        note "$1"
        package=appimage
        channel=$requested_channel
        check_appimage_host
        check_download
        fetch_package
        install_appimage
    }
    # Pacman fetches and authenticates both the index and payload itself. Avoid
    # a second, redundant GitHub download before using the signed repository.
    if [ "$package" != archlinux ]; then
        fetch_package
    fi

    case "$package" in
        appimage) install_appimage ;;
        archlinux)
            arch_updated=false
            if command -v omarchy-update >/dev/null 2>&1; then
                if ! ( : < /dev/tty ) 2>/dev/null; then
                    die 'Omarchy needs a terminal for its system update. Run omarchy update, then rerun this installer in a terminal.'
                fi
                note "Omarchy detected: the installer will run Omarchy's system update before installing put.gd."
                printf 'Run the Omarchy system update now? [y/N] ' > /dev/tty
                IFS= read -r answer < /dev/tty || die 'Omarchy system update cancelled; nothing installed'
                case "$answer" in y|Y|yes|YES) ;; *) die 'Omarchy system update declined; nothing installed' ;; esac
                # Our explicit confirmation precedes -y so a declined native prompt
                # (which some updater versions report as success) cannot install put.gd.
                if ! omarchy-update -y < /dev/tty; then
                    if [ -f /etc/pacman.d/putgd.conf ] && [ ! -r /etc/pacman.d/putgd.conf ]; then
                        note 'The existing /etc/pacman.d/putgd.conf is not readable by pacman users (an older installer made it root-only). Run: sudo chmod 0644 /etc/pacman.d/putgd.conf'
                    fi
                    die 'Omarchy system update failed; nothing installed'
                fi
                arch_updated=true
            fi
            # Arch Linux itself builds the package against its own current OpenSSL, and pacman -Syu putgd brings
            # the system to it. Any other Arch-family system can still have an older one, where the putgd-cli and
            # the S3 libraries of the package would not load: ask pacman about the installed system only, after
            # Omarchy's own update, and change nothing of put.gd's (no key, no repository file, no chmod) unless
            # the package is chosen. A wrong AppImage is harmless; a wrong native install is not.
            arch_native=true
            if "$arch_probe_needed"; then
                command -v pacman >/dev/null 2>&1 || die 'Missing pacman'
                # A configuration of its own: an unreadable /etc/pacman.d/putgd.conf must not stop an unprivileged query.
                printf '[options]\nArchitecture = auto\n' > "$work/probe.conf"
                arch_probe=0
                pacman --config "$work/probe.conf" -T "$arch_libcrypto" > /dev/null || arch_probe=$?
                case "$arch_probe" in
                    0) ;;
                    127)
                        arch_native=false
                        use_appimage "This system's OpenSSL is not the one the native package was built against ($arch_libcrypto; Arch derivatives such as Omarchy follow Arch with a delay), so its putgd-cli and S3 and R2 uploads would not load. Using the AppImage."
                        # An installed native package keeps its files but cannot run putgd-cli or upload to S3 or R2 here.
                        if pacman -Q putgd >/dev/null 2>&1; then
                            note 'The put.gd pacman package installed here cannot run putgd-cli or upload to S3 or R2 on this system. Remove it with: sudo pacman -R putgd (the AppImage now installed replaces it).'
                        fi
                        ;;
                    *) die "pacman could not check whether $arch_libcrypto is installed (exit $arch_probe); nothing installed" ;;
                esac
            fi
            if "$arch_native"; then
                if "$arch_probe_needed"; then
                    check_download
                    announce_package
                fi
                fetch --output "$work/putgd.asc" "https://pkg.put.gd/keys/putgd-$channel.asc"
                # Pinned channel fingerprint comes from the public key shipped in source.
                case "$channel" in
                    nightly) fingerprint=4032E28D893BAF66F4AC23D9A11D38D55536B371 ;;
                    stable) fingerprint=C0962EFA929F5A5477A4509C0CC9E5A0F0B6FC1C ;;
                esac
                command -v sudo >/dev/null 2>&1 || die 'Missing sudo'
                sudo sh -c '
                    set -eu
                    # Repository configuration contains public URLs and must be readable by pacman users.
                    umask 022
                    actual=$(gpg --show-keys --with-colons "$1" | awk -F: '\''$1 == "fpr" { print $10; exit }'\'')
                    [ "$actual" = "$2" ] || exit 1
                    pacman-key --add "$1"
                    pacman-key --lsign-key "$2"
                    if [ ! -e /etc/pacman.d/putgd.conf ]; then
                        printf "[putgd-%s]\nSigLevel = PackageRequired DatabaseRequired\nServer = https://pkg.put.gd/pacman/x86_64/%s\n" "$3" "$3" > /etc/pacman.d/putgd.conf
                    fi
                    chmod 0644 /etc/pacman.d/putgd.conf
                    if ! grep -qxF "Include = /etc/pacman.d/putgd.conf" /etc/pacman.conf; then
                        printf "\nInclude = /etc/pacman.d/putgd.conf\n" >> /etc/pacman.conf
                    fi
                    if [ "$4" = true ]; then
                        # Omarchy refreshed the system repositories before this file existed.
                        # Sync only the newly configured repository, then install normally.
                        pacman --config /etc/pacman.d/putgd.conf -Sy
                        pacman -S putgd
                    else
                        pacman -Syu putgd
                    fi
                ' sh "$work/putgd.asc" "$fingerprint" "$channel" "$arch_updated" < /dev/tty
                # The proof that counts: pacman -T cannot predict what this transaction replaced (an OpenSSL with
                # another soname, say), so run the installed putgd-cli. If it does not start, take the package
                # back out and use the AppImage.
                if ! putgd-cli --version > /dev/null 2>&1; then
                    note 'The pacman package installed, but its putgd-cli does not start on this system (a shared library it needs is missing or does not match). Removing the package and using the AppImage.'
                    sudo pacman -R --noconfirm putgd < /dev/tty \
                        || die 'The pacman package does not start and could not be removed. Remove it with: sudo pacman -R putgd; then use --appimage'
                    use_appimage 'Installing the AppImage instead.'
                fi
            fi
            ;;
        *)
            command -v sudo >/dev/null 2>&1 || die 'Missing sudo; install sudo or use --appimage'
            case "$package" in fedora*) manager=dnf ;; *) manager=apt ;; esac
            command -v "$manager" >/dev/null 2>&1 || die "Missing $manager; use --appimage"
            note "Using sudo only for $manager install; sudo and $manager may ask for confirmation on your terminal."
            if [ "$manager" = dnf ]; then
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
            'GNOME Shell 50'*) note 'GNOME 50: put.gd will ask you to log out and back in once to finish setting up.' ;;
        esac
    fi
}

main "$@"
