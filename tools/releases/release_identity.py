"""Strict release grammar shared by tag verification and public asset inventories.

Stable `vX.Y.Z`, candidate `vX.Y.Z-rc.N` and nightly `vX.Y.Z-nightly.N`, where a
nightly's N is the full-history commit count of the tagged source.
"""
from dataclasses import dataclass
import re

_BASE = r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
_VERSION = re.compile(r'(?P<base>' + _BASE + r')(?:-rc\.(?P<candidate>[1-9][0-9]*)'
                      r'|-nightly\.(?P<count>[1-9][0-9]*))?')


@dataclass(frozen=True)
class ReleaseIdentity:
    version: str
    base_version: str
    package_version: str
    channel: str
    count: int | None = None

    @property
    def prerelease(self):
        return self.channel != 'stable'


def parse_version(version):
    match = _VERSION.fullmatch(version) if isinstance(version, str) else None
    if not match:
        raise ValueError('Exact stable, release candidate or nightly version required')
    base, candidate, count = match.group('base', 'candidate', 'count')
    if count:
        # Packages order ~nightly before ~rc and both before the stable base.
        return ReleaseIdentity(version, base, f'{base}~nightly.{count}', 'nightly', int(count))
    if candidate:
        return ReleaseIdentity(version, base, base + '~rc.' + candidate, 'candidate')
    return ReleaseIdentity(version, base, base, 'stable')


def parse_tag(tag):
    if not isinstance(tag, str) or not tag.startswith('v'):
        raise ValueError('Release tag must start with v')
    return parse_version(tag[1:])
