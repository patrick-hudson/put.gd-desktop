"""Strict release grammar shared by tag verification and public asset inventories.

Releases use calendar versions `YY.MDD.N` (#373): the UTC committer date of the
tagged commit (month times 100 plus day, so January 5 is `105`) and the commit's
full-history count. Stable is `vYY.MDD.N` and nightly `vYY.MDD.N-nightly`.

Tags cut before #373, stable `v0.Y.Z`, candidate `v0.Y.Z-rc.N` and nightly
`v0.Y.Z-nightly.N`, stay readable: installed builds, published releases and the
package repositories still carry them. Nothing creates them any more.
"""
from dataclasses import dataclass
import datetime
import re

_CALENDAR = re.compile(r'(?P<base>(?P<year>[2-9][0-9])\.(?P<date>[1-9][0-9]{2,3})\.(?P<count>[1-9][0-9]*))'
                       r'(?P<nightly>-nightly)?')
_LEGACY_BASE = r'0\.(?:0|[1-9][0-9]*)\.(?:0|[1-9][0-9]*)'
_LEGACY = re.compile(r'(?P<base>' + _LEGACY_BASE + r')(?:-rc\.(?P<candidate>[1-9][0-9]*)'
                     r'|-nightly\.(?P<count>[1-9][0-9]*))?')
_RANK = {'nightly': 0, 'candidate': 1, 'stable': 2}
# 26 is the year the scheme started; earlier years never named a release.
FIRST_YEAR = 26


@dataclass(frozen=True)
class ReleaseIdentity:
    version: str
    base_version: str
    package_version: str
    channel: str
    count: int | None = None
    legacy: bool = False

    @property
    def prerelease(self):
        return self.channel != 'stable'

    @property
    def key(self):
        """Ordering of every release, old and new, as dpkg and rpm order the packages.

        Calendar versions sort by date, then commit count; on one YY.MDD.N a nightly
        sorts before the stable release. Every calendar version sorts after every
        pre-#373 one, whose major version is 0.
        """
        numbers = tuple(int(part) for part in self.base_version.split('.'))
        return numbers + (_RANK[self.channel], 0 if not self.legacy else self.count or 0)


def calendar_base(timestamp, count):
    """`YY.MDD.N` for a commit's committer timestamp (epoch seconds) and commit count."""
    if isinstance(timestamp, bool) or not isinstance(timestamp, int) or isinstance(count, bool) \
            or not isinstance(count, int) or count < 1:
        raise ValueError('A committer timestamp and a positive commit count are required')
    date = datetime.datetime.fromtimestamp(timestamp, datetime.timezone.utc)
    if not FIRST_YEAR <= date.year - 2000 <= 99:
        raise ValueError('The commit date is outside the calendar version years')
    return f'{date.year - 2000}.{date.month * 100 + date.day}.{count}'


def _calendar(match):
    year, date = int(match['year']), int(match['date'])
    try:
        datetime.date(2000 + year, date // 100, date % 100)
    except ValueError:
        return None
    if year < FIRST_YEAR:
        return None
    base, count = match['base'], int(match['count'])
    if match['nightly']:
        return ReleaseIdentity(match.string, base, base + '~nightly', 'nightly', count)
    return ReleaseIdentity(match.string, base, base, 'stable', count)


def parse_version(version):
    match = _CALENDAR.fullmatch(version) if isinstance(version, str) else None
    identity = _calendar(match) if match else None
    if identity:
        return identity
    match = _LEGACY.fullmatch(version) if isinstance(version, str) else None
    if not match:
        raise ValueError('Exact stable or nightly release version required')
    base, candidate, count = match.group('base', 'candidate', 'count')
    if count:
        # Packages order ~nightly before ~rc and both before the stable base.
        return ReleaseIdentity(version, base, f'{base}~nightly.{count}', 'nightly', int(count), True)
    if candidate:
        return ReleaseIdentity(version, base, base + '~rc.' + candidate, 'candidate', int(candidate), True)
    return ReleaseIdentity(version, base, base, 'stable', None, True)


def parse_tag(tag):
    if not isinstance(tag, str) or not tag.startswith('v'):
        raise ValueError('Release tag must start with v')
    return parse_version(tag[1:])
