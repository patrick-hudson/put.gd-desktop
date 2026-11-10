#!/usr/bin/env python3
"""summarize_run.py RESULTS.json: one screen of what a record_run.py run measured (microphone, tones, A/V offsets, clicks)."""
import json
import sys


def main():
    """Prints the run's builds, then each round's audio, tones, A/V offsets and click measurement."""
    try:
        report = json.load(open(sys.argv[1]))
    except (OSError, ValueError) as error:
        print('no results:', error)
        return 1
    print('builds', {k: v.get('version') or v.get('file') for k, v in (report.get('builds') or {}).items()}, '| desktop', report.get('desktop'),
          report.get('session'), '| error', report.get('error'), '| microphone removed', (report.get('microphoneRemoved') or {}).get('left'))
    for entry in report.get('rounds', []):
        print(' %s: audio %s | sources at start %s | export %s | durationUs %s' % (
            entry.get('format'), entry.get('audio'), (entry.get('progressAtStart') or {}).get('audioSources'), entry.get('export'), entry.get('durationUs')))
        if entry.get('microphones'):
            print('   microphones', {k: entry['microphones'].get(k) for k in ('available', 'reason', 'defaultIsMonitor')}, 'device', (entry['microphones'].get('device') or {}).get('id'))
        if entry.get('problems'):
            print('   PROBLEMS: %s' % '; '.join(entry['problems']))
        for key in ('error', 'recordedS', 'microphoneLostAtStop', 'outputGoneAtS', 'outputGone', 'afterOutputGone', 'afterUnplug', 'unpluggedAtS', 'captureStepMs', 'exportMinusWorkingMs', 'clickHighlightsError'):
            if entry.get(key) is not None:
                print('   %s %s' % (key, entry[key]))
        for step in entry.get('readyFollow') or []:
            m = step.get('microphone') or {}
            print('   ready %-8s available %-5s effective %-5s requested %-5s %s%s' % (step.get('step'), m.get('available'), m.get('effective'), m.get('requested'),
                                                                                    m.get('device') or m.get('unavailable') or '', '' if step.get('waitedS') is None else ' | after %.1f s' % step['waitedS']))
        if entry.get('clickHighlights'):
            print('   clickHighlights', {k: entry['clickHighlights'].get(k) for k in ('available', 'route', 'drawnBy', 'targets', 'reason')})
        for kind, clip in (entry.get('clips') or {}).items():
            tones = {hz: {k: round(v.get(k) or 0, 4) for k in ('medianAmplitude', 'maxAmplitude', 'activeFraction')} for hz, v in (clip.get('tones') or {}).items() if isinstance(v, dict)}
            print('   %-7s motion %s | pairs %s median %s ms spread %s | tones %s%s' % (kind, clip.get('motion'), clip.get('pairs'), clip.get('medianMs'), clip.get('spreadMs'), tones,
                                                                                     ' | ' + str(clip.get('error')) if clip.get('error') else ''))
            check = clip.get('clickCheck')
            if check:
                print('           clicks ok %s seen %s/%s startCallMs %s problems %s error %s frame %s' % (check.get('ok'), check.get('seen'), check.get('expected'), check.get('startCallMs'),
                                                                                                  check.get('problems'), check.get('error'), check.get('frame')))
                for pointer, ring in zip(check.get('pointer') or [], check.get('rings') or []):
                    print('             pointer at', pointer.get('firstMs'), '| ring', {k: ring.get(k) for k in ('seen', 'firstMs', 'delayMs', 'lengthMs', 'peak', 'dx', 'dy', 'rgb', 'reason')})
    return 0


if __name__ == '__main__':
    sys.exit(main())
