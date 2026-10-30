"""Secret-safe evidence primitives. Raw values and pixels live only in memory.

Audit a completed run with ``python3 -m tools.lab.surface_driver.redact RUN``.
OCR is an optional evidence audit using an installed tesseract, never a product
feature. Without it, every PNG must have a verified masking manifest.
"""
from __future__ import annotations

import argparse
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
from urllib.parse import unquote, urlsplit

URL = re.compile(r'https?://[^\s<>\x00"\'`\\]+', re.I)
URL_SPAN = re.compile(r'\[url \{[^{}\r\n]+\}\]')
REDACTED_SPANS = ('[redacted]', '[redacted AWS key]', '[redacted session/authorization]', '[redacted object]')
AWS = re.compile(r'\b(?:AKIA|ASIA|AIDA|AROA)[A-Z0-9]{16}\b')
SECRET_FIELD = re.compile(r'(?:password|secret(?!_scan)|token|authorization|session(?:id|token|data|$)|private.?key|access.?key|url$|link$|clipboard)', re.I)
MARKER = re.compile(r'(?i)(?:bearer\s+[^\s\[]+|(?:aws_secret_access_key|aws_session_token|secretAccessKey|secretKey|accessToken|refreshToken|sessionToken|sessionId|session\.uid|userKeyPassword|x-pm-uid|x-pm-session|proton.?session)\s*["\']?\s*[:=]\s*["\']?[^\s,}{\["\']+)')
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
    def __new__(cls, value):
        instance = super().__new__(cls, '[redacted]')
        instance._raw = value
        return instance

    def reveal(self):
        return self._raw

    def to_json(self):
        return '[redacted]'


class Redactor:
    def __init__(self):
        self._salt = os.urandom(32)  # deliberately never persisted
        self._secrets = set()
        self.audit_literals = set()

    def remember(self, value, *, opaque=False):
        """Register sensitive leaves before a raw resident result reaches a recipe."""
        if isinstance(value, Sensitive):
            value = value.reveal()
            # Explicit, distinctive values only. Ordinary typed characters and
            # common UI words must not make every artifact a false positive.
            if isinstance(value, str) and len(value) >= 12 and len(set(value)) >= 6:
                self.audit_literals.add(value)
            opaque = True
        if isinstance(value, dict):
            for key, item in value.items():
                if key in ('id', 'operationId', 'historyId', 'recordingId', 'shareOperationId', 'state', 'status', 'code', 'host', 'url_host'):
                    # Contract identities/statuses are allowed evidence. URL/key
                    # patterns in a malformed value are still registered below.
                    self.remember(item)
                    continue
                self.remember(item, opaque=opaque or bool(SECRET_FIELD.search(str(key))))
        elif isinstance(value, (list, tuple)):
            for item in value:
                self.remember(item, opaque=opaque)
        elif isinstance(value, str):
            if opaque and value:
                self._secrets.add(value)
                # Legacy MCP text blocks can contain a JSON-encoded result.
                # Register its leaves too, before a recipe decodes the text.
                try:
                    decoded = json.loads(value)
                except ValueError:
                    decoded = None
                if isinstance(decoded, (dict, list, str)) and decoded != value:
                    self.remember(decoded, opaque=True)
            for match in URL.finditer(value):
                url = match.group()
                self._secrets.add(url)
                try:
                    parsed = urlsplit(url)
                    # Protect a fragment/token copied separately into a failure.
                    for part in (parsed.fragment, parsed.query):
                        if part:
                            self._secrets.add(part)
                            for token in re.split(r'[=&]', unquote(part)):
                                if len(token) >= 4:
                                    self._secrets.add(token)
                except ValueError:
                    pass

    def url(self, value):
        if isinstance(value, Sensitive):
            value = value.reveal()
        value = str(value)
        self.remember(value)
        try:
            parsed = urlsplit(value)
            host = parsed.hostname if parsed.scheme in ('http', 'https') else None
        except ValueError:
            host = None
        return {'host': host, 'hash': hmac.new(self._salt, value.encode(), hashlib.sha256).hexdigest(),
                'length': len(value), 'present': bool(value)}

    def text(self, value, *, replace_secrets=True):
        if isinstance(value, Sensitive):
            return '[redacted]'
        value = str(value).replace('\\/', '/')
        def scrub(part):
            part = AWS.sub('[redacted AWS key]', part)
            part = MARKER.sub('[redacted session/authorization]', part)
            if replace_secrets:
                for secret in sorted(self._secrets, key=len, reverse=True):
                    for spelling in (secret, json.dumps(secret, ensure_ascii=False)[1:-1], repr(secret)[1:-1]):
                        part = part.replace(spelling, '[redacted]')
            return part
        # Keep safe derivatives outside substring replacement too: a one-letter
        # typed secret must not damage a salted hash or its JSON field names.
        pieces, offset = [], 0
        matches = list(URL.finditer(value))
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
                if valid and not any(match.group() in secret for secret in self._secrets):
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
            value = dataclasses.asdict(value)
        if isinstance(value, dict):
            result = {}
            for key, item in value.items():
                # Dictionary keys here are schema field names. Raw responses
                # are wrapped in Sensitive at persistence boundaries; patterns
                # still protect a malformed URL/key/marker used as a field.
                key = self.text(key, replace_secrets=False)
                derivative = isinstance(item, dict) and set(item) <= {'host', 'hash', 'length', 'present', 'redacted'}
                if SECRET_FIELD.search(key) and item is not None and not derivative and not isinstance(item, (bool, int, float, Sensitive)):
                    self.remember(item, opaque=True)
                    result[key] = self.safe(Sensitive(item))
                elif key in STRUCTURAL_FIELDS and isinstance(item, str):
                    result[key] = self.text(item, replace_secrets=False)
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
            return self.text(str(error))
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


