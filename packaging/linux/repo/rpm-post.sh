# Add the repository on the first install only: %ghost keeps RPM from putting
# back a file the user deleted; rpm-postun.sh removes it on erase.
if [ "$1" -eq 1 ] && [ ! -e /etc/yum.repos.d/putgd.repo ] && [ -d /etc/yum.repos.d ]; then
  install -m 0644 /usr/share/putgd/repo/putgd.repo /etc/yum.repos.d/putgd.repo || :
fi
