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


def replay(path, actor, desk):
    with open(path) as stream:
        records = [json.loads(line) for line in stream]
    for record in records:
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
            desk.replayed(record['a'])  # a replayed monitor change is one the run made: finish() puts the layout back


def stop_code(findings, args, log, started, now):
    """What the walk does next. A fatal finding stops it (exit 3) before the step and time limits are even looked at, so a run that reaches its
    time limit at the moment of a fatal finding still ends as failed. None means: take another step."""
    if findings.fatal.is_set() and not args.keep_going:
        return 3
    if now - started >= args.minutes * 60 or (args.max_steps and log.step >= args.max_steps):
        return 0
    return None


def exit_status(code, watch, findings, args):
    """Stop the watchdog, give it one last look at everything it watches, then decide."""
    watch.stop.set()
    if watch.is_alive():
        watch.join(3)
    watch.final_poll()
    return 3 if findings.fatal.is_set() and not args.keep_going else code


def blocked(step):
    """Run `step` with SIGTERM and SIGINT held back, then let any that arrived through (the handler runs after the step is done)."""
    held = {signal.SIGTERM, signal.SIGINT}
    before = signal.pthread_sigmask(signal.SIG_BLOCK, held)
    try:
        return step()
    finally:
        signal.pthread_sigmask(signal.SIG_SETMASK, before)


def quietly(step, what):
    try:
        step()
    except Exception as error:  # every cleanup step runs, whatever the one before it did
        print('cleanup: %s failed: %r' % (what, error), flush=True)


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
    fence = dev = watch = actor = walker = findings = log = None
    started = time.time()
    code = 0
    paused_since = None
    # Everything that changes the session is created inside this try, so whatever fails, and whenever the signal arrives, finish() runs.
    try:
        desk.prepare()
        fence = desk.make_fence()
        fence.start()
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
        if args.replay:
            replay(args.replay, actor, desk)
        else:
            while True:
                stop = stop_code(findings, args, log, started, time.time())
                if stop is not None:
                    code = stop
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
            if watch is not None:
                code = exit_status(code, watch, findings, args)
            if walker is not None:
                summary = {'desktop': desk.name, 'seed': args.seed, 'seconds': round(time.time() - started), 'steps': log.step, 'actions': walker.counts,
                           'denied': actor.denied, 'harness_errors': walker.harness_errors, 'findings': {'%s|%s' % k: v for k, v in findings.seen.items()},
                           'pings': watch.pings, 'log_warnings': watch.warnings, 'shell_warnings': watch.shell_warnings, 'foreign_windows_closed': watch.foreign}
                with open(os.path.join(args.out, 'summary.json'), 'w') as stream:
                    json.dump(summary, stream, indent=1)
                print(json.dumps({k: summary[k] for k in ('desktop', 'seed', 'seconds', 'steps', 'denied', 'harness_errors', 'findings')}), flush=True)
        except Exception as error:  # a failed summary must not skip the cleanup below
            print('summary failed: %r' % (error,), flush=True)
            code = code or 2
        finally:
            if actor is not None:
                quietly(actor.release_all, 'release input')
            if dev is not None:
                quietly(dev.close, 'destroy the virtual devices')
            if fence is not None:
                quietly(fence.stop, 'stop the fence')
            quietly(lambda: blocked(desk.finish), 'restore the session and take the witness down')  # no interruption may cut the teardown short
    return code


if __name__ == '__main__':
    try:
        sys.exit(main())
    except KeyboardInterrupt:  # an interruption that arrived while the teardown was protected: it is done now
        sys.exit(130)
