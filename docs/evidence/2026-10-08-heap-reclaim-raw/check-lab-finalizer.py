#!/usr/bin/env python3
"""Fault-inject only the real script's finalizer; no VM or desktop interaction."""
import ast, contextlib, copy, hashlib, io, json, pathlib, sys, time, types
source = pathlib.Path('tests/performance/bench_payload_lab.py')
tree = ast.parse(source.read_text(), filename=str(source))
finalizers = [node.finalbody for node in tree.body if isinstance(node, ast.Try) and node.finalbody]
assert len(finalizers) == 1
code = compile(ast.fix_missing_locations(ast.Module(body=finalizers[0], type_ignores=[])), str(source), 'exec')
class InputError(Exception): pass
class RestoreError(Exception): pass
cases = []
for fault in ('none', 'input', 'restore', 'both', 'not-started'):
    calls, saved = [], []
    results = {'ok': True, 'samples': [], 'cycles': []}
    def input_events(*args):
        calls.append('input')
        if fault in ('input', 'both'): raise InputError('synthetic input fault')
    def guest_session(*args):
        calls.append('restore')
        if fault in ('restore', 'both'): raise RestoreError('synthetic restoration fault')
    lab = types.SimpleNamespace(input_events=input_events, guest_session=guest_session,
        cli=lambda *a: {'metadata': {'acceptedRevision': 'original'}}, LabError=RestoreError)
    ns = dict(started=fault != 'not-started', lab=lab, RUN=pathlib.Path('/synthetic/run'),
        GUEST='/home/lab/putgd-lab/synthetic', original_revision='original', results=results,
        time=time, json=json, save=lambda n, v: saved.append(copy.deepcopy(v)))
    error = None
    with contextlib.redirect_stdout(io.StringIO()):
        try: exec(code, ns)
        except Exception as exc: error = type(exc).__name__
    expected_error = 'RestoreError' if fault in ('restore', 'both') else 'InputError' if fault == 'input' else None
    checks = {'restorationAttempted': 'restore' in calls if fault != 'not-started' else not calls,
        'savedOnce': len(saved) == 1, 'errorPropagated': error == expected_error,
        'failedCleanupNotSuccess': results['ok'] is False if fault in ('input', 'restore', 'both') else results['ok'],
        'restorationStatus': results.get('restoredConfig') is True if fault in ('none', 'input') else results.get('restoredConfig') is not True}
    cases.append({'fault': fault, 'calls': calls, 'errorType': error, 'checks': checks, 'passed': all(checks.values())})
output = {'scope': 'Fault injection of actual script finalizer AST only; not VM acceptance',
          'sourceSha256': hashlib.sha256(source.read_bytes()).hexdigest(), 'cases': cases,
          'passed': all(c['passed'] for c in cases)}
pathlib.Path(sys.argv[1]).write_text(json.dumps(output, indent=2)+'\n')
print(json.dumps(output, indent=2))
raise SystemExit(0 if output['passed'] else 1)
