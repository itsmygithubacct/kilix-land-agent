#!/usr/bin/env python3
"""Prove the two asset digest sources agree, in both directions.

assets/graphics/manifest.json and assets/graphics/SHA256SUMS both record a
SHA-256 for every graphics asset. Two sources of truth for the same fact drift
silently unless something compares them, and until this check existed nothing
did: manifest.json was not itself covered by any SHA256SUMS entry, so neither
its own corruption nor a disagreement with SHA256SUMS was detectable.

This asserts four separable things, so that a failure names which one broke:

  1. every manifest entry's sha256 matches the actual bytes on disk;
  2. every manifest entry's sha256 matches the SHA256SUMS line for that path;
  3. no manifest path is absent from SHA256SUMS;
  4. no SHA256SUMS path is absent from the manifest.

(3) and (4) are the directions that matter and are the easy ones to omit: a
checker that only walks the manifest validates what is present and never what
is missing. SHA256SUMS additionally covers manifest.json itself, which by
construction cannot be one of its own entries, so that single path is excluded
from (4) by name rather than by a pattern that could swallow a real omission.
"""
from __future__ import annotations

import hashlib
import json
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
MANIFEST = ROOT / "assets" / "graphics" / "manifest.json"
SUMS = ROOT / "assets" / "graphics" / "SHA256SUMS"
SELF_COVERED = "assets/graphics/manifest.json"


def digest(path: Path) -> str:
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main() -> int:
    failures: list[str] = []

    manifest = json.loads(MANIFEST.read_text())
    entries = {a["path"]: a["sha256"] for a in manifest["assets"]}

    sums: dict[str, str] = {}
    for line in SUMS.read_text().splitlines():
        if not line.strip():
            continue
        want, _, path = line.partition("  ")
        sums[path.strip()] = want.strip()

    for path, want in sorted(entries.items()):
        target = ROOT / path
        if not target.exists():
            failures.append(f"manifest names a missing file: {path}")
            continue
        got = digest(target)
        if got != want:
            failures.append(f"manifest sha256 != file bytes for {path}: {want} != {got}")
        if path in sums and sums[path] != want:
            failures.append(
                f"manifest sha256 != SHA256SUMS for {path}: {want} != {sums[path]}"
            )

    missing_from_sums = sorted(set(entries) - set(sums))
    for path in missing_from_sums:
        failures.append(f"manifest path absent from SHA256SUMS: {path}")

    missing_from_manifest = sorted(set(sums) - set(entries) - {SELF_COVERED})
    for path in missing_from_manifest:
        failures.append(f"SHA256SUMS path absent from manifest: {path}")

    if SELF_COVERED not in sums:
        failures.append(f"SHA256SUMS does not cover {SELF_COVERED}")

    if failures:
        for f in failures:
            print(f"FAIL {f}", file=sys.stderr)
        return 1

    print(
        f"PASS asset digest agreement: {len(entries)}/{len(entries)} manifest entries "
        f"match file bytes and SHA256SUMS; {len(sums)}/{len(sums)} SHA256SUMS paths "
        f"accounted for; manifest.json itself covered"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
