#!/usr/bin/env python3
"""Pin the sealed-link viewer page (#2079).

The viewer's Content-Security-Policy allows its one inline script and one inline
style by SHA-256 only. After an edit to either, this rewrites the two hashes in
src/seal/viewer.html, writes the page's own SHA-256 into src/seal/SealPage.cpp
(viewerSha256) and prints it for the design document. `--check` changes
nothing and fails when a hash is out of date; CI runs it.
"""
import argparse
import base64
import hashlib
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
VIEWER = ROOT / "src/seal/viewer.html"
PAGE = ROOT / "src/seal/SealPage.cpp"


def csp_hash(text: str) -> str:
    return base64.b64encode(hashlib.sha256(text.encode("utf-8")).digest()).decode()


# The one policy the viewer may carry; only the two hashes change.
CSP = re.compile(r'<meta http-equiv="Content-Security-Policy" content="default-src \'none\'; script-src \'sha256-[A-Za-z0-9+/=]*\'; '
                 r'style-src \'sha256-[A-Za-z0-9+/=]*\'; img-src blob:; media-src blob:; connect-src \'none\'; base-uri \'none\'; form-action \'none\'">')


def pinned(page: str) -> str:
    script = re.search(r"<script>(.*?)</script>", page, re.S)
    style = re.search(r"<style>(.*?)</style>", page, re.S)
    if not script or not style or page.count("<script") != 1 or page.count("<style") != 1 or "<link rel=\"stylesheet" in page:
        raise SystemExit("viewer.html needs exactly one inline <script> and one <style>")
    if len(CSP.findall(page)) != 1 or page.count("Content-Security-Policy") != 1:
        raise SystemExit("viewer.html's CSP must be exactly the pinned policy (see CSP in tools/seal/pin_viewer.py)")
    if page.count("<!--putgd-sealed-->") != 1 or re.search(r"https?://", page):
        raise SystemExit("viewer.html needs one <!--putgd-sealed--> marker and no outside address")
    page = re.sub(r"script-src 'sha256-[^']*'", f"script-src 'sha256-{csp_hash(script.group(1))}'", page, count=1)
    return re.sub(r"style-src 'sha256-[^']*'", f"style-src 'sha256-{csp_hash(style.group(1))}'", page, count=1)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--check", action="store_true")
    options = parser.parse_args()
    current = VIEWER.read_text(encoding="utf-8")
    wanted = pinned(current)
    digest = hashlib.sha256(wanted.encode("utf-8")).hexdigest()
    recorded = re.search(r'viewerSha256\[\]="([0-9a-f]{64})"', PAGE.read_text(encoding="utf-8"))
    if options.check:
        problems = []
        if wanted != current:
            problems.append("the CSP hashes in src/seal/viewer.html are out of date: run tools/seal/pin_viewer.py")
        if not recorded or recorded.group(1) != digest:
            problems.append(f"src/seal/SealPage.cpp viewerSha256 must be {digest}")
        for problem in problems:
            print(problem, file=sys.stderr)
        return 1 if problems else 0
    VIEWER.write_text(wanted, encoding="utf-8")
    source = PAGE.read_text(encoding="utf-8")
    PAGE.write_text(re.sub(r'viewerSha256\[\]="[0-9a-f]{64}"', f'viewerSha256[]="{digest}"', source, count=1), encoding="utf-8")
    print(digest)
    return 0


if __name__ == "__main__":
    sys.exit(main())
