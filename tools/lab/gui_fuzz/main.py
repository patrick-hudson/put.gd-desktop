"""Run the walk: python3 -m gui_fuzz.main --seed N --minutes M --out DIR   (or --replay DIR/actions.jsonl)."""
import argparse
import json
import os
import random
import signal
import sys
import time

from gui_fuzz.actor import Actor
from gui_fuzz import desktop
from gui_fuzz.uinput import Devices
from gui_fuzz.walk import Walker
from gui_fuzz.watch import Findings, Watchdog
from gui_fuzz.world import Log, World


def replay(path, actor):
    for line in open(path):
        record = json.loads(line)
        op = record.get('op')
        if op == 'sleep':
            time.sleep(record['s'])
        elif op == 'move':
            actor.move(record['x'], record['y'], drag=record.get('drag', False))
        elif op == 'btn':
            actor.button(record['b'], record['down'])
        elif op == 'wheel':
            actor.wheel(record['n'], record['h'])
        elif op == 'chord':
            actor.chord(record['k'])
        elif op == 'text':
            actor.text(record['v'])
        elif op == 'release_all':
            actor.release_all()
        elif op == 'cli':
            actor.cli(record['m'], record['p'], timeout=55)
        elif op == 'sh':
            actor.sh(*record['a'])


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--seed', type=int, default=1)
    parser.add_argument('--minutes', type=float, default=10)
    parser.add_argument('--max-steps', type=int, default=0)
    parser.add_argument('--out', required=True)
    parser.add_argument('--replay')
    parser.add_argument('--desktop', choices=sorted(desktop.DESKTOPS), help='default: from XDG_CURRENT_DESKTOP')
    parser.add_argument('--chaos', action='store_true', help='favour monitor hotplug and rapid repeats (default for odd seeds)')
    parser.add_argument('--calm', action='store_true', help='never chaos, whatever the seed')
    parser.add_argument('--keep-going', action='store_true', help='continue after a fatal finding (default: stop the walk)')
    args = parser.parse_args()
    os.makedirs(args.out, exist_ok=True)
    rng = random.Random(args.seed)
    desk = desktop.make(args.desktop)
    signal.signal(signal.SIGTERM, lambda *_: (_ for _ in ()).throw(KeyboardInterrupt()))  # an interruption ends the run the same way Ctrl+C does
    desk.prepare()
    fence = desk.make_fence()
    try:
        fence.start()
    except Exception:
        desk.finish()
        raise
    dev = Devices()
    desk.flatten_acceleration(dev)
    log = Log(os.path.join(args.out, 'actions.jsonl'), args.seed)
    world = World(fence, desk)
    desk.begin(world)
    findings = Findings(args.out, log, world, args.seed)
    watch = Watchdog(args.out, world, findings, log)
    watch.prime()
    watch.start()
    actor = Actor(dev, world, log, rng, replaying=bool(args.replay))
    walker = Walker(actor, world, watch, findings, rng, log, chaos=args.chaos or (args.seed % 2 == 1 and not args.calm))
    started = time.time()
    code = 0
    paused_since = None
    try:
        if args.replay:
            replay(args.replay, actor)
        else:
            while time.time() - started < args.minutes * 60 and not (args.max_steps and log.step >= args.max_steps):
                if findings.fatal.is_set() and not args.keep_going:
                    code = 3
                    break
                if watch.paused_walk.is_set():
                    paused_since = paused_since or time.time()
                    if time.time() - paused_since > 5:
                        walker.recover()
                        paused_since = time.time()
                    time.sleep(0.5)
                    continue
                paused_since = None
                walker.step()
    except KeyboardInterrupt:
        pass
    finally:
        try:
            actor.release_all()
        except OSError:
            pass
        watch.stop.set()
        summary = {'desktop': desk.name, 'seed': args.seed, 'seconds': round(time.time() - started), 'steps': log.step, 'actions': walker.counts, 'denied': actor.denied,
                   'harness_errors': walker.harness_errors, 'findings': {'%s|%s' % k: v for k, v in findings.seen.items()},
                   'pings': watch.pings, 'log_warnings': watch.warnings, 'shell_warnings': watch.shell_warnings, 'foreign_windows_closed': watch.foreign}
        with open(os.path.join(args.out, 'summary.json'), 'w') as stream:
            json.dump(summary, stream, indent=1)
        dev.close()
        fence.stop()
        desk.finish()
        print(json.dumps({k: summary[k] for k in ('desktop', 'seed', 'seconds', 'steps', 'denied', 'harness_errors', 'findings')}), flush=True)
    return code


if __name__ == '__main__':
    sys.exit(main())
