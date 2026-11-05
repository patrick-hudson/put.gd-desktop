"""Narrow, per-overlay sudo authority using the golden's apt-get grant."""
import shlex
import time

from ..api import RecipeBlocked

COMMANDS = '/usr/bin/mount, /usr/bin/umount, /usr/sbin/nft'
# Seconds between removal attempts: apt's lists lock may be held (#762).
REMOVE_RETRY = (1, 3)


def apt_hook(scope, hook):
    # Same no-sources staging route as lab.stage_install_helper (#727).
    # The failing hook deliberately stops apt before it changes package lists.
    command = ('sudo -n /usr/bin/apt-get -o Dir::Etc::sourcelist=/dev/null '
               '-o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 '
               '-o '+shlex.quote('APT::Update::Pre-Invoke::='+hook+'; exit 1')+
               ' update >/dev/null 2>&1 || true')
    scope.ctx.shell(command)


def rule(token):
    return '# putgd fixture '+token+'\nlab ALL=(root) NOPASSWD: '+COMMANDS+'\n'


def leftovers(keep):
    """Root shell: remove rules an earlier run left (SIGKILL, apt lock at teardown).

    Only files named and worded exactly as ensure() writes them; one surface
    runner owns an overlay at a time.
    """
    return ('for f in /etc/sudoers.d/putgd-surface-*; do '
            'test -f "$f" || continue; test "$f" = '+keep+' && continue; t="${f##*/putgd-surface-}"; '
            'case "$t" in *[!0-9a-f]*) continue;; esac; test "${#t}" -eq 32 || continue; '
            'if test "$(cat -- "$f")" = "$(printf '+shlex.quote(rule('%s').strip())+' "$t")"; '
            'then rm -- "$f"; fi; done')


def granted(ctx):
    """How many rules grant COMMANDS per ``sudo -l``; None if it cannot be listed."""
    reply = ctx.shell('out=$(sudo -n -l 2>/dev/null) || exit 3; printf "%s\\n" "$out" | grep -cF -- '+
                      shlex.quote(COMMANDS)+'; true')
    try:
        return int(reply['stdout'].strip()) if not reply['returncode'] else None
    except (KeyError, ValueError):
        return None


def ensure(scope):
    ctx=scope.ctx
    if ctx.placement not in ('guest','boat') or not ctx.profile.get('guest'):
        raise RecipeBlocked('privileged fixtures require a disposable nested guest',issue=682)
    if scope.values.get('_privileges'):
        return
    scope.initialize()
    path='/etc/sudoers.d/putgd-surface-'+scope.token
    # Only these three executables (including their --version probes). No
    # shell, wildcard executable, sudo, or package-manager authority is added.
    text=rule(scope.token)
    if ctx.shell('test ! -e '+path)['returncode']:
        raise RecipeBlocked('fixture sudo rule path already exists; existing rule preserved')
    if granted(ctx):
        ctx.note('removing a fixture sudo rule an earlier run left in this overlay')
    def remove():
        # Bounded retries; removal is proven by sudo itself, not only the path.
        for delay in REMOVE_RETRY+(None,):
            apt_hook(scope,'set -eu; if test -e '+path+'; then test "$(cat '+path+')" = '+shlex.quote(text.strip())+'; rm -- '+path+'; fi')
            if not ctx.shell('test ! -e '+path)['returncode'] and not granted(ctx):
                return
            if delay is not None:
                time.sleep(delay)
        raise RecipeBlocked('owned fixture sudo rule remains in overlay')
    scope.defer('overlay sudo rule',remove,priority=-900)
    apt_hook(scope,'set -eu; '+leftovers(path)+'; test ! -e '+path+'; '+
             'printf %s '+shlex.quote(text)+' > '+path+'; chmod 440 '+path+'; '+
             'visudo -cf '+path+' >/dev/null || { rm -- '+path+'; exit 1; }')
    if ctx.shell('test -f '+path+' && test "$(stat -c %u:%a '+path+')" = 0:440')['returncode']:
        raise RecipeBlocked('cannot stage narrow overlay sudo rule via existing sudo -n apt-get',issue=682)
    if granted(ctx) not in (1, None):
        raise RecipeBlocked('a fixture sudo rule an earlier run left could not be removed',issue=762)
    scope.values['_privileges']=path
