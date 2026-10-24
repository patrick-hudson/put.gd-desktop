"""All-pixel attribution before any physical framebuffer bytes reach a saver."""
from bench_ui_process import EvidenceError

BASE = bytes((80, 90, 100))


def require(value, code):
    if not value:
        raise EvidenceError(code)


def cover_rgb(width, height, bgrx):
    count = width * height
    require(width > 0 and height > 0 and len(bgrx) == count * 4, 'invalid_frame_shape')
    require(bgrx[0::4] == bytes((100,)) * count and bgrx[1::4] == bytes((90,)) * count
            and bgrx[2::4] == bytes((80,)) * count, 'unattributed_pixels_in_cover_frame')
    # This constant is byte-identical to the admitted actual framebuffer. No
    # unknown pixels or tolerated percentage can be handed to the image encoder.
    return BASE * count


def selector_rgb(width, height, bgrx, overlays, excluded):
    """Keep only exact observed opaque pixels of authenticated selector drawables.

    Every other pixel is a declared synthetic replacement. In particular, a
    notification/panel on transparent background never reaches the saver. If
    it obscures an attributed stroke, refuse the whole result rather than
    synthesizing that stroke. The known cursor rectangle is replaced as well.
    """
    count = width * height
    require(width > 0 and height > 0 and len(bgrx) == count * 4, 'invalid_frame_shape')
    output = bytearray(BASE * count)
    black = white = 0
    for overlay in overlays:
        x, y, w, h, rgba = (overlay[key] for key in ('x', 'y', 'width', 'height', 'rgba'))
        require(w > 0 and h > 0 and 0 <= x and 0 <= y and x + w <= width and y + h <= height
                and len(rgba) == w * h * 4, 'invalid_owned_selector_geometry')
        for i in range(w * h):
            red, green, blue, alpha = rgba[i * 4:i * 4 + 4]
            require(alpha in (0, 255), 'selector_alpha_not_binary')
            if alpha == 0:
                continue
            rgb = bytes((red, green, blue))
            require(rgb in (b'\0\0\0', b'\xff\xff\xff'), 'unexpected_owned_selector_pixels')
            px, py = x + i % w, y + i // w
            if excluded and excluded[0] <= px < excluded[0] + excluded[2] and excluded[1] <= py < excluded[1] + excluded[3]:
                continue
            offset = (py * width + px) * 4
            actual = bytes((bgrx[offset + 2], bgrx[offset + 1], bgrx[offset]))
            require(actual == rgb, 'owned_selector_pixels_occluded')
            target = (py * width + px) * 3
            output[target:target + 3] = actual
            black += rgb == b'\0\0\0'
            white += rgb == b'\xff\xff\xff'
    require(black > 0 and white > 0, 'complete_current_selector_not_observed')
    return bytes(output), {'opaqueBlackPixelsVerified': black, 'opaqueWhitePixelsVerified': white,
        'allOtherPixelsReplacedWithSyntheticBackground': True, 'excludedCursorRectangle': excluded,
        'rawFramebufferRetained': False}


def save_cover(path, width, height, bgrx, saver=None):
    rgb = cover_rgb(width, height, bgrx)
    (saver or save_rgb)(path, width, height, rgb)


def save_selector(path, width, height, bgrx, overlays, excluded, saver=None):
    rgb, proof = selector_rgb(width, height, bgrx, overlays, excluded)
    (saver or save_rgb)(path, width, height, rgb)
    return proof


def save_rgb(path, width, height, rgb):
    from PIL import Image
    Image.frombytes('RGB', (width, height), rgb).save(path)
