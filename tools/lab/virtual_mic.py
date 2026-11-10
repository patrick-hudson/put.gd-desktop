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

`create` checks the microphone before it hands it over: a second of tone goes
into the sink and must come out of the source, and a loopback into a third,
silent sink then keeps the source in use for the whole run. PipeWire 1.0.5
(Mint 22, Ubuntu 24.04) needs both: there a remapped source that was left
without a reader sometimes passes pure silence to every later reader, while
the sink's monitor carries the sound (measured on m74: 12 of 21 recordings
silent, an independent `parec` on the source silent with them; 10 of 10
carry the sound since). A source that
fails the check is made again, up to six times; the state says how many it
took (`attempts`) and what the last check heard (`heardPeak`).

`create --system-sink` also adds a second null sink and makes it the default
output, so "system audio" played during the run goes nowhere audible and is
what put.gd's system-audio capture records; `--monitor-default` makes that
output's monitor the default input, the state a desktop without a microphone
is in; `remove` puts the previous defaults back by name. Works with PipeWire's PulseAudio layer (1.0.5 and later) and
PulseAudio; needs `pactl`, `gst-launch-1.0` for `tone` and the check, and `parec` for the check
(without it the microphone is handed over unchecked and the state says `heardPeak: null`).
"""
import argparse
import array
import json
import os
import signal
import subprocess
import sys
import time


def sound_env():
    env = dict(os.environ)
    env.setdefault('XDG_RUNTIME_DIR', '/run/user/%d' % os.getuid())
    return env


def pactl(*arguments, check=True):
    result = subprocess.run(['pactl', *arguments], capture_output=True, text=True, env=sound_env())
    if check and result.returncode != 0:
        raise SystemExit('pactl %s failed: %s' % (' '.join(arguments), result.stderr.strip()))
    return result.stdout.strip()


def heard_peak(sink, source):
    """The largest sample read from the source while a tone plays into the sink (16-bit scale), or None without `parec`."""
    env = sound_env()
    # Three seconds of tone into the null sink: nothing audible. Two seconds are read back, from half a second in,
    # so a slow start of either process (a software-rendered guest) still overlaps.
    player = subprocess.Popen(['gst-launch-1.0', '-q', 'audiotestsrc', 'freq=1000', 'volume=0.2', 'num-buffers=130', '!', 'audioconvert', '!',
                               'pulsesink', 'device=' + sink], env=env, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        time.sleep(0.5)
        try:
            heard = subprocess.run(['parec', '-d', source, '--format=s16le', '--rate=48000', '--channels=1', '--latency-msec=50'], env=env,
                                   capture_output=True, timeout=2.0).stdout
        except subprocess.TimeoutExpired as expired:
            heard = expired.stdout or b''
        except FileNotFoundError:
            return None
    finally:
        player.terminate()
        player.wait(10)
    samples = array.array('h')
    samples.frombytes(heard[:len(heard) // 2 * 2])
    return max((abs(value) for value in samples), default=0)


def load_microphone(state, sink, source, hold):
    """The remapped source and the loopback that keeps it in use."""
    state['modules']['source'] = int(pactl('load-module', 'module-remap-source', 'master=' + sink + '.monitor',
                                           'source_name=' + source, 'source_properties=device.description=' + state['name'] + '-microphone'))
    state['modules']['hold'] = int(pactl('load-module', 'module-loopback', 'source=' + source, 'sink=' + hold, 'latency_msec=100'))


def create(args):
    if args.monitor_default and not args.system_sink:
        raise SystemExit('--monitor-default needs --system-sink')
    state = {'name': args.name, 'modules': {}, 'defaultSource': pactl('get-default-source', check=False),
             'defaultSink': pactl('get-default-sink', check=False)}
    sink, source, hold = args.name + '_sink', args.name + '_mic', args.name + '_hold'
    try:
        state['modules']['sink'] = int(pactl('load-module', 'module-null-sink', 'sink_name=' + sink,
                                             'sink_properties=device.description=' + args.name + '-sink'))
        state['modules']['holdSink'] = int(pactl('load-module', 'module-null-sink', 'sink_name=' + hold,
                                                 'sink_properties=device.description=' + args.name + '-hold'))
        for attempt in range(1, 7):
            load_microphone(state, sink, source, hold)
            state.update(attempts=attempt, heardPeak=heard_peak(sink, source))
            # The check's tone is at 0.2 of full scale, about 6500 here.
            if state['heardPeak'] is None or state['heardPeak'] > 500:
                break
            remove_modules(state, only=('hold', 'source'))
        else:
            raise SystemExit('the simulated microphone passes no sound on this sound server (6 tries)')
        state.update(sink=sink, source=source)
        if args.system_sink:
            system = args.name + '_system'
            state['modules']['system'] = int(pactl('load-module', 'module-null-sink', 'sink_name=' + system,
                                                   'sink_properties=device.description=' + args.name + '-system'))
            # Noted before each change, so a failure half way still puts the default back.
            state['systemSink'] = system
            pactl('set-default-sink', system)
        if args.default:
            state['madeDefault'] = True
            pactl('set-default-source', source)
        if args.monitor_default:
            # The state a desktop without a microphone is in: the default input is an output's monitor.
            state['madeDefault'] = True
            pactl('set-default-source', state['systemSink'] + '.monitor')
    except BaseException:
        # Whatever was changed before the failure is put back: the defaults by name, the modules by id.
        restore_defaults(state)
        remove_modules(state)
        raise
    write(args.state, state)
    print(json.dumps(state))


def remove_modules(state, only=None):
    # The loopback goes before its source: left behind, the sound server would move it to the default
    # input, and a real microphone is never opened in the lab.
    for key in ('hold', 'source', 'holdSink', 'system', 'sink'):
        ident = state['modules'].get(key)
        if ident is None or (only and key not in only):
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
    remove_modules(state, only=('hold', 'source'))
    write(args.state, state)
    print(json.dumps({'unplugged': state.get('source')}))


def tone(args):
    state = load(args.state)
    sink = state.get('systemSink') if args.to == 'system' else state.get('sink')
    if not sink:
        raise SystemExit('no %s sink in this state; create it with --system-sink' % args.to)
    buffers = int(args.seconds * 44100 / 1024)
    player = subprocess.Popen(['gst-launch-1.0', '-q', 'audiotestsrc', 'freq=%g' % args.hz, 'volume=%g' % args.volume,
                               'num-buffers=%d' % buffers, '!', 'audioconvert', '!', 'pulsesink', 'device=' + sink])
    # The player ends with this command: left alone it would outlive its sink, and the sound server
    # would move it to the real default output.
    for number in (signal.SIGTERM, signal.SIGINT, signal.SIGHUP):
        signal.signal(number, lambda *_: player.terminate())
    return player.wait()


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
