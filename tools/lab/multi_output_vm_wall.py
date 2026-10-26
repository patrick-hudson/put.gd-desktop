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
from types import SimpleNamespace
import time

import lab
import multi_output
import rfb
import wall_probe


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument('run'); p.add_argument('--output', required=True)
    a = p.parse_args()
    c = multi_output.Campaign(SimpleNamespace(run=a.run, output=a.output))
    c.profile = lab.load_profile(lab.state(c.run)['profile'])
    # A removed head has no screendump; retain that explicit result in witnesses.
    c.current = 'output-change'
    result = {'ok': False, 'heads': [], 'runId': lab.state(c.run)['id'],
              'driverSha256': lab.hash_file(__file__)}
    try:
        outputs = lab.guest_outputs(c.run, lab.layout_backend(c.profile))
        multi_output.save(c.out / 'native-outputs.json', outputs)
        heads = lab.state(c.run)['heads']
        if len(outputs['outputs']) != len(heads):
            raise RuntimeError('Native output inventory does not uniquely map to the configured wall heads')
        c.fixture(motion=True, minimum_heads=1)
        for index, (head, output) in enumerate(zip(heads, outputs['outputs'])):
            item = {'head': index, 'port': head['port'], 'nativeOutput': output}
            result['heads'].append(item)
            if not output['enabled']:
                item.update(disabled=True, ok=True)
                continue
            client = rfb.RFB(head['port'])
            try:
                client.frame(); time.sleep(.25)
                first = client.frame(); time.sleep(.5); second = client.frame()
                # A rotated physical framebuffer can retain its unrotated pixel
                # extent; native logical geometry is recorded independently above.
                rect = {'name': output['name'], 'x': 0, 'y': 0,
                        'width': first[0], 'height': first[1]}
                item['freshness'] = wall_probe.check_frames(first, second, [rect])
                item['ok'] = item['freshness']['ok']
                if item['ok']:
                    shot = c.out / ('head%d.png' % index)
                    last = client.screenshot(shot)
                    item['finalFrameGate'] = wall_probe.check_frames(second, last, [rect])
                    item['ok'] = item['finalFrameGate']['ok']
                    if not item['ok']:
                        shot.unlink()
            finally:
                client.close()
        result['ok'] = bool(result['heads']) and all(h['ok'] for h in result['heads'])
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
