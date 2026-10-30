"""#762 live: a rule left by a killed run is removed by the next staging; removal is proven by sudo -l."""
import json, sys, time, uuid
from pathlib import Path
sys.path.insert(0, sys.argv[1])
from tools.lab import lab
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.fixtures import privileges
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.target import Target
RUN = Path(sys.argv[2]).resolve()
root = '/tmp/sd-sig815s-' + uuid.uuid4().hex[:6]
target = Target(run=RUN, session_root=root)
print(lab.guest_session(RUN, 'mkdir -m 700 ' + root + ' && for d in runtime config data cache state; do mkdir -m 700 ' + root + '/$d; done; echo root', timeout=60).strip())
env = dict(desktop='mint-cinnamon-x11', host='boat', placement='guest', guest='cinnamon-x11', package='deb', destination='local')
ctx = DriverContext(target, env, load_profile('mint-cinnamon-x11'), '/tmp', '/tmp')
ctx._deadline = time.monotonic() + 600
listing = lambda: lab.guest_session(RUN, 'set +e\nsudo -n -l 2>&1 | grep -F "/usr/bin/mount, /usr/bin/umount, /usr/sbin/nft"; ls /etc/sudoers.d/ | grep putgd-surface; true', timeout=60).strip()
leftover = uuid.uuid4().hex
path = '/etc/sudoers.d/putgd-surface-' + leftover
# Plant what a SIGKILLed run leaves behind, through the same apt hook route.
scope0 = ctx.fixture('launcher') and ctx._fixture_manager.scope
privileges.apt_hook(scope0, "set -eu; printf %s '" + privileges.rule(leftover) + "' > " + path + '; chmod 440 ' + path)
print('planted:', listing().splitlines())
scope = ctx._fixture_manager.scope
privileges.ensure(scope)
print('notes:', ctx.notes)
print('after staging:', listing().splitlines())
errors = ctx._fixture_manager.close()
print('close errors:', errors)
print('after close:', listing().splitlines() or ['(no rule, no file)'])
print(lab.guest_session(RUN, 'rm -rf -- ' + root + '; echo root removed', timeout=60).strip())
