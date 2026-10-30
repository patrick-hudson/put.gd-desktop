"""Narrow, per-overlay sudo authority using the golden's apt-get grant."""
import shlex

from ..api import RecipeBlocked


def apt_hook(scope, hook):
    # Same no-sources staging route as lab.stage_install_helper (#727).
    # The failing hook deliberately stops apt before it changes package lists.
    command = ('sudo -n /usr/bin/apt-get -o Dir::Etc::sourcelist=/dev/null '
               '-o Dir::Etc::sourceparts=- -o APT::Get::List-Cleanup=0 '
               '-o '+shlex.quote('APT::Update::Pre-Invoke::='+hook+'; exit 1')+
               ' update >/dev/null 2>&1 || true')
    scope.ctx.shell(command)


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
    rule='# putgd fixture '+scope.token+'\nlab ALL=(root) NOPASSWD: /usr/bin/mount, /usr/bin/umount, /usr/sbin/nft\n'
    if ctx.shell('test ! -e '+path)['returncode']:
        raise RecipeBlocked('fixture sudo rule path already exists; existing rule preserved')
    def remove():
        apt_hook(scope,'set -eu; if test -e '+path+'; then test "$(cat '+path+')" = '+shlex.quote(rule.strip())+'; rm -- '+path+'; fi')
        if ctx.shell('test ! -e '+path)['returncode']:
            raise RecipeBlocked('owned fixture sudo rule remains in overlay')
    scope.defer('overlay sudo rule',remove,priority=-900)
    apt_hook(scope,'set -eu; test ! -e '+path+'; '+
             'printf %s '+shlex.quote(rule)+' > '+path+'; chmod 440 '+path+'; '+
             'visudo -cf '+path+' >/dev/null || { rm -- '+path+'; exit 1; }')
    if ctx.shell('test -f '+path+' && test "$(stat -c %u:%a '+path+')" = 0:440')['returncode']:
        raise RecipeBlocked('cannot stage narrow overlay sudo rule via existing sudo -n apt-get',issue=682)
    scope.values['_privileges']=path
