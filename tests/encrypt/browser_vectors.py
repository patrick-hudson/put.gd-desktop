#!/usr/bin/env python3
"""Encrypted-link vectors in real browsers (#2079).

Serves the pinned viewer (src/encrypt/viewer.html) on 127.0.0.1 and opens
tests/encrypt/harness.html in headless Firefox and Chromium. The harness encrypts every
vector with WebCrypto and checks the bytes equal the C++ containers in
tests/fixtures/encrypt/vectors.json, then opens each encrypted page as a recipient would:
inline and split forms, passphrase, wrong key, missing key, changed data, a missing
data file, and a file too large for the browser to read back (saved through the save picker,
or a plain message without one). The server records every request; a page that asks for anything but itself
and, in the split form, its one ./NAME.bin fails the run (the CSP check). Needs only
the Python standard library and the browsers.

    python3 tests/encrypt/browser_vectors.py [--browser firefox|chromium ...] [--timeout S]
"""
import argparse
import base64
import hashlib
import http.server
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
HERE = ROOT / "tests/encrypt"
VIEWER = (ROOT / "src/encrypt/viewer.html").read_bytes()
MARKER = b"<!--putgd-sealed-->"
VECTORS = json.loads((ROOT / "tests/fixtures/encrypt/vectors.json").read_text(encoding="utf-8"))
SPLIT_VECTORS = ("multi", "passphrase")
STATIC = {"/harness.html": ("text/html", HERE / "harness.html"), "/harness.js": ("text/javascript", HERE / "harness.js"),
          "/reference_encrypt.js": ("text/javascript", HERE / "reference_encrypt.js")}


# Test only, placed before the page's CSP: the browser can't read back the file the page puts
# together (as Chrome above about 256 MB), and a save picker records what the page writes.
LARGE = b"""<script>
Blob.prototype.arrayBuffer=function(){return Promise.reject(new DOMException("unreadable","NotReadableError"))};
window.showSaveFilePicker=async o=>{const parts=[];window.savedName=o&&o.suggestedName;
 return {createWritable:async()=>({write:async p=>{parts.push(new Uint8Array(p))},abort:async()=>{},
  close:async()=>{let n=0;for(const x of parts)n+=x.length;const all=new Uint8Array(n);n=0;for(const x of parts){all.set(x,n);n+=x.length}window.saved=all}})}};
</script>"""
NO_PICKER = LARGE.replace(b"window.showSaveFilePicker=async", b"window.showSaveFilePicker=undefined;window.unusedPicker=async")


def page(container: bytes | None, split: bool = False, vid: str = "", before: bytes = b"") -> bytes:
    prefix, suffix = VIEWER.split(MARKER)
    if before:
        prefix = prefix.replace(b"<head>\n", b"<head>\n" + before + b"\n", 1)
    if split:
        prefix = prefix.replace(b"connect-src 'none'", b"connect-src 'self'")
        element = b'<script id="putgd-sealed" type="application/octet-stream" data-form="split" data-bin="' + vid.encode() + b'.bin"></script>'
    else:
        element = b'<script id="putgd-sealed" type="application/octet-stream" data-form="inline">' + base64.b64encode(container) + b"</script>"
    return prefix + element + suffix


class Run:
    def __init__(self):
        self.containers = {v["id"]: base64.b64decode(v["container"]) for v in VECTORS["vectors"] if v["container"]}
        self.requests = []
        self.result = None
        self.done = threading.Event()


