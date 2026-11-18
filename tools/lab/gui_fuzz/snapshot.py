"""Pseudo windows: things that are not windows (Shell actors, panels, notification regions) in the fence's window list."""


def pseudo(ident, cls, frame, z, kind='ShellOverlay', title='', active=False):
    frame = [int(v or 0) for v in frame]
    return {'id': ident, 'pid': 0, 'cls': cls, 'title': title, 'frame': frame, 'content': frame, 'active': active, 'min': False, 'popup': False,
            'dock': False, 'desktop': False, 'normal': False, 'notif': False, 'fullscreen': False, 'tr': None, 'kind': kind, 'z': z}


NOTIFICATION_BOX = (420, 200)  # where Cinnamon's notification bubbles may lie: the right-hand corners of every monitor


def chrome_regions(monitors, workarea):
    """Cinnamon's panel and notification bubbles are Clutter actors, invisible to X. Stand-ins that put.gd's fence must deny:
    whatever part of a monitor lies outside the work area (the panel and any other strut), and both right-hand corners of every monitor."""
    regions = []
    for m in monitors:
        mx, my, mw, mh = m['geom']
        if workarea:
            wx, wy, ww, wh = workarea
            ix0, iy0, ix1, iy1 = max(mx, wx), max(my, wy), min(mx + mw, wx + ww), min(my + mh, wy + wh)
            if ix0 >= ix1 or iy0 >= iy1:
                regions.append([mx, my, mw, mh])  # no work area on this monitor: all of it is chrome
            else:
                for strip in ([mx, my, mw, iy0 - my], [mx, iy1, mw, my + mh - iy1], [mx, iy0, ix0 - mx, iy1 - iy0], [ix1, iy0, mx + mw - ix1, iy1 - iy0]):
                    if strip[2] > 0 and strip[3] > 0:
                        regions.append(strip)
        else:
            regions.append([mx, my, mw, mh])  # the work area is unknown: deny everything
        bw, bh = min(NOTIFICATION_BOX[0], mw), min(NOTIFICATION_BOX[1], mh)
        regions.append([mx + mw - bw, my, bw, bh])
        regions.append([mx + mw - bw, my + mh - bh, bw, bh])
    return regions
