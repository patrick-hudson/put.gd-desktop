"""Live signal-safety trials on a boat Cinnamon guest (#815, #816).

usage: live.py SRC RUN OUT input|browser

input: 20 real QMP gestures (7 clicks, 7 held-Shift drags aimed at the pointer
release, 6 aimed at the held-key release), each with run.py's SIGALRM deadline
handler and a one-shot alarm landing 0-150 us after the step before the
release. After each: xinput query-state for every pointer and keyboard.

browser: 5 default_browser transactions with run.py's SIGTERM handler and a
SIGTERM aimed 0-150 us after the body ends; the real mimeapps.list files are
fingerprinted (bytes, mode) before and after each.
"""
import ctypes, hashlib, json, signal, sys, time, types, uuid
from pathlib import Path

SRC, RUN, OUT, MODE = Path(sys.argv[1]), Path(sys.argv[2]).resolve(), Path(sys.argv[3]), sys.argv[4]
sys.path.insert(0, str(SRC))
ctypes.CDLL(None).prctl(29, 1, 0, 0, 0)
from tools.lab import lab
from tools.lab.surface_driver import browser, context as context_module
from tools.lab.surface_driver.api import RecipeBlocked, RecipeFailed
from tools.lab.surface_driver.context import DriverContext
from tools.lab.surface_driver.profiles import load_profile
from tools.lab.surface_driver.run import verify_guest_endpoint
from tools.lab.surface_driver.target import Target

OUT.mkdir(parents=True, exist_ok=True)
root = '/tmp/sd-sig815-' + uuid.uuid4().hex[:6]
target = Target(run=RUN, session_root=root)
info = lab.verify_owned_guest(RUN)
command = (Path('/proc') / str(info['pid']) / 'cmdline').read_bytes()
target._verify_endpoints = lambda: verify_guest_endpoint(target, info['pid'], command)
profile = load_profile('mint-cinnamon-x11')
links = SRC / 'tools/lab/surface_driver/profiles/mint-cinnamon-x11.d/links.json'
if 'browser' not in profile and links.exists():
    profile = dict(profile, **json.loads(links.read_text()))
env = dict(desktop='mint-cinnamon-x11', host='boat', placement='guest', guest='cinnamon-x11', package='deb',
           destination='local')
print(lab.guest_session(RUN, 'mkdir -m 700 ' + root + ' && for d in runtime config data cache state; do mkdir -m 700 '
                        + root + '/$d; done; echo private root ready', timeout=60).strip())


def guest(script, timeout=120):
    return lab.guest_session(RUN, 'set +e\n' + script + '\ntrue\n', timeout=timeout)


def input_state():
    script = '''export DISPLAY=:0
for d in "QEMU QEMU USB Tablet" "Virtual core pointer" "Virtual core XTEST pointer"; do
 echo "== $d"; xinput query-state "$d" | grep -E "button\\[[1-5]\\]"; done
for d in "Virtual core keyboard" "AT Translated Set 2 keyboard" "Virtual core XTEST keyboard"; do
 echo "== $d"; xinput query-state "$d" | grep -E "key\\[(50|62|37|105|64)\\]"; done
'''
    text = guest(script)
    down = [line.strip() for line in text.splitlines() if '=down' in line]
    return text, down


def fresh_ctx():
    ctx = DriverContext(target, env, profile, OUT / 'catalog', OUT)
    ctx._deadline = time.monotonic() + 600
    ctx._guard = lambda *a, **k: None  # No app window here: input safety is what is under test.
    return ctx


def deadline(_signum, _frame):
    raise RecipeFailed('recipe deadline exceeded: live')


def terminate(signum, _frame):
    interrupted.append(signum)
    raise RecipeFailed('runner interrupted by ' + signal.Signals(signum).name)


interrupted = []
results = []
if MODE == 'input':
    aim = {'offset': None, 'skip': 0}
    real_sleep = time.sleep

    def sleep_then_arm(seconds):
        real_sleep(seconds)
        if aim['offset'] is not None and seconds in (.04, .025):
            if aim['skip']:
                aim['skip'] -= 1  # Earlier drag steps: aim after the last one.
                return
            signal.setitimer(signal.ITIMER_REAL, aim['offset'])  # Lands as the release starts.
            aim['offset'] = None
    fast = types.SimpleNamespace(**{k: getattr(time, k) for k in dir(time) if not k.startswith('_')})
    fast.sleep = sleep_then_arm
    context_module.time = fast
    plan = [('click', i) for i in range(7)] + [('drag-pointer', i) for i in range(7)] + [('drag-keys', i) for i in range(6)]
    for number, (kind, i) in enumerate(plan):
        offset = 150e-6 * i / 6
        signal.signal(signal.SIGALRM, deadline)
        ctx = fresh_ctx()
        raised = None
        try:
            if kind == 'click':
                aim['offset'] = max(1e-6, offset)
                ctx._pointer_click(900 + 20 * i, 500, 'left', 0, guarded=True)
            else:
                with ctx.held_keys('Shift'):
                    if kind == 'drag-pointer':
                        aim['offset'], aim['skip'] = max(1e-6, offset), 2
                    ctx.drag((700, 300 + 30 * i), (760, 340 + 30 * i), steps=3)
                    if kind == 'drag-keys':
                        signal.setitimer(signal.ITIMER_REAL, max(1e-6, offset))  # Lands as the key release starts.
            end = time.perf_counter() + 300e-6
            while time.perf_counter() < end:
                pass
        except RecipeFailed as error:
            raised = str(error)
        signal.setitimer(signal.ITIMER_REAL, 0)
        refused = None
        del ctx._guard
        try:
            ctx._guard()
        except RecipeBlocked as error:
            refused = str(error)
        except Exception as error:
            refused = None
        text, down = input_state()
        row = {'trial': number + 1, 'kind': kind, 'offset_us': round(offset * 1e6, 1), 'raised': raised,
               'poisoned': ctx._input_poisoned, 'press_open': ctx._press_open,
               'later_input_refused': bool(refused and 'release was not confirmed' in refused),
               'reviews': ctx.reviews, 'xinput_down': down,
               'mask_clear': signal.SIGALRM not in signal.pthread_sigmask(signal.SIG_BLOCK, ()),
               'handler_restored': signal.getsignal(signal.SIGALRM) is deadline}
        row['ok'] = bool(row['raised'] and row['mask_clear'] and row['handler_restored'] and
                         (not down or row['later_input_refused']))
        results.append(row)
        (OUT / ('input-%02d.txt' % (number + 1))).write_text(text)
        print(json.dumps(row), flush=True)
