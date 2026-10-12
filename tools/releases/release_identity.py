"""Strict release grammar shared by tag verification and public asset inventories."""
from dataclasses import dataclass
import datetime
import re

_BASE = r'(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
_VERSION = re.compile(r'(?P<base>' + _BASE + r')(?:(?:-rc\.(?P<candidate>[1-9][0-9]*))|'
                      r'(?:-dev\.(?P<count>[1-9][0-9]*)\.(?P<utc>[0-9]{14})'
                      r'\+g(?P<short>[0-9a-f]{12})))?')


@dataclass(frozen=True)
class ReleaseIdentity:
    version: str
    base_version: str
    package_version: str
    channel: str
    count: int | None = None
    utc: str | None = None
    short_commit: str | None = None

    @property
    def prerelease(self):
        return self.channel != 'stable'


def parse_version(version):
    match = _VERSION.fullmatch(version) if isinstance(version, str) else None
    if not match:
        raise ValueError('Exact stable, release candidate or immutable nightly version required')
    base, candidate, count, utc, short = match.group('base', 'candidate', 'count', 'utc', 'short')
    if count:
        try:
            datetime.datetime.strptime(utc, '%Y%m%d%H%M%S')
        except ValueError as error:
            raise ValueError('Nightly timestamp must be a valid UTC date and time') from error
        return ReleaseIdentity(version, base, f'{base}~dev.{count}.{utc}+g{short}',
                               'nightly', int(count), utc, short)
    if candidate:
        return ReleaseIdentity(version, base, base + '~rc.' + candidate, 'candidate')
    return ReleaseIdentity(version, base, base, 'stable')


def parse_tag(tag):
    if not isinstance(tag, str) or not tag.startswith('v'):
        raise ValueError('Release tag must start with v')
    return parse_version(tag[1:])
