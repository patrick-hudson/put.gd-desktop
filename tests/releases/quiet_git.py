"""Keep throwaway fixture repositories free of detached git background work.

A commit can start `git gc --auto` or `git maintenance` in the background. If a
fixture's TemporaryDirectory is removed while it still writes `.git/objects`,
cleanup fails with "Directory not empty" (#99). Importing this module turns both
off for every git process the test starts, through git's environment config.
"""
import os

SETTINGS = (('gc.auto', '0'), ('maintenance.auto', 'false'))


def disable_background_maintenance(environ=os.environ):
    count = int(environ.get('GIT_CONFIG_COUNT') or 0)
    for offset, (key, value) in enumerate(SETTINGS):
        environ[f'GIT_CONFIG_KEY_{count + offset}'] = key
        environ[f'GIT_CONFIG_VALUE_{count + offset}'] = value
    environ['GIT_CONFIG_COUNT'] = str(count + len(SETTINGS))


if not os.environ.get('PUTGD_QUIET_GIT_APPLIED'):
    disable_background_maintenance()
    os.environ['PUTGD_QUIET_GIT_APPLIED'] = '1'
