"""Secret-safe evidence primitives. Raw values and pixels live only in memory.

Audit a completed run with ``python3 -m tools.lab.surface_driver.redact RUN``.
OCR is a mandatory pre-write guard for visible pixels and an evidence audit,
never a product feature. Remote runs and registered secrets require tesseract.
"""
from __future__ import annotations

import argparse
import base64
import collections
import stat
import tempfile
import dataclasses
import hashlib
import hmac
import io
import json
import math
import os
from pathlib import Path
import re
import shutil
import subprocess
from urllib.parse import unquote, urlsplit, quote, parse_qsl

URL = re.compile(r'https?://[^\s<>\x00"\'`\\]+', re.I)
URL_SPAN = re.compile(r'\[url \{[^{}\r\n]+\}\]')
REDACTED_SPANS = ('[redacted]', '[redacted AWS key]', '[redacted session/authorization]', '[redacted object]')
AWS = re.compile(r'\b(?:AKIA|ASIA|AIDA|AROA)[A-Z0-9]{16}\b')
SECRET_FIELD = re.compile(r'(?:password|secret(?!_scan)|token|authorization|^(?:session(?:[._-]?(?:id|uid|data))?|proton[._-]?session|x-pm-(?:uid|session))$|private.?key|access.?key|url$|link$)', re.I)
# Object-store hosts whose scheme-less host/path form is itself a working link:
# AWS S3, Backblaze B2 (friendly and S3 endpoints), common S3-compatible
# services and any host with an "s3" label (custom/self-hosted endpoints).
# A run also adds its destination profiles' configured endpoint hosts.
OBJECT_HOST = (r'(?:[a-z0-9-]+\.)*(?:s3(?:[.-][a-z0-9-]+)*\.amazonaws\.com(?:\.cn)?|backblazeb2\.com|backblaze\.com'
               r'|r2\.cloudflarestorage\.com|r2\.dev|digitaloceanspaces\.com|wasabisys\.com|linodeobjects\.com'
               r'|storage\.googleapis\.com|blob\.core\.windows\.net|s3(?:-[a-z0-9-]+)?(?:\.[a-z0-9-]+)+)')
LINK_TAIL = r'(?::[0-9]{1,5})?/[^\s<>"\'`\\]+'
SHARE = re.compile(r'(?:hxxps?://[^\s<>"\'`\\]+|(?<![\w./-])(?://)?(?:put\.gd/[^\s<>"\'`\\]+|drive\.proton\.me/urls/[^\s<>"\'`\\]+|'
                   + OBJECT_HOST + LINK_TAIL + '))', re.I)
# Scheme-less host/path text in a recipe's failure message is treated as a link.
LINKISH = re.compile(r'(?<![\w./@-])(?:[a-z0-9-]+\.)+[a-z][a-z0-9-]{1,62}(?::[0-9]{1,5})?/[^\s<>"\'`\\]+', re.I)


def host_links(hosts):
    """Scheme-less links on a run's configured endpoint hosts (and their subdomains)."""
    hosts = sorted({h.lower().strip('.') for h in hosts
                    if isinstance(h, str) and re.fullmatch(r'[A-Za-z0-9.:\[\]-]{3,253}', h)}, key=len, reverse=True)
    if not hosts:
        return None
    return re.compile(r'(?<![\w./-])(?://)?(?:[a-z0-9-]+\.)*(?:' + '|'.join(map(re.escape, hosts)) + ')' + LINK_TAIL, re.I)
ENUMS = frozenset(('text', 'image', 'files', 'local', 'tty', 'clipboard', 'wayland', 'x11', 'completed', 'cancelled', 'pending', 'recording', 'uploading', 'initializing', 'background', 'foreground', 'available', 'unavailable'))


def distinctive(value):
    if len(value) < 8 or value.lower() in ENUMS or len(set(value)) < 5:
        return False
    return -sum((n/len(value))*math.log2(n/len(value)) for n in collections.Counter(value).values()) >= 2.5


