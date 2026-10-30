set -eu
export DEBIAN_FRONTEND=noninteractive
cd /packages
sha256sum *.deb
dpkg-deb -f production.deb Package Version Description
dpkg-deb -f fixture.deb Package Version Conflicts Replaces Description
apt-get update
apt-get install -y ./production.deb
dpkg -i production.deb
echo PRODUCTION_INSTALL_PASSED
set +e
dpkg -i fixture.deb > fixture-install.log 2>&1
fixture_status=$?
set -e
cat fixture-install.log
echo FIXTURE_INSTALL_STATUS=$fixture_status
test "$fixture_status" = 0
test "$(dpkg-query -W -f='${db:Status-Status}' putgd-lab-fixture)" = installed
if test "$(dpkg-query -W -f='${db:Status-Status}' putgd 2>/dev/null || true)" = installed; then exit 20; fi
set +e
dpkg -i production.deb > reverse-install.log 2>&1
reverse_status=$?
set -e
cat reverse-install.log
echo REVERSE_INSTALL_STATUS=$reverse_status
test "$reverse_status" != 0
grep -qi conflicts reverse-install.log
if grep -qi 'trying to overwrite' fixture-install.log reverse-install.log; then exit 21; fi
dpkg-query -W -f='${Package}: ${db:Status-Status}\n' putgd putgd-lab-fixture || true
echo FIXTURE_CONFLICT_PROOF_PASSED