def save_frame(image, path, regions, *, crop=None, fallback=False):
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
    # Copy pixels into a fresh object: PNG text/EXIF/ICC metadata is discarded.
    from PIL import Image
    clean = Image.frombytes('RGB', image.size, image.tobytes())
    stream = io.BytesIO()
    clean.save(stream, format='PNG')
    encoded = stream.getvalue()
    path = Path(path)
    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    path.write_bytes(encoded)
    return {'sha256': hashlib.sha256(encoded).hexdigest(), 'size': list(clean.size),
            'regions': masks, 'fallback': fallback}


def patterns(text):
    text = text.replace('\\/', '/')
    found = []
    if URL.search(text):  # paths themselves can be bearer tokens, without ?/#
        found.append('url')
    if AWS.search(text):
        found.append('aws-key')
    if any(re.split(r'[:=]', match.group())[-1].strip(' \"\'') not in ('null', 'None', 'true', 'false') for match in MARKER.finditer(text)):
        found.append('session-or-authorization')
    return found


def scan_run(directory, *, frames=None, ocr=None, literals=()):
    """Never include a matched value, OCR text or an unsafe filename in findings."""
    root = Path(directory)
    if frames is None:
        try:
            frames = json.loads((root/'frame-masks.json').read_text())['frames']
        except (OSError, ValueError, KeyError, TypeError):
            frames = {}
    tesseract = shutil.which('tesseract') if ocr is None else ocr
    def detected(text):
        kinds = patterns(text)
        if any(value in text for value in literals):
            kinds.append('registered-secret')
        return kinds
    findings, count, pngs = [], 0, 0
    for index, path in enumerate(sorted(root.rglob('*'))):
        # Indexes, not filenames: a leaked URL may be in the filename itself.
        report = lambda kind: findings.append({'file_index': index, 'kind': kind})
        if path.is_symlink():
            report('symlink-not-audited')
            continue
        if path.is_dir():
            continue
        count += 1
        for kind in detected(str(path.relative_to(root))):
            report('filename-'+kind)
        try:
            data = path.read_bytes()
            for kind in detected(data.decode('utf-8', errors='replace')):
                report(kind)
            if path.suffix.lower() == '.json':
                # Decode JSON escaping before matching (e.g. escaped slashes).
                decoded = json.loads(data)
                for kind in detected(json.dumps(decoded, ensure_ascii=False)):
                    if kind not in detected(data.decode('utf-8', errors='replace')):
                        report(kind)
            if path.suffix.lower() == '.png':
                from PIL import Image, ImageStat
                pngs += 1
                with Image.open(io.BytesIO(data)) as image:
                    image.load()
                    for kind in detected(str(image.info)):
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
                    if tesseract:
                        # stdin/stdout only. OCR's temporary images never touch disk.
                        result = subprocess.run([tesseract, 'stdin', 'stdout'], input=data,
                                                capture_output=True, timeout=30, check=False,
                                                env=dict(os.environ, OMP_THREAD_LIMIT='1'))
                        if result.returncode:
                            report('ocr-failed')
                        else:
                            for kind in detected(result.stdout.decode('utf-8', errors='replace')):
                                report('png-text-'+kind)
        except Exception as error:
            findings.append({'file_index': index, 'kind': 'unreadable-or-invalid-artifact',
                             'exception_type': type(error).__name__})
    return {'passed': not findings, 'files': count, 'pngs': pngs,
            'image_audit': 'ocr-and-known-regions' if tesseract else 'known-regions', 'findings': findings}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--coverage', type=Path, help='campaign coverage to invalidate (defaults to RUN/coverage.json)')
    args = parser.parse_args(argv)
    result = scan_run(args.run)
    # Import lazily to keep receipts -> redact free of circular imports.
    from .receipts import fail_coverage, write_json
    receipt = args.run/'receipt.json'
    if receipt.exists():
        value = json.loads(receipt.read_text())
        value['secret_scan'] = result
        if not result['passed']:
            for row in value.get('interactions', []):
                row['functional'] = 'failed'
        write_json(receipt, value)
        if not result['passed']:
            fail_coverage(args.coverage or args.run/'coverage.json', value.get('interactions', []))
    print(json.dumps(result))
    return 0 if result['passed'] else 1


def notification_monitor(path, server=None):
    """Target-side bounded RAM monitor; no notification bodies in a log file."""
    import collections
    import os
    import signal
    import socket
    import subprocess
    import threading
    lines = collections.deque(maxlen=4096)
    lock = threading.Lock()
    filters = ["type='method_call',interface='org.freedesktop.Notifications',member='Notify',arg0='put.gd'",
               "type='method_return',sender='"+(server or 'org.freedesktop.Notifications')+"'"]
    if server:
        filters.append("type='signal',interface='org.freedesktop.Notifications',sender='"+server+"'")
    process = subprocess.Popen(['dbus-monitor', '--session', *filters],
        stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    def read():
        for line in iter(process.stdout.readline, b''):
            with lock:
                lines.append(line[:16384])
    threading.Thread(target=read, daemon=True).start()
    server = socket.socket(socket.AF_UNIX)
    def stop(*_):
        raise SystemExit()
    signal.signal(signal.SIGTERM, stop)
    try:
        server.bind(path)
        os.chmod(path, 0o600)
        server.listen(1)
        while True:
            connection, _ = server.accept()
            with connection:
                with lock:
                    data = b''.join(lines)
                connection.settimeout(3)
                try:
                    connection.sendall(data)
                except OSError:
                    pass
    finally:
        server.close()
        process.terminate()
        process.wait(timeout=3)
        os.unlink(path)


if __name__ == '__main__':
    raise SystemExit(main())
