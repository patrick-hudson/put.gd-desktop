#!/usr/bin/env python3
"""Prove fresh fixture pixels on every enabled VM wall listener after topology changes.

  multi_output_vm_wall.py RUN --output NEW_PRIVATE_DIR

Run on the VM host. Uses the run's pinned guest SSH transport, native output
inventory, native GTK fixture and existing per-head VNC listeners. Does not
launch put.gd, change topology, restart VNC or alter wall tiles. Disabled heads
are explicitly recorded. Keep the workstation tunnels/tiles visible throughout.
"""
import argparse
import json
from pathlib import Path
import re
import shlex
import shutil
from types import SimpleNamespace
import time

import lab
import multi_output
import rfb
import wall_probe


def pixel_inventory(c, outputs):
    """Current native mode/transform, independent of every VNC listener."""
    if c.profile['session'] == 'wayland' and c.profile['desktop'] != 'KDE':
        values = json.loads(c.session('python3 -c ' + shlex.quote(multi_output.MUTTER_OUTPUTS) + '\n'))
        return {v['name']: {'size': [v['pixelWidth'], v['pixelHeight']],
                            'rotated': bool(v['transform'] % 2)} for v in values}
    if c.profile['desktop'] == 'KDE' and c.profile['session'] == 'wayland':
        values = json.loads(c.session('kscreen-doctor -j\n'))['outputs']
        result = {}
        for v in values:
            if not v['enabled']:
                continue
            mode = next(m for m in v['modes'] if m['id'] == v['currentModeId'])
            result[v['name']] = {'size': [mode['size']['width'], mode['size']['height']],
                                 'rotated': v['rotation'] in (2, 8)}
        return result
    query = c.session('xrandr --query\n')
    result = {}
    for v in outputs:
        if not v['enabled']:
            continue
        line = next(line for line in query.splitlines() if line.startswith(v['name'] + ' connected'))
        result[v['name']] = {'size': [v['width'], v['height']],
                             'rotated': bool(re.search(r'\b(left|right)\s+\(', line))}
    return result


def wall_rect(name, native, qmp_size):
    candidates = [tuple(native['size'])]
    if native['rotated']:
        candidates.append(tuple(reversed(native['size'])))
    if tuple(qmp_size) not in candidates:
        raise RuntimeError('QMP scanout extent disagrees with native mode/rotation: ' + name)
    # A rotated scanout can have the raw or transformed extent. The independent
    # QMP frame selects which one this GPU exposes; VNC must match it exactly.
    return {'name': name, 'x': 0, 'y': 0, 'width': qmp_size[0], 'height': qmp_size[1]}


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run'); p.add_argument('--output', required=True)
    a = p.parse_args()
    c = multi_output.Campaign(SimpleNamespace(run=a.run, output=a.output))
    c.profile = lab.load_profile(lab.state(c.run)['profile'])
    # A removed head has no screendump; retain that explicit result in witnesses.
    c.current = 'output-change'
    result = {'ok': False, 'heads': [], 'runId': lab.state(c.run)['id'],
              'driverSha256': lab.hash_file(__file__), 'rfbSha256': lab.hash_file(rfb.__file__),
              'wallProbeSha256': lab.hash_file(wall_probe.__file__)}
    try:
        outputs = lab.guest_outputs(c.run, lab.layout_backend(c.profile))
        multi_output.save(c.out / 'native-outputs.json', outputs)
        heads = lab.state(c.run)['heads']
        if len(outputs['outputs']) != len(heads):
            raise RuntimeError('Native output inventory does not uniquely map to the configured wall heads')
        if c.profile['session'] == 'x11':
            result['tabletMappingsRefreshed'] = lab.map_tablets_x11(c.run, outputs['outputs'])
        c.fixture(motion=True, minimum_heads=1)
        pixels = pixel_inventory(c, outputs['outputs'])
        multi_output.save(c.out / 'native-pixel-modes.json', pixels)
        for index, (head, output) in enumerate(zip(heads, outputs['outputs'])):
            item = {'head': index, 'port': head['port'], 'nativeOutput': output}
            result['heads'].append(item)
            if not output['enabled']:
                item.update(disabled=True, ok=True)
                continue
            reference = lab.screenshot(c.run, 'wall-reference-' + c.attempt, index)
            target = c.out / ('head%d-reference.ppm' % index)
            shutil.copyfile(reference['path'], target)
            width, height, _ = lab.ppm(target)
            rect = wall_rect(output['name'], pixels[output['name']], (width, height))
            item['reference'] = {'path': target.name, 'sha256': lab.hash_file(target),
                                 'qmpSize': [width, height], 'nativeMode': pixels[output['name']]}
            client = rfb.RFB(head['port'])
            try:
                client.frame(); time.sleep(.25)
                first = client.frame(); time.sleep(.5); second = client.frame()
                item['freshness'] = wall_probe.check_frames(first, second, [rect])
                item['ok'] = item['freshness']['ok']
                if item['ok']:
                    shot = c.out / ('head%d.png' % index)
                    item['finalFrameGate'] = wall_probe.save_checked_frame(client, second, [rect], shot)
                    item['ok'] = item['finalFrameGate']['ok']
            finally:
                client.close()
        result['ok'] = bool(result['heads']) and all(h['ok'] for h in result['heads'])
    except Exception as error:
        result.update(ok=False, error=str(error))
    finally:
        c.stop_fixture()
        result['fixtureStopped'] = True
        multi_output.save(c.out / 'result.json', result)
        multi_output.save(c.out / 'sha256.json', {f.name: lab.hash_file(f) for f in c.out.iterdir()
                                               if f.is_file() and f.name != 'sha256.json'})
    print(json.dumps(result))
    return 0 if result['ok'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