def public_url(value):
    # Exact product documentation/release paths only; arbitrary put.gd paths are shares.
    try:
        p = urlsplit(value)
        return (not p.query and
                ((p.hostname == 'github.com' and
                  (p.path in ('/patrick-hudson/put.gd-desktop', '/patrick-hudson/put.gd-desktop/releases', '/patrick-hudson/put.gd-desktop/releases/latest')
                   or re.fullmatch(r'/patrick-hudson/put\.gd-desktop/releases/tag/v[0-9A-Za-z.+-]+', p.path)
                   or re.fullmatch(r'/patrick-hudson/put\.gd-desktop/blob/(?:main|nightly)/docs/[a-z0-9/-]+\.md', p.path)))
                 or (p.hostname == 'put.gd' and p.path in ('', '/', '/docs', '/docs/'))))
    except ValueError:
        return False


def spellings(value):
    """Boundary-independent encodings, including base64 embedded in a larger blob."""
    data = value.encode()
    forms = {value, json.dumps(value, ensure_ascii=False)[1:-1], repr(value)[1:-1],
             quote(value, safe=''), quote(value), data.hex(), data.hex().upper(),
             ''.join('%%%02X' % b for b in data), ''.join('%%%02x' % b for b in data)}
    forms |= {re.sub(r'%[0-9A-F]{2}', lambda m:m[0].lower(), x) for x in forms}
    for n in range(3):
        for encode in (base64.b64encode, base64.urlsafe_b64encode):
            encoded = encode(b'\0'*n+data).decode()
            forms.add(encoded[(n*8+5)//6:((n+len(data))*8)//6])
            if n == 0:
                forms.update((encoded, encoded.rstrip('=')))
    return {x for x in forms if len(x) >= 8}

MARKER = re.compile(r'(?i)(?:bearer\s+[^\s\[]+'
                    r'|authorization\s*["\']?\s*[:=]\s*["\']?(?:(?:bearer|basic|aws4-hmac-sha256)\s+)?[^\s,}{\["\']+'
                    r'|(?:aws_secret_access_key|aws_session_token|secretAccessKey|secretKey|accessToken|refreshToken|sessionToken|sessionId|session\.uid|userKeyPassword|x-pm-uid|x-pm-session|proton.?session'
                    r'|x-amz-(?:signature|credential|security-token)|x-goog-(?:signature|credential)|awsaccesskeyid|(?<=[?&;])(?:signature|credential|sig))'
                    r'\s*["\']?\s*[:=]\s*["\']?[^\s,}{\["\']+)')
STRUCTURAL_FIELDS = frozenset({
    'interaction', 'desktop', 'package', 'host', 'placement', 'guest', 'destination',
    'recipe', 'build', 'functional', 'visual', 'at', 'path', 'reference', 'label', 'receipt', 'campaign',
    'hash', 'sha256', 'kind', 'image_audit', 'frame_backend', 'input_backend',
    'gitCommit', 'gitTree', 'version', 'baseVersion', 'packageVersion', 'sourceProvenance',
    'driver_source', 'run_id', 'lab_profile', 'run_directory', 'session_root', 'socket',
    'hostname', 'kernel', 'machine', 'ID', 'VERSION_ID', 'VARIANT_ID', 'session_type',
    'qt_platform', 'boat_sandbox_id', 'runId', 'stage', 'backend', 'exception_type',
    'operationId', 'recordingId', 'shareOperationId',
})


class Sensitive(str):
    """Explicitly unwrap with reveal(); str, repr and stdlib JSON are harmless.

    The str payload is a constant, not the raw value. This also protects default
    exception formatting, dataclass serialization and json.dumps(default=str).
    This is confinement of evidence, not a locked-memory/zeroization promise.
    """
    __slots__ = ('_raw',)

    def __new__(cls, value):
        instance = super().__new__(cls, '[redacted]')
        instance._raw = value
        return instance

    def reveal(self):
        return self._raw

    def to_json(self):
        return '[redacted]'

    def __reduce_ex__(self, protocol):
        raise TypeError('Sensitive cannot be pickled')

    def __reduce__(self):
        raise TypeError('Sensitive cannot be pickled')

    def __getstate__(self):
        raise TypeError('Sensitive state cannot be serialized')

    def __copy__(self):
        return '[redacted]'

    def __deepcopy__(self, memo):
        return '[redacted]'

    def __eq__(self, other):
        other = other.reveal() if isinstance(other, Sensitive) else other
        if type(self._raw) is not type(other):
            return False
        encode = lambda v: (v if isinstance(v, str) else repr(v)).encode()
        return hmac.compare_digest(encode(self._raw), encode(other))

    def __ne__(self, other):
        return not self == other

    def __hash__(self):
        return hash(self._raw)

    def __len__(self):
        raise TypeError('explicit reveal required')

    def __contains__(self, item):
        raise TypeError('explicit reveal required')

    def __iter__(self):
        raise TypeError('explicit reveal required')

    def __getitem__(self, item):
        raise TypeError('explicit reveal required')


class Redactor:
    def __init__(self):
        self._salt = os.urandom(32)  # deliberately never persisted
        self._secrets = set()
        self._hosts = set()
        self._host_links = None

    @property
    def link_hosts(self):
        return frozenset(self._hosts)

    def add_link_hosts(self, hosts):
        """Configured endpoint hosts: their scheme-less host/path text is a link."""
        self._hosts.update(h.lower() for h in hosts if isinstance(h, str) and h)
        self._host_links = host_links(self._hosts)

    def links(self, text):
        """Every URL, share/object-store link and configured-endpoint link in text."""
        found = list(URL.finditer(text)) + list(SHARE.finditer(text))
        return found + (list(self._host_links.finditer(text)) if self._host_links else [])

    @property
    def audit_literals(self):
        return frozenset(self._secrets)

    def remember(self, value, *, opaque=False, force=False):
        """Register raw leaves and URL components before handing results to recipes."""
        if isinstance(value, Sensitive):
            value, opaque, force = value.reveal(), True, True
        if isinstance(value, dict):
            for key, item in value.items():
                self.remember(key, force=force)  # Link-shaped keys are data too; schema names stay public.
                identity = key in STRUCTURAL_FIELDS or key in ('id', 'historyId', 'state', 'status', 'code', 'url_host', 'length', 'present', 'type', 'tool', 'mode', 'format', 'formats', 'category', 'sourceKind', 'mime', 'mimeType')
                self.remember(item, opaque=force or not identity and (opaque or bool(SECRET_FIELD.search(str(key)))), force=force)
        elif isinstance(value, (list, tuple)):
            for item in value:
                self.remember(item, opaque=opaque, force=force)
        elif isinstance(value, str):
            if opaque and distinctive(value) and (force or not public_url(value)):
                self._secrets.add(value)
            try:
                decoded = json.loads(value)
                if isinstance(decoded, (dict, list, str)) and decoded != value:
                    self.remember(decoded, opaque=opaque, force=force)
            except ValueError:
                pass
            for match in self.links(value):
                url = match.group()
                if public_url(url) and not force:
                    continue
                normalized = re.sub(r'^hxxp', 'http', url, flags=re.I)
                if '://' not in normalized:
                    normalized = 'https:'+normalized if normalized.startswith('//') else 'https://'+normalized
                self._secrets.update((url, normalized))
                try:
                    parsed = urlsplit(normalized)
                    parts = [normalized.split('://', 1)[1], parsed.path, parsed.path.lstrip('/'),
                             parsed.path+('?' + parsed.query if parsed.query else '')+('#'+parsed.fragment if parsed.fragment else ''),
                             parsed.query, parsed.fragment, parsed.username or '', parsed.password or '']
                    parts += parsed.path.split('/') + unquote(parsed.path).split('/')
                    parts += [Path(part).stem for part in unquote(parsed.path).split('/')]
                    parts += [v for _,v in parse_qsl(parsed.query, keep_blank_values=True)]
                    parts += re.split(r'[=&/!:]', unquote(parsed.fragment))
                    for part in parts:
                        for spelling in (part, unquote(part)):
                            if distinctive(spelling):
                                self._secrets.add(spelling)
                except ValueError:
                    pass

    def has_sensitive_url(self, value):
        text = str(value)
        if any(not public_url(m[0] if '://' in m[0] else 'https://'+m[0].lstrip('/')) for m in self.links(text)):
            return True
        return any(variant in text for secret in self._secrets if URL.fullmatch(secret)
                   for variant in (secret, secret.split('://', 1)[1]))

    def url(self, value):
        self.remember(value)
        if isinstance(value, Sensitive):
            value = value.reveal()
        value = str(value)
        self.remember(value)
        try:
            parsed = urlsplit(value)
            host = parsed.hostname if parsed.scheme in ('http', 'https') else None
        except ValueError:
            host = None
        # A registered path/query token can also occur in the hostname.
        # Derivatives obey the same literal replacement rule as every field.
        host = self.text(host) if host else host
        return {'host': host, 'hash': hmac.new(self._salt, value.encode(), hashlib.sha256).hexdigest(),
                'length': len(value), 'present': bool(value)}

    def text(self, value, *, replace_secrets=True):
        if isinstance(value, Sensitive):
            return '[redacted]'
        value = str(value).replace('\\/', '/')
        def scrub(part):
            part = unquote(part)
            self.remember(part)
            part = AWS.sub('[redacted AWS key]', part)
            part = MARKER.sub('[redacted session/authorization]', part)
            if replace_secrets:
                for secret in sorted(self._secrets, key=len, reverse=True):
                    part = re.sub(re.escape(secret.encode().hex()), '[redacted]', part, flags=re.I)
                    for spelling in sorted(spellings(secret), key=len, reverse=True):
                        part = part.replace(spelling, '[redacted]')
            # Decoding may expose a public documentation URL not in the secret registry.
            part = URL.sub(lambda m: '[url '+json.dumps(self.url(m[0]), sort_keys=True)+']', part)
            return part
        # Keep safe derivatives outside substring replacement too: a one-letter
        # typed secret must not damage a salted hash or its JSON field names.
        pieces, offset = [], 0
        matches = self.links(value)
        derivatives = [self.url(match.group()) for match in matches]
        spans = [(m.start(), m.end(), '[url '+json.dumps(d, sort_keys=True)+']') for m, d in zip(matches, derivatives)]
        for match in URL_SPAN.finditer(value):
            try:
                d = json.loads(match.group()[5:-1])
                valid = (set(d) == {'host', 'hash', 'length', 'present'}
                    and isinstance(d['hash'], str) and re.fullmatch('[0-9a-f]{64}', d['hash'])
                    and type(d['length']) is int and d['length'] >= 0 and type(d['present']) is bool
                    and (d['host'] is None or isinstance(d['host'], str)
                         and not URL.search(d['host']) and not AWS.search(d['host']) and not MARKER.search(d['host'])))
                if (valid and not any(secret in match.group() for secret in self._secrets)
                        and not detected(json.dumps(d, ensure_ascii=False), self._secrets)):
                    spans.append((match.start(), match.end(), match.group()))
            except (ValueError, TypeError, KeyError):
                pass
        for marker in REDACTED_SPANS:
            spans += [(m.start(), m.end(), marker) for m in re.finditer(re.escape(marker), value)]
        for start, end, replacement in sorted(spans):
            if start < offset:
                continue
            pieces += [scrub(value[offset:start]), replacement]
            offset = end
        return ''.join(pieces)+scrub(value[offset:])

    def safe(self, value):
        if isinstance(value, Sensitive):
            raw = value.reveal()
            if isinstance(raw, str) and URL.fullmatch(raw):
                return self.url(raw)
            return {'redacted': True, 'length': len(raw) if hasattr(raw, '__len__') else None,
                    'present': bool(raw)}
        if dataclasses.is_dataclass(value):
            value = {f.name: getattr(value, f.name) for f in dataclasses.fields(value)}
        if isinstance(value, dict):
            self.remember(value)
            result = {}
            for key, item in value.items():
                # Dictionary keys here are schema field names. Raw responses
                # are wrapped in Sensitive at persistence boundaries; patterns
                # still protect a malformed URL/key/marker used as a field.
                key = self.text(key)
                derivative = isinstance(item, dict) and set(item) <= {'host', 'hash', 'length', 'present', 'redacted'}
                if SECRET_FIELD.search(key) and item is not None and not derivative and not isinstance(item, (bool, int, float, Sensitive)):
                    self.remember(item, opaque=True)
                    result[key] = self.safe(Sensitive(item))
                elif key in STRUCTURAL_FIELDS and isinstance(item, str):
                    result[key] = self.text(item)
                elif key in ('note', 'review_reason', 'blocked', 'reason', 'error', 'message') and isinstance(item, str):
                    # Receipt text fields keep their schema type even when the
                    # entire message is one URL. Derivatives stay inside text.
                    result[key] = self.text(item)
                else:
                    result[key] = self.safe(item)
            return result
        if isinstance(value, (tuple, list)):
            return [self.safe(item) for item in value]
        if isinstance(value, str):
            return self.url(value) if URL.fullmatch(value) else self.text(value)
        if value is None or isinstance(value, (bool, int, float)):
            return value
        return '[redacted object]'

    def exception(self, error):
        from .api import RecipeBlocked, RecipeFailed
        if isinstance(error, (RecipeBlocked, RecipeFailed)):
            if str(error).startswith('QMP US keyboard cannot type portable key '):
                # The backend's diagnostic quotes a single input character,
                # which can be a fragment of a longer registered secret.
                return 'requested text uses a key unavailable to this input backend'
            # Recipes should pass links through ctx.url_evidence(); a scheme-less
            # host/path quoted anyway is replaced by its derivative by pattern.
            message = LINKISH.sub(lambda m: '[url '+json.dumps(self.url('https://'+m[0]), sort_keys=True)+']', str(error))
            return self.text(message)
        # Exception arguments/chains/traceback locals can contain raw commands.
        return 'driver error: '+type(error).__name__


def rect(value):
    """A framebuffer rectangle in device pixels; invalid masks fail closed."""
    if not isinstance(value, (list, tuple)) or len(value) != 4:
        raise ValueError('sensitive rectangle needs x, y, width, height')
    if any(isinstance(v, bool) or not isinstance(v, (int, float)) or not math.isfinite(v) for v in value):
        raise ValueError('sensitive rectangle must contain finite numbers')
    x, y, w, h = value
    if w <= 0 or h <= 0:
        raise ValueError('sensitive rectangle must have positive area')
    # Round outwards, including fractional HiDPI edges.
    return [math.floor(x), math.floor(y), math.ceil(x+w)-math.floor(x), math.ceil(y+h)-math.floor(y)]


_OCR_CACHE = {}


def save_frame(image, path, regions, *, crop=None, fallback=False, literals=(), ocr=None, hosts=()):
    """Mask/crop in memory, strip metadata, then perform the first pixel write."""
    image = image.convert('RGB')
    masks = []
    for region in regions:
        x, y, w, h = rect(region)
        left, top, right, bottom = max(0, x), max(0, y), min(image.width, x+w), min(image.height, y+h)
        if left < right and top < bottom:
            image.paste((0, 0, 0), (left, top, right, bottom))
            masks.append([left, top, right-left, bottom-top])
    if crop:
        x, y, w, h = rect(crop)
        if x < 0 or y < 0 or x+w > image.width or y+h > image.height:
            raise ValueError('window crop extends outside captured head')
        image = image.crop((x, y, x+w, y+h))
        masks = [[max(0, mx-x), max(0, my-y), min(x+w, mx+mw)-max(x, mx), min(y+h, my+mh)-max(y, my)]
                 for mx, my, mw, mh in masks if mx < x+w and my < y+h and mx+mw > x and my+mh > y]
    guard, text = 'fully-masked', None
    if image.getbbox():
        try:
            text = ocr_text(image, ocr=ocr)
        except Exception:
            text, guard = '', 'ocr-unavailable-fully-masked'
            fallback = True
        else:
            guard = 'ocr-checked'
            if ocr_detected(text, literals, hosts):
                guard, fallback = 'ocr-sensitive-text-fully-masked', True
        if fallback and guard != 'ocr-checked':
            image.paste((0, 0, 0), (0, 0, image.width, image.height))
            masks = [[0, 0, image.width, image.height]]
            text = None
    # Copy pixels into a fresh object: PNG text/EXIF/ICC metadata is discarded.
    from PIL import Image
    clean = Image.frombytes('RGB', image.size, image.tobytes())
    stream = io.BytesIO()
    clean.save(stream, format='PNG')
    encoded = stream.getvalue()
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.write_bytes(encoded)
    path.chmod(0o600)
    digest = hashlib.sha256(encoded).hexdigest()
    if text is not None:
        # RAM only: the end-of-run audit re-matches this frame's text against
        # literals registered later, without reading the pixels twice.
        _OCR_CACHE[digest] = text
        while len(_OCR_CACHE) > 512:
            _OCR_CACHE.pop(next(iter(_OCR_CACHE)))
    return {'sha256': digest, 'size': list(clean.size),
            'regions': masks, 'fallback': fallback, 'guard': guard, 'visible_pixels': bool(clean.getbbox())}


def patterns(text, extra=None):
    text = text.replace('\\/', '/')
    found = []
    if URL.search(text) or SHARE.search(text) or (extra and extra.search(text)):
        found.append('url')
    if AWS.search(text):
        found.append('aws-key')
    if any(re.split(r'[:=]', match.group())[-1].strip(' \"\'') not in ('null', 'None', 'true', 'false') for match in MARKER.finditer(text)):
        found.append('session-or-authorization')
    return found


def decoded_forms(text):
    yield text
    yield unquote(text)
    # Standalone audits also recognize encoded link shapes without a registry.
    for m in re.finditer(r'[A-Za-z0-9_+/=-]{16,}', text):
        for offset in range(4):
            try:
                value = m[0][offset:]
                yield base64.urlsafe_b64decode(value+'='*((-len(value))%4)).decode('utf-8', errors='replace')
            except ValueError:
                pass
        try:
            yield bytes.fromhex(m[0]).decode('utf-8', errors='replace')
        except ValueError:
            pass


def detected(text, literals=(), hosts=()):
    kinds = set()
    candidates = tuple(decoded_forms(text))
    extra = host_links(hosts)
    for value in candidates:
        kinds.update(patterns(value, extra))
    if any(spelling in candidate for literal in literals for spelling in spellings(literal) for candidate in candidates):
        kinds.add('registered-secret')
    return sorted(kinds)


# OCR reads l/I/1, O/0, S/5 ... interchangeably and splits or joins words.
CONFUSABLES = str.maketrans('0oqd1ijt|!5$8z2g96', 'ooooolllllssbzzggg')


def ocr_normal(text):
    return re.sub(r'[^a-z0-9]', '', text.lower().translate(CONFUSABLES))


def fuzzy_contains(text, literal):
    """literal occurs in normalized OCR text with at most len/8 edits.

    Pigeonhole: with e edits, one of e+1 literal chunks survives exactly;
    each exact chunk anchors a bounded semi-global edit-distance check.
    """
    m = len(literal)
    errors = m // 8
    if literal in text:
        return True
    if not errors:
        return False
    size = m // (errors + 1)
    for index in range(errors + 1):
        chunk = literal[index*size:(index+1)*size]
        offset = text.find(chunk)
        while offset >= 0:
            start = max(0, offset - index*size - errors)
            window = text[start:offset - index*size + m + errors]
            previous = [0] * (len(window) + 1)
            for i, character in enumerate(literal, 1):
                current = [i] + [0] * len(window)
                for j, other in enumerate(window, 1):
                    current[j] = min(previous[j]+1, current[j-1]+1, previous[j-1]+(character != other))
                previous = current
            if min(previous) <= errors:
                return True
            offset = text.find(chunk, offset+1)
    return False


def ocr_detected(text, literals=(), hosts=()):
    """Audit OCR text: OCR inserts spaces and confuses glyphs, so match loosely.

    Patterns are checked as read and with whitespace removed; registered
    literals are compared after whitespace/punctuation removal and glyph
    folding, with a small edit budget. A false positive only masks a frame.
    """
    kinds = set(detected(text, literals, hosts)) | set(detected(re.sub(r'\s+', '', text), (), hosts))
    normalized = ocr_normal(text)
    for literal in literals:
        folded = ocr_normal(literal)
        if len(folded) >= 10 and fuzzy_contains(normalized, folded):
            kinds.add('registered-secret')
            break
    return sorted(kinds)


def ocr_text(image, *, ocr=None):
    """Tesseract over an upscaled grayscale copy, read as drawn and inverted.

    UI and notification text is often 11-13 px and light-on-dark; Tesseract
    misses it at native size, so the audit reads a 3x (large frames: 2x) copy.
    """
    from PIL import Image, ImageOps
    tesseract = shutil.which('tesseract') if ocr is None else ocr
    if not tesseract:
        raise RuntimeError('OCR required: tesseract is unavailable')
    gray = image.convert('L')
    scale = 3 if gray.width * gray.height <= 1_200_000 else 2
    gray = gray.resize((gray.width*scale, gray.height*scale), Image.Resampling.LANCZOS)
    texts = []
    for variant in (gray, ImageOps.invert(gray)):
        stream = io.BytesIO()
        variant.save(stream, format='PNG')
        result = subprocess.run([tesseract, 'stdin', 'stdout', '--psm', '11'], input=stream.getvalue(), capture_output=True,
                                timeout=120, check=False, env=dict(os.environ, OMP_THREAD_LIMIT='1'))
        if result.returncode:
            raise RuntimeError('OCR failed')
        texts.append(result.stdout.decode('utf-8', errors='replace'))
    return '\n'.join(texts)


# Self-test tokens use glyphs OCR does not confuse with one another (see
# CONFUSABLES), so an exact read is required rather than a fuzzy one.
OCR_PROBE_ALPHABET = 'ACEFHKMNPRUVWXY347'


def ocr_self_test(ocr=None, size=11):
    """Render a fresh random link at the smallest UI text size; OCR must read it.

    Both polarities (dark on light, light on a dark notification banner) must
    yield the link pattern and exactly that run's random token, through the
    frame guard's own reader. A blind, broken or replaying OCR fails here.
    """
    import secrets
    from PIL import Image, ImageDraw, ImageFont
    font = None
    for candidate in ('/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf', '/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf',
                      '/usr/share/fonts/TTF/DejaVuSans.ttf', '/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf',
                      '/usr/share/fonts/google-noto/NotoSans-Regular.ttf', '/usr/share/fonts/truetype/ubuntu/Ubuntu-R.ttf'):
        if Path(candidate).is_file():
            font = ImageFont.truetype(candidate, size)
            break
    if font is None:
        font = ImageFont.load_default(size=size)
    for foreground, background in (('black', 'white'), ('#f0f0f0', '#2b2b2b')):
        # A new token for each polarity: neither image's answer fits the other.
        token = ''.join(secrets.choice(OCR_PROBE_ALPHABET) for _ in range(12))
        image = Image.new('RGB', (520, 40), background)
        ImageDraw.Draw(image).text((10, 12), 'Link copied: https://put.gd/' + token + '.png', font=font, fill=foreground)
        text = ocr_text(image, ocr=ocr)
        if 'url' not in ocr_detected(text) or token not in re.sub(r'\s+', '', text):
            return False
    return True


def scan_run(directory, *, frames=None, ocr=None, literals=(), require_ocr=False, quarantine=False, hosts=(), paths=None):
    """Never include a matched value, OCR text or an unsafe filename in findings.

    paths limits the audit to those files (relative to directory), for bytes
    rewritten after the full audit: the last write is always the last scan.
    """
    root = Path(directory)
    if frames is None:
        try:
            frames = json.loads((root/'frame-masks.json').read_text())['frames']
        except (OSError, ValueError, KeyError, TypeError):
            frames = {}
    tesseract = shutil.which('tesseract') if ocr is None else ocr
    check = lambda text: detected(text, literals, hosts)
    findings, count, pngs = [], 0, 0
    if not tesseract and (require_ocr or literals):
        findings.append({'kind': 'ocr-required-unavailable', 'blocked': 'tesseract must be staged in the run sandbox/guest'})
    quarantined = []
    selected = sorted(root.rglob('*')) if paths is None else sorted(root/p for p in paths if (root/p).exists())
    for index, path in enumerate(selected):
        # Indexes, not filenames: a leaked URL may be in the filename itself.
        before = len(findings)
        report = lambda kind: findings.append({'file_index': index, 'kind': kind})
        for kind in check(str(path.relative_to(root))):
            report('filename-'+kind)
        if path.is_symlink():
            report('symlink-not-audited')
        if path.is_symlink() or path.is_dir():
            if quarantine and len(findings) > before:
                quarantined.append((index, path))
            continue
        count += 1
        try:
            data = path.read_bytes()
            for kind in check(data.decode('utf-8', errors='replace')):
                report(kind)
            if path.suffix.lower() == '.json':
                # Decode JSON escaping before matching (e.g. escaped slashes).
                decoded = json.loads(data)
                for kind in check(json.dumps(decoded, ensure_ascii=False)):
                    if kind not in check(data.decode('utf-8', errors='replace')):
                        report(kind)
            if path.suffix.lower() == '.png':
                from PIL import Image, ImageStat
                pngs += 1
                with Image.open(io.BytesIO(data)) as image:
                    image.load()
                    for kind in check(str(image.info)):
                        report('png-metadata-'+kind)
                    manifest = frames.get(str(path.relative_to(root)))
                    if not manifest or manifest.get('sha256') != hashlib.sha256(data).hexdigest():
                        report('missing-or-changed-frame-manifest')
                    else:
                        if manifest.get('size') != list(image.size):
                            report('frame-size-mismatch')
                        for region in manifest.get('regions', []):
                            x, y, w, h = rect(region)
                            if x < 0 or y < 0 or x+w > image.width or y+h > image.height:
                                report('mask-outside-frame')
                            elif any(ImageStat.Stat(image.crop((x, y, x+w, y+h)).convert('RGB')).extrema[c][1] for c in range(3)):
                                report('unmasked-sensitive-region')
                    if not tesseract and image.convert('RGB').getbbox():
                        report('png-ocr-unavailable')
                    if tesseract and image.convert('RGB').getbbox():
                        try:
                            text = _OCR_CACHE.get(hashlib.sha256(data).hexdigest()) or ocr_text(image, ocr=tesseract)
                        except Exception:
                            report('ocr-failed')
                        else:
                            # Re-checked against literals registered after the frame was saved.
                            for kind in ocr_detected(text, literals, hosts):
                                report('png-text-'+kind)
        except Exception as error:
            findings.append({'file_index': index, 'kind': 'unreadable-or-invalid-artifact',
                             'exception_type': type(error).__name__})
        if quarantine and len(findings) > before:
            quarantined.append((index, path))
    actions = quarantine_files(root, quarantined) if quarantined else []
    return {'quarantine': actions, 'passed': not findings, 'files': count, 'pngs': pngs,
            'image_audit': 'ocr-and-known-regions' if tesseract else 'known-regions', 'findings': findings}


def quarantine_files(root, files):
    """Outside the export root; names never preserve attacker-controlled text."""
    directory = Path(tempfile.mkdtemp(prefix='putgd-surface-quarantine-'))
    directory.chmod(0o700)
    actions = []
    moved = []
    for index, path in files:
        if any(parent in path.parents for parent in moved):
            continue
        destination = directory/str(index)
        # Quarantine may live on another mount (including sandbox bind mounts
        # that report the same st_dev). Copy into its private directory and
        # remove the source when an atomic rename is unavailable.
        shutil.move(path, destination)
        moved.append(path)
        if not destination.is_symlink():
            destination.chmod(0o700 if destination.is_dir() else 0o600)
        actions.append({'file_index': index, 'action': 'moved-to-private-quarantine'})
    return actions


# Capability contracts for the live-run admission check. These belong to the
# actual functions used by Evidence, so replacing/disconnecting a sink closes it.
Redactor.evidence_revision = 2
save_frame.evidence_revision = 2
scan_run.evidence_revision = 2


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--coverage', type=Path, help='campaign coverage to invalidate (defaults to RUN/coverage.json)')
    parser.add_argument('--literals-file', type=Path, help='private JSON array of in-memory run literals; never exported')
    args = parser.parse_args(argv)
    literals = ()
    if args.literals_file:
        info = args.literals_file.lstat()
        if not stat.S_ISREG(info.st_mode) or info.st_uid != os.getuid() or info.st_mode & 0o077:
            parser.error('literals file must be an owned private regular file')
        literals = json.loads(args.literals_file.read_text())
        if not isinstance(literals, list) or not all(isinstance(v, str) and distinctive(v) for v in literals):
            parser.error('literals file must contain distinctive strings')
    redactor = Redactor()
    for literal in literals:
        redactor.remember(Sensitive(literal))
    literals = redactor.audit_literals
    receipt = args.run/'receipt.json'
    try:
        value = json.loads(receipt.read_text()) if receipt.exists() else None
    except (OSError, ValueError):
        value = None
    result = scan_run(args.run, literals=literals, require_ocr=bool(value and value.get('environment', {}).get('destination', 'local') != 'local'), quarantine=True)
    if value and not value.get('secret_scan', {'passed': True}).get('passed', True):
        previous = value['secret_scan']
        result['passed'] = False
        result['findings'] += previous.get('findings', [])
        result['quarantine'] += previous.get('quarantine', [])
    # Import lazily to keep receipts -> redact free of circular imports.
    from .receipts import fail_coverage, write_json
    receipt = args.run/'receipt.json'
    if value is not None:
        value['secret_scan'] = result
        if not result['passed']:
            for row in value.get('interactions', []):
                row['functional'] = 'failed'
        write_json(receipt, value, redactor=redactor)
        if not result['passed']:
            fail_coverage(args.coverage or args.run/'coverage.json', value.get('interactions', []), redactor=redactor)
    print(json.dumps(result))
    return 0 if result['passed'] else 1


if __name__ == '__main__':
    raise SystemExit(main())
