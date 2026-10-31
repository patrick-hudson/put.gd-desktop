#!/usr/bin/env python3
"""Publish only the fields needed to locate a Gitleaks finding.

The raw report must stay outside the artifact directory. --redact does not
redact commit messages, author metadata or arbitrary SARIF properties.
"""
import argparse
import json
from pathlib import Path
import re


def objects(value):
    if not isinstance(value, list) or any(not isinstance(item, dict) for item in value):
        raise ValueError('Expected an array of SARIF objects')
    return value


def sanitize(report):
    if not isinstance(report, dict) or report.get('version') != '2.1.0':
        raise ValueError('Expected a SARIF 2.1.0 report')
    runs = []
    for run in objects(report.get('runs')):
        results = []
        for finding in objects(run.get('results')):
            if not isinstance(finding.get('ruleId'), str) or not finding['ruleId']:
                raise ValueError('Expected a SARIF rule ID')
            result = {
                'ruleId': finding['ruleId'],
                'message': {'text': 'Potential secret detected; see rule and location.'},
                'locations': [],
            }
            for location in objects(finding.get('locations')):
                physical = location['physicalLocation']
                if (not isinstance(physical, dict)
                        or not isinstance(physical.get('artifactLocation'), dict)
                        or not isinstance(physical['artifactLocation'].get('uri'), str)
                        or not isinstance(physical.get('region'), dict)):
                    raise ValueError('Invalid SARIF physical location')
                region = {key: physical['region'][key] for key in
                          ('startLine', 'endLine', 'startColumn', 'endColumn')
                          if key in physical['region']}
                if ('startLine' not in region
                        or any(type(value) is not int or value < 1 for value in region.values())):
                    raise ValueError('Invalid SARIF region coordinates')
                result['locations'].append({'physicalLocation': {
                    'artifactLocation': {'uri': physical['artifactLocation']['uri']},
                    'region': region,
                }})
            fingerprints = finding.get('partialFingerprints', {})
            if not isinstance(fingerprints, dict):
                raise ValueError('Invalid SARIF fingerprints')
            commit = fingerprints.get('commitSha', '')
            if not isinstance(commit, str):
                raise ValueError('Invalid SARIF commit SHA')
            if re.fullmatch(r'(?:[0-9a-f]{40}|[0-9a-f]{64})', commit):
                result['partialFingerprints'] = {'commitSha': commit}
            results.append(result)
        runs.append({'tool': {'driver': {'name': 'gitleaks'}}, 'results': results})
    return {'$schema': 'https://json.schemastore.org/sarif-2.1.0.json',
            'version': '2.1.0', 'runs': runs}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('raw', type=Path)
    parser.add_argument('output', type=Path)
    args = parser.parse_args()
    # No output is created if parsing or projection fails. Do not print raw
    # JSON/exception details: malformed input can contain unredacted metadata.
    try:
        report = sanitize(json.loads(args.raw.read_text()))
    except (OSError, ValueError, KeyError, TypeError):
        raise SystemExit('Cannot sanitize Gitleaks report; no report published.') from None
    args.output.write_text(json.dumps(report, indent=2) + '\n')


if __name__ == '__main__':
    main()
