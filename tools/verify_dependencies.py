#!/usr/bin/env python3
"""Verify the SDK checkout against the committed manifest; never download or build."""
import argparse
import json
import subprocess
from pathlib import Path


def verify(source: Path, lock: Path) -> None:
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
    parser.add_argument("source", type=Path)
    parser.add_argument("--lock", type=Path, default=Path(__file__).resolve().parents[1] / "cmake/dependency-lock.json")
    args = parser.parse_args()
    try:
        verify(args.source, args.lock)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Dependency verification failed: {error}\n")
