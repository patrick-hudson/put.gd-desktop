#!/usr/bin/env python3
"""Markdown summary of one Renovate run from its JSON log (.github/workflows/renovate.yml).

Lists every dependency Renovate detected, the update it found for each, and the
branches it planned, plus its warnings and errors.
"""
import argparse
import json
from pathlib import Path

LEVELS = {40: 'warning', 50: 'error', 60: 'fatal'}


def records(path):
    for line in path.read_text(errors='replace').splitlines():
        line = line.strip()
        if line.startswith('{'):
            try:
                yield json.loads(line)
            except json.JSONDecodeError:
                continue


def package_files(entries):
    """The last dependency list, {manager: [{packageFile, deps}]}: with updates after lookups,
    without them after an extract-only run."""
    found = None
    for entry in entries:
        if entry.get('msg') in ('packageFiles with updates', 'Extracted dependencies'):
            found = next((value for value in entry.values() if isinstance(value, dict)
                          and all(isinstance(files, list) for files in value.values())), found)
    return found or {}


def dependency_rows(files):
    rows = []
    for manager, items in sorted(files.items()):
        for item in items:
            for dep in item.get('deps', []):
                updates = [update for update in dep.get('updates', []) if update.get('updateType') != 'rollback']
                proposed = ', '.join(f"{update.get('newValue') or update.get('newDigest', '')[:12]} ({update.get('updateType', '?')}"
                                     + (', needs approval' if update.get('dependencyDashboardApproval') else '') + ')'
                                     for update in updates)
                status = dep.get('skipReason') and 'skipped: ' + dep['skipReason']
                rows.append({'manager': manager, 'file': item.get('packageFile', ''), 'dependency': dep.get('depName') or dep.get('packageName', ''),
                             'datasource': dep.get('datasource', ''), 'current': dep.get('currentValue') or dep.get('currentDigest', ''),
                             'proposed': proposed or status or ('up to date' if 'updates' in dep else 'not looked up')})
    return rows


def markdown(entries):
    entries = list(entries)
    rows = dependency_rows(package_files(entries))
    lines = ['### Renovate', '']
    if rows:
        pending = sum(1 for row in rows if row['proposed'] not in ('up to date', 'not looked up') and not row['proposed'].startswith('skipped'))
        lines += [f'{len(rows)} dependencies detected; {pending} with an update.', '',
                  '| Manager | File | Dependency | Datasource | Current | Update |', '| --- | --- | --- | --- | --- | --- |']
        lines += [f"| {row['manager']} | `{row['file']}` | {row['dependency']} | {row['datasource']} | {row['current']} | {row['proposed']} |"
                  for row in rows]
    else:
        lines.append('No dependency list in the log.')
    branches = [entry for entry in entries if entry.get('msg') in ('branches info extended', 'Branch summary')]
    for entry in branches[-1:]:
        listed = entry.get('branchesInformation') or entry.get('branches') or []
        if listed:
            lines += ['', '| Branch | Group or dependency | Result |', '| --- | --- | --- |']
            for branch in listed:
                name = branch.get('branchName', '')
                upgrades = branch.get('upgrades') or []
                what = branch.get('groupName') or ', '.join(sorted({upgrade.get('depName', '') for upgrade in upgrades}))
                lines.append(f"| `{name}` | {what} | {branch.get('result') or branch.get('prNo') or ''} |")
    problems = [entry for entry in entries if entry.get('level', 0) >= 40]
    if problems:
        lines += ['', f'{len(problems)} warnings or errors:', '']
        lines += [f"- {LEVELS.get(entry['level'], entry['level'])}: {entry.get('msg', '')}" for entry in problems[:50]]
    return '\n'.join(lines) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', type=Path, required=True)
    parser.add_argument('--summary', type=Path)
    args = parser.parse_args()
    if not args.log.is_file():
        print('No Renovate log at ' + str(args.log))
        return
    text = markdown(records(args.log))
    print(text)
    if args.summary:
        with open(args.summary, 'a') as stream:
            stream.write(text)


if __name__ == '__main__':
    main()