def handler(run: Run):
    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def send(self, status, body=b"", kind="text/plain"):
            self.send_response(status)
            self.send_header("Content-Type", kind)
            self.send_header("Content-Length", str(len(body)))
            self.send_header("Cache-Control", "no-store")
            self.end_headers()
            self.wfile.write(body)

        def do_GET(self):
            path = self.path.split("?")[0]
            run.requests.append(path)
            if path in STATIC:
                kind, file = STATIC[path]
                return self.send(200, file.read_bytes(), kind)
            if path == "/vectors.json":
                return self.send(200, json.dumps(VECTORS).encode(), "application/json")
            match = re.fullmatch(r"/(large|large-nosave)/(\w+)\.html", path)
            if match and match.group(2) in run.containers:
                form, vid = match.groups()
                return self.send(200, page(None, True, vid, LARGE if form == "large" else NO_PICKER), "text/html")
            match = re.fullmatch(r"/(large|large-nosave)/(\w+)\.bin", path)
            if match and match.group(2) in run.containers:
                return self.send(200, run.containers[match.group(2)], "application/octet-stream")
            match = re.fullmatch(r"/(page|split|tampered|split-missing)/(\w+)\.html", path)
            if match and match.group(2) in run.containers:
                form, vid = match.groups()
                if form == "tampered":
                    data = bytearray(run.containers[vid])
                    data[len(data) // 2] ^= 1
                    return self.send(200, page(bytes(data)), "text/html")
                return self.send(200, page(run.containers[vid], form in ("split", "split-missing"), vid), "text/html")
            match = re.fullmatch(r"/split/(\w+)\.bin", path)
            if match and match.group(1) in run.containers:
                return self.send(200, run.containers[match.group(1)], "application/octet-stream")
            return self.send(404)

        def do_POST(self):
            path = self.path
            run.requests.append(path)
            body = self.rfile.read(int(self.headers.get("Content-Length", "0")))
            match = re.fullmatch(r"/container/(\w+)", path)
            if match:
                run.containers[match.group(1)] = base64.b64decode(body)
                return self.send(204)
            if path == "/result":
                run.result = json.loads(body)
                self.send(204)
                run.done.set()
                return
            return self.send(404)
    return Handler


def browser_command(name: str, url: str, profile: str) -> list[str] | None:
    if name == "firefox":
        exe = shutil.which("firefox")
        return [exe, "--headless", "--no-remote", "--profile", profile, url] if exe else None
    for candidate in ("chromium", "chromium-browser", "google-chrome", "google-chrome-stable"):
        exe = shutil.which(candidate)
        if exe:
            # Ubuntu 24.04's AppArmor refuses Chrome's sandbox to an unprivileged CI user (and root needs it off too).
            sandbox = "--no-sandbox" if os.geteuid() == 0 or os.environ.get("CI") else "--disable-sync"
            return [exe, "--headless=new", f"--user-data-dir={profile}", "--no-first-run", "--no-default-browser-check",
                    "--disable-gpu", "--disable-extensions", "--disable-dev-shm-usage", sandbox, url]
    return None


ALLOWED = re.compile(r"^/(harness\.html|harness\.js|reference_encrypt\.js|vectors\.json|result|container/\w+|"
                     r"(page|split|tampered|split-missing|large|large-nosave)/\w+\.html|(split|split-missing|large|large-nosave)/\w+\.bin)$")


def run_browser(name: str, timeout: float) -> list[str]:
    run = Run()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", 0), handler(run))
    threading.Thread(target=server.serve_forever, daemon=True).start()
    url = f"http://127.0.0.1:{server.server_address[1]}/harness.html"
    problems = []
    with tempfile.TemporaryDirectory(prefix="putgd-encrypt-") as profile:
        command = browser_command(name, url, profile)
        if not command:
            server.shutdown()
            return [f"{name}: not installed"]
        log = open(os.path.join(profile, "browser.log"), "w+b")
        process = subprocess.Popen(command, stdout=log, stderr=subprocess.STDOUT)
        try:
            if not run.done.wait(timeout):
                problems.append(f"{name}: no result within {timeout:.0f} s")
                log.flush(); log.seek(0)
                print(f"{name} output (last lines):", *log.read().decode(errors="replace").splitlines()[-20:], sep="\n  ")
                print(f"{name} requests: {run.requests[-10:]}")
        finally:
            process.terminate()
            try:
                process.wait(10)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            server.shutdown()
            log.close()
    result = run.result or {"results": [], "userAgent": "?"}
    print(f"{name}: {result['userAgent']}")
    for entry in result["results"]:
        mark = "ok  " if entry["ok"] else "FAIL"
        print(f"  {mark} {entry['id']}: {entry['check']}" + ("" if entry["ok"] else f" ({entry['detail'][:300]})"))
        if not entry["ok"]:
            problems.append(f"{name}: {entry['id']}: {entry['check']}")
    if run.result and len(result["results"]) < 40:
        problems.append(f"{name}: only {len(result['results'])} checks ran")
    # The CSP check: nothing but the harness, the pages and the split page's one data file.
    for path in run.requests:
        if not ALLOWED.match(path):
            problems.append(f"{name}: unexpected request {path}")
    for vid in SPLIT_VECTORS:
        bins = run.requests.count(f"/split/{vid}.bin")
        if run.result and bins != 1:
            problems.append(f"{name}: the {vid} split page asked for its data {bins} times, not once")
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--browser", action="append", choices=["firefox", "chromium"])
    parser.add_argument("--timeout", type=float, default=240)
    parser.add_argument("--allow-missing", action="store_true", help="skip a browser that isn't installed")
    options = parser.parse_args()
    digest = hashlib.sha256(VIEWER).hexdigest()
    print(f"viewer sha256 {digest}")
    problems = []
    for name in options.browser or ["firefox", "chromium"]:
        found = run_browser(name, options.timeout)
        if options.allow_missing:
            found = [p for p in found if not p.endswith("not installed")]
        problems += found
    for problem in problems:
        print(problem, file=sys.stderr)
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
