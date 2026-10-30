"""Lazy publication and truthful running-build release descriptors."""
from ..api import RecipeBlocked
from .base import Fixture
from .uploads import upload


def notification_upload(scope):
    # 'needs' resolves before the browser guard; return a lazy descriptor so the
    # actual publication happens only on first recipe access after that guard.
    return LazyUpload(lambda:upload(scope))


class LazyUpload(dict):
    def __init__(self, factory):
        super().__init__();self.factory=factory;self.loaded=False
    def load(self):
        if not self.loaded:
            self.update(self.factory());self.loaded=True
    def get(self,*args):self.load();return super().get(*args)
    def __getitem__(self,key):self.load();return super().__getitem__(key)
    def __getattr__(self,key):
        try:return self[key]
        except KeyError as error:raise AttributeError(key) from error


def release(scope):
    status=scope.ctx.call('update.status')
    build=status.get('build',{})
    link=build.get('releaseLink') or scope.ctx.call('ui.snapshot').get('diagnostics',{}).get('release')
    if not isinstance(link,dict) or not link.get('url'):
        raise RecipeBlocked('running untagged build has no release link; fixture will not relabel it',issue=691)
    return Fixture(expect={'kind':'release','name':link.get('version'),'preview':False,'url_host':'github.com','text':link.get('version')})
