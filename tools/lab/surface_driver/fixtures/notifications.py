"""Lazy publication and truthful running-build release descriptors."""
from collections.abc import Mapping

from ..api import RecipeBlocked
from .base import Fixture
from .uploads import upload


def notification_upload(scope):
    # 'needs' resolves before the browser guard; return a lazy descriptor so the
    # actual publication happens only on first recipe access after that guard.
    return LazyUpload(lambda:upload(scope))


class LazyUpload(Mapping):
    """Explicit read activates publication; introspection/serialization does not."""
    def __init__(self, factory):
        self.factory=factory
        self.value=None
    def load(self):
        if self.value is None:
            self.value=self.factory()
        return self.value
    def __getitem__(self,key):return self.load()[key]
    def __iter__(self):return iter(self.load())
    def __len__(self):return len(self.load())
    def __getattr__(self,key):
        if key.startswith('__'):
            raise AttributeError(key)
        try:return self[key]
        except KeyError as error:raise AttributeError(key) from error


def release(scope):
    status=scope.ctx.call('update.status')
    build=status.get('build',{})
    link=build.get('releaseLink') or scope.ctx.call('ui.snapshot').get('diagnostics',{}).get('release')
    if not isinstance(link,dict) or not link.get('url'):
        raise RecipeBlocked('running untagged build has no release link; fixture will not relabel it',issue=691)
    return Fixture(expect={'kind':'release','name':link.get('version'),'preview':False,'url_host':'github.com','text':link.get('version')})
