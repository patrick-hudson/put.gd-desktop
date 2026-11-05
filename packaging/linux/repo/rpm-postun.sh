# Erase (not upgrade): remove the repository file the first install added.
# RPM leaves a %ghost %config file in place on erase.
if [ "$1" -eq 0 ]; then
  rm -f /etc/yum.repos.d/putgd.repo /etc/yum.repos.d/putgd.repo.rpmsave || :
fi
