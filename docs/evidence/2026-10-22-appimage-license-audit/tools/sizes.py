"""sizes.py ROOT MATCH_JSON WORK: per-group raw and squashfs (zstd, 16 KiB blocks) bytes; copies files into WORK/<group>."""
import json, os, re, shutil, subprocess, sys
root, match, work = sys.argv[1], json.load(open(sys.argv[2])), sys.argv[3]
REC = set('gst-plugins-bad1.0 gst-plugins-base1.0 gst-plugins-good1.0 gst-plugins-ugly1.0 gstreamer-vaapi gstreamer1.0 pipewire x264 x265 libvpx libwebp opus vo-aacenc lame mpg123 flac libogg libvorbis libsndfile orc pulseaudio libva numactl libasyncns elfutils libunwind'.split())
srcof = {p: v['source'] for p, v in match['packages'].items()}
def group(rel):
    if rel.startswith('usr/libexec/putgd/proton') or rel.startswith('usr/share/doc/putgd/proton'): return 'proton-helper'
    if rel.startswith('usr/share/doc'): return 'licenses-notices'
    b = os.path.basename(rel)
    if re.match(r'lib(aws|s2n)', b): return 'aws-sdk'
    if re.match(r'lib(Qt6|icu|qt6keychain|LayerShellQt)', b) or rel.startswith(('usr/plugins', 'usr/translations')) or b in ('libadwaita.so','libbradient.so','libqgtk3.so','libxdg-shell.so','liblayer-shell.so','libqt-plugin-wayland-egl.so','qt.conf'): return 'qt'
    if re.match(r'lib(av|sw)', b) or rel.startswith(('usr/lib/gstreamer-1.0', 'usr/libexec/gstreamer-1.0', 'usr/lib/putgd-host-fallback')): return 'recording-media'
    if rel in match['matched']: return 'recording-media' if srcof[match['matched'][rel]] in REC else 'distro-other'
    return 'putgd'
groups = {}
for d, _, files in os.walk(root):
    for f in files:
        p = os.path.join(d, f); rel = os.path.relpath(p, root)
        if os.path.islink(p): continue
        g = group(rel); groups.setdefault(g, []).append(rel)
        dst = os.path.join(work, g, rel); os.makedirs(os.path.dirname(dst), exist_ok=True); shutil.copy2(p, dst)
out = {}
for g, rels in sorted(groups.items()):
    img = os.path.join(work, g + '.sqfs')
    subprocess.run(['mksquashfs', os.path.join(work, g), img, '-comp', 'zstd', '-b', '16384', '-noappend', '-no-xattrs', '-quiet', '-no-progress'], check=True, stdout=subprocess.DEVNULL)
    out[g] = {'files': len(rels), 'rawBytes': sum(os.path.getsize(os.path.join(root, r)) for r in rels), 'squashfsBytes': os.path.getsize(img)}
whole = os.path.join(work, 'whole.sqfs')
subprocess.run(['mksquashfs', root, whole, '-comp', 'zstd', '-b', '16384', '-noappend', '-no-xattrs', '-quiet', '-no-progress'], check=True, stdout=subprocess.DEVNULL)
out['_whole'] = {'squashfsBytes': os.path.getsize(whole)}
print(json.dumps(out, indent=1))
