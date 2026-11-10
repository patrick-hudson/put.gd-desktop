#!/usr/bin/env python3
"""A simulated microphone for recording runs (stdlib only): a null sink whose monitor is remapped into a source.

No real microphone is ever recorded in the lab. Sound played into the sink
comes out of the source, so a tone or the `--av-marker` clip's bursts stand in
for a voice. The source is a real input to the sound server (not a monitor),
so put.gd lists and opens it like any microphone. The modules are removed by
the exact ids `load-module` printed, never by name or pattern.

  virtual_mic.py create [--name putgd_lab] [--state FILE]   prints {sink, source, modules, defaultSource, ...}
  virtual_mic.py remove --state FILE                        unloads those modules and restores the defaults
  virtual_mic.py unplug --state FILE                        unloads only the source (a microphone pulled mid-recording)
  virtual_mic.py tone --state FILE [--hz 440] [--volume 0.012] [--seconds 60] [--to system|mic]

`create --system-sink` also adds a second null sink and makes it the default
output, so "system audio" played during the run goes nowhere audible and is
what put.gd's system-audio capture records; `--monitor-default` makes that
output's monitor the default input, the state a desktop without a microphone
is in; `remove` puts the previous defaults back by name. Works with PipeWire's PulseAudio layer (1.0.5 and later) and
PulseAudio; needs `pactl`, and `gst-launch-1.0` for `tone`.
"""
import argparse
import json
import os
import subprocess
import sys


def pactl(*arguments, check=True):
    env = dict(os.environ)
    env.setdefault('XDG_RUNTIME_DIR', '/run/user/%d' % os.getuid())
    result = subprocess.run(['pactl', *arguments], capture_output=True, text=True, env=env)
    if check and result.returncode != 0:
        raise SystemExit('pactl %s failed: %s' % (' '.join(arguments), result.stderr.strip()))
    return result.stdout.strip()


def create(args):
    state = {'name': args.name, 'modules': {}, 'defaultSource': pactl('get-default-source', check=False),
             'defaultSink': pactl('get-default-sink', check=False)}
    sink, source = args.name + '_sink', args.name + '_mic'
    try:
        state['modules']['sink'] = int(pactl('load-module', 'module-null-sink', 'sink_name=' + sink,
                                             'sink_properties=device.description=' + args.name + '-sink'))
        state['modules']['source'] = int(pactl('load-module', 'module-remap-source', 'master=' + sink + '.monitor',
                                               'source_name=' + source, 'source_properties=device.description=' + args.name + '-microphone'))
        state.update(sink=sink, source=source)
        if args.system_sink:
            system = args.name + '_system'
            state['modules']['system'] = int(pactl('load-module', 'module-null-sink', 'sink_name=' + system,
                                                   'sink_properties=device.description=' + args.name + '-system'))
            pactl('set-default-sink', system)
            state['systemSink'] = system
        if args.default:
            pactl('set-default-source', source)
            state['madeDefault'] = True
        if args.monitor_default:
            if not args.system_sink:
                raise SystemExit('--monitor-default needs --system-sink')
            # The state a desktop without a microphone is in: the default input is an output's monitor.
            pactl('set-default-source', state['systemSink'] + '.monitor')
            state['madeDefault'] = True
    except BaseException:
        remove_modules(state)
        raise
    write(args.state, state)
    print(json.dumps(state))


def remove_modules(state, only=None):
    for key in ('source', 'system', 'sink'):
        ident = state['modules'].get(key)
        if ident is None or (only and key != only):
            continue
        pactl('unload-module', str(ident), check=False)
        state['modules'][key] = None


def restore_defaults(state):
    if state.get('systemSink') and state.get('defaultSink'):
        pactl('set-default-sink', state['defaultSink'], check=False)
    if state.get('madeDefault') and state.get('defaultSource'):
        pactl('set-default-source', state['defaultSource'], check=False)


def write(path, state):
    if path:
        with open(path, 'w') as stream:
            json.dump(state, stream)
            stream.write('\n')


def load(path):
    with open(path) as stream:
        return json.load(stream)


def remove(args):
    state = load(args.state)
    restore_defaults(state)
    remove_modules(state)
    left = [line.split('\t')[1] for line in (pactl('list', 'short', 'sources', check=False) + '\n' + pactl('list', 'short', 'sinks', check=False)).splitlines()
            if '\t' in line and line.split('\t')[1].startswith(state['name'] + '_')]
    write(args.state, state)
    print(json.dumps({'removed': True, 'left': left, 'defaultSource': pactl('get-default-source', check=False),
                      'defaultSink': pactl('get-default-sink', check=False)}))
    return 1 if left else 0


def unplug(args):
    state = load(args.state)
    remove_modules(state, only='source')
    write(args.state, state)
    print(json.dumps({'unplugged': state.get('source')}))


def tone(args):
    state = load(args.state)
    sink = state.get('systemSink') if args.to == 'system' else state.get('sink')
    if not sink:
        raise SystemExit('no %s sink in this state; create it with --system-sink' % args.to)
    buffers = int(args.seconds * 44100 / 1024)
    return subprocess.call(['gst-launch-1.0', '-q', 'audiotestsrc', 'freq=%g' % args.hz, 'volume=%g' % args.volume,
                            'num-buffers=%d' % buffers, '!', 'audioconvert', '!', 'pulsesink', 'device=' + sink])


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sub = parser.add_subparsers(dest='command', required=True)
    c = sub.add_parser('create')
    c.add_argument('--name', default='putgd_lab')
    c.add_argument('--state')
    c.add_argument('--system-sink', action='store_true', help='also a private default output for system audio')
    c.add_argument('--default', action='store_true', help='make the simulated microphone the default input for the run')
    c.add_argument('--monitor-default', action='store_true', help="make the private output's monitor the default input (with --system-sink)")
    for name in ('remove', 'unplug', 'tone'):
        child = sub.add_parser(name)
        child.add_argument('--state', required=True)
        if name == 'tone':
            child.add_argument('--hz', type=float, default=440)
            child.add_argument('--volume', type=float, default=0.012,
                               help='quiet enough that the marker detector still sees the bursts (under a tenth of their level)')
            child.add_argument('--seconds', type=float, default=60)
            child.add_argument('--to', choices=('system', 'mic'), default='system')
    args = parser.parse_args(argv)
    return {'create': create, 'remove': remove, 'unplug': unplug, 'tone': tone}[args.command](args) or 0


if __name__ == '__main__':
    sys.exit(main())