else:
    fingerprint = r'''for f in ~/.config/mimeapps.list ~/.local/share/applications/mimeapps.list ~/.config/cinnamon-mimeapps.list ~/.config/x-cinnamon-mimeapps.list; do
 if test -e "$f"; then echo "$f $(stat -c %a "$f") $(sha256sum < "$f" | cut -c1-64)"; else echo "$f absent"; fi; done
ls -d ~/.mozilla/firefox/putgd-surface-* ~/.local/share/applications/putgd-surface-* 2>/dev/null | sed 's/^/leftover /'
'''
    libc = ctypes.CDLL(None)
    class SigEvent(ctypes.Structure):
        _fields_ = [('value', ctypes.c_void_p), ('signo', ctypes.c_int), ('notify', ctypes.c_int), ('pad', ctypes.c_int * 12)]
    class TS(ctypes.Structure):
        _fields_ = [('s', ctypes.c_long), ('ns', ctypes.c_long)]
    class ITS(ctypes.Structure):
        _fields_ = [('interval', TS), ('value', TS)]
    timer = ctypes.c_void_p()
    assert libc.timer_create(1, ctypes.byref(SigEvent(None, signal.SIGTERM, 0)), ctypes.byref(timer)) == 0
    def arm(seconds):
        ns = max(1, int(seconds * 1e9))
        libc.timer_settime(timer, 0, ctypes.byref(ITS(TS(0, 0), TS(ns // 10**9, ns % 10**9))), None)
    setup_failures = []
    number = 0
    while number < 5 and len(setup_failures) < 4:
        offset = 150e-6 * number / 4
        signal.signal(signal.SIGTERM, terminate)
        signal.signal(signal.SIGALRM, deadline)
        before = guest(fingerprint)
        ctx = fresh_ctx()
        raised = None
        interrupted.clear()
        try:
            with browser.default_browser(ctx):
                arm(max(1e-6, offset))
            end = time.perf_counter() + 300e-6
            while time.perf_counter() < end:
                pass
        except RecipeFailed as error:
            raised = str(error)
        except RuntimeError as error:
            # Setup (prepare) failed before the transaction opened: not a signal trial.
            after = guest(fingerprint)
            setup_failures.append({'error': str(error)[:200], 'mime_exact': before == after,
                                   'active_leak': bool(browser._ACTIVE)})
            print(json.dumps({'setup_failure': setup_failures[-1]}), flush=True)
            browser._ACTIVE.clear()
            time.sleep(20)
            continue
        after = guest(fingerprint)
        row = {'trial': number + 1, 'offset_us': round(offset * 1e6, 1), 'raised': raised,
               'terminate_ran': interrupted == [signal.SIGTERM], 'mime_exact': before == after,
               'before': before.strip().splitlines(), 'after': after.strip().splitlines(),
               'reviews': ctx.reviews, 'notes': ctx.notes, 'active_leak': bool(browser._ACTIVE),
               'handler_restored': signal.getsignal(signal.SIGTERM) is terminate,
               'mask_clear': signal.SIGTERM not in signal.pthread_sigmask(signal.SIG_BLOCK, ())}
        row['ok'] = bool(row['raised'] and 'SIGTERM' in row['raised'] and row['terminate_ran'] and row['mime_exact']
                         and not row['active_leak'] and row['handler_restored'] and row['mask_clear']
                         and not any(line.startswith('leftover') for line in row['after']))
        results.append(row)
        print(json.dumps(row), flush=True)
        browser._ACTIVE.clear()
        number += 1
        time.sleep(10)  # Let the stopped private Firefox finish exiting.
    (OUT / 'browser-setup-failures.json').write_text(json.dumps(setup_failures, indent=1))
(OUT / (MODE + '-results.json')).write_text(json.dumps(results, indent=1))
print(json.dumps({'mode': MODE, 'trials': len(results), 'ok': sum(r['ok'] for r in results)}))
print(guest('rm -rf -- ' + root + '; echo private root removed').strip())
