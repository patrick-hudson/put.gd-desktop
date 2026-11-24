# Add the repository on the first install only: %ghost keeps RPM from putting
# back a file the user deleted; rpm-postun.sh removes it on erase.
if [ "$1" -eq 1 ] && [ ! -e /etc/yum.repos.d/putgd.repo ] && [ -d /etc/yum.repos.d ]; then
  install -m 0644 /usr/share/putgd/repo/putgd.repo /etc/yum.repos.d/putgd.repo || :
fi
# Repository files written before Fedora 45 had its own build name Fedora 44's
# repository. Point only that baseurl at $releasever so a system upgrade
# fetches the matching build; other edits to the file stay as they are.
if [ "$1" -ge 2 ] && [ -f /etc/yum.repos.d/putgd.repo ]; then
  sed -i 's#^\(baseurl=https://[^[:space:]]*/rpm/fedora/\)44\(/x86_64/\(nightly\|stable\)\)$#\1$releasever\2#' \
    /etc/yum.repos.d/putgd.repo || :
fi
