#!/usr/bin/env python3
"""Verify SDK and vendored source against the committed manifest; never download or build."""
import argparse
import hashlib
import json
import subprocess
from pathlib import Path


def verify_vendored(root: Path, lock: Path) -> None:
    pin = json.loads(lock.read_text(encoding="utf-8")).get("qrCodeGenerator")
    if pin is None:
        if (root / "src/core/qrcodegen").exists():
            raise ValueError("Vendored QR generator has no dependency lock")
        return
    for item in pin["files"]:
        path = root / item["path"]
        if path.is_symlink() or not path.resolve().is_relative_to(root.resolve()) or not path.is_file():
            raise ValueError("Missing or unsafe vendored QR generator file: " + item["path"])
        with path.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        if digest != item["sha256"]:
            raise ValueError("Vendored QR generator differs from dependency-lock.json: " + item["path"])


def verify(source: Path, lock: Path) -> None:
    verify_vendored(lock.resolve().parent.parent, lock)
    expected = json.loads(lock.read_text(encoding="utf-8"))["awsSdk"]
    def git(*args: str) -> str:
        return subprocess.check_output(["git", "-C", str(source), *args], text=True)
    if git("rev-parse", "HEAD").strip() != expected["commit"]:
        raise ValueError("AWS SDK commit differs from dependency-lock.json")
    actual = {}
    for line in git("submodule", "status", "--recursive").splitlines():
        if line[0] != " ":
            raise ValueError("Uninitialized or changed SDK submodule: " + line)
        commit, path, *_ = line.split()
        actual[path] = commit
    pinned = {entry["path"]: entry["commit"] for entry in expected["submodules"]}
    if actual != pinned:
        raise ValueError("SDK submodules differ from dependency-lock.json")
    if git("status", "--porcelain", "--untracked-files=no").strip():
        raise ValueError("SDK checkout has tracked modifications")
    print(f"Verified AWS SDK {expected['release']} and {len(actual)} submodules")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path, nargs="?")
    parser.add_argument("--vendored-only", action="store_true")
    parser.add_argument("--lock", type=Path, default=Path(__file__).resolve().parents[1] / "cmake/dependency-lock.json")
    args = parser.parse_args()
    try:
        if args.vendored_only:
            verify_vendored(args.lock.resolve().parent.parent, args.lock)
        elif args.source is not None:
            verify(args.source, args.lock)
        else:
            parser.error("source is required unless --vendored-only is specified")
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Dependency verification failed: {error}\n")
