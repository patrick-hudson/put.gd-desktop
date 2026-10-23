# VM legs (Ubuntu 26.04 GNOME on d69, Aurora 44 KDE on f44), N=26.1022.1017-nightly -> N+1=26.1022.1019-nightly (live)
Ubuntu: overlay of gnome-provisioned-20261016 (transfer-0d2922e34f4b, sha 563730cc… before/after), run ubu-4c0235, ssh 22222, vnc 5922.
Aurora: overlay of aurora-stable-44.20260908.1-v3-492c3f8945d1 (sha 6a2fb407… before/after), run aur-3e84a1, ssh 22426, vnc 5926.
U2/U4 Ubuntu: DEB N from file; Settings: repository putgd.sources, Nightly, "1019 available", Or in a terminal; apt-get install --only-upgrade putgd -> 26.1022.1019~nightly; restart -> Up to date. Purged.
U3 Ubuntu: no Open software updates button (action software-updates, updater None, command shown). Stock 26.04 has update-manager (Software Updater) + snap-store, not gnome-software; SystemUpdates.cpp knows only mintupdate/plasma-discover/gnome-software. update-manager launched by hand opens and offers 150 MB of updates.
U7 both: real click Download and install -> zsync installed; real click Restart -> 1019, backup removed.
U9 Gear Lever 4.6.2 both; AppImageLauncher 3.0.0-beta-2 (gha287, sha 41175521…) Ubuntu only. Integrate N -> menu-entry resident -> update.install (CLI, not a click) -> download method -> file = 1019 SHA256SUMS at same path; restart -> 1019, backup removed; quit + menu start -> 1019; entry byte-identical; GL --list-updates "No updates available"; AIL updater window "No updates found"; one visible entry each. GL's own --update on a fresh N -> e5b7ad2c… = 1019.
U11 both: Launch at login on N (private-profile run, entry in real autostart), install without restart, session reboot (gnome-session-quit --reboot / org.kde.Shutdown.logoutAndReboot), autologin started the same path = 1019.
U14 both: notifications on, no Check now; scheduled check found 1019; dbus-monitor saw no put.gd Notify.
U15: Ubuntu A 29.21 MiB RSS median / Pss 22.76 / priv 22.42; B 29.30/22.85. Aurora A 30.05 / Pss 23.55 / priv 23.18; B 30.14/23.65. No TLS/network libs mapped.
