#!/usr/bin/env python3
"""Inspect an SMRPG coverage proof bundle folder or zip."""

from __future__ import annotations

import argparse
import json
from pathlib import Path
import sys
import zipfile


MANIFEST_SCHEMA = "snesrecomp tier2 coverage v1"
SUMMARY_SCHEMA = "smrpg coverage proof bundle v1"


class Bundle:
    def __init__(self, path: Path):
        self.path = path
        self.zip: zipfile.ZipFile | None = None
        if path.is_file() and path.suffix.lower() == ".zip":
            self.zip = zipfile.ZipFile(path)
            self.names = [n for n in self.zip.namelist() if not n.endswith("/")]
        elif path.is_dir():
            self.names = [
                str(p.relative_to(path)).replace("\\", "/")
                for p in path.rglob("*")
                if p.is_file()
            ]
        else:
            raise ValueError(f"{path} is not a folder or .zip bundle")

    def close(self) -> None:
        if self.zip:
            self.zip.close()

    def find(self, basename: str) -> str | None:
        matches = [n for n in self.names if Path(n).name == basename]
        return sorted(matches)[0] if matches else None

    def find_suffix(self, suffix: str) -> str | None:
        matches = [n for n in self.names if n.endswith(suffix)]
        return sorted(matches)[0] if matches else None

    def read_text(self, name: str) -> str:
        if self.zip:
            return self.zip.read(name).decode("utf-8-sig")
        return (self.path / name).read_text(encoding="utf-8-sig")


def load_json(bundle: Bundle, name: str) -> dict:
    try:
        data = json.loads(bundle.read_text(name))
    except json.JSONDecodeError as exc:
        raise ValueError(f"{name}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise ValueError(f"{name}: expected JSON object")
    return data


def inspect(path: Path) -> int:
    bundle = Bundle(path)
    try:
        summary_name = bundle.find("coverage_proof_summary.json")
        manifest_name = bundle.find("tier2_super_mario_rpg.json")
        journal_name = bundle.find("tier2_super_mario_rpg.jsonl")
        if not manifest_name:
            manifest_name = bundle.find_suffix(".json")

        if not manifest_name:
            raise ValueError("bundle is missing a tier2 manifest")

        manifest = load_json(bundle, manifest_name)
        if manifest.get("schema") != MANIFEST_SCHEMA:
            raise ValueError(
                f"{manifest_name}: unsupported schema {manifest.get('schema')!r}"
            )
        summary = load_json(bundle, summary_name) if summary_name else None
        if summary and summary.get("schema") != SUMMARY_SCHEMA:
            raise ValueError(
                f"{summary_name}: unsupported schema {summary.get('schema')!r}"
            )

        discoveries = manifest.get("discoveries", [])
        ram_routines = manifest.get("ram_routines", [])
        pending = 0
        clean = 0
        bailed = 0
        clean_call_gap_targets: set[str] = set()
        for item in discoveries:
            clean_hits = int(item.get("clean_hits", item.get("hits", 0)) or 0)
            bail_hits = int(item.get("bail_hits", 0) or 0)
            if item.get("outcome_pending"):
                pending += 1
            if clean_hits:
                clean += 1
            if bail_hits:
                bailed += 1
            if (
                item.get("site_kind") == "call_gap"
                and clean_hits > 0
                and bail_hits == 0
                and not item.get("outcome_pending")
            ):
                clean_call_gap_targets.add(str(item.get("target_pc24")))

        saves = [n for n in bundle.names if n.startswith("saves/")]
        journal_lines = 0
        if journal_name:
            journal_lines = sum(
                1 for line in bundle.read_text(journal_name).splitlines() if line.strip()
            )

        print(f"bundle: {path}")
        print(f"summary: {'yes' if summary else 'no'}")
        print(f"manifest: {manifest_name}")
        print(f"journal: {journal_name or 'missing'} ({journal_lines} records)")
        print(f"saves: {len(saves)} file(s)")
        print(f"discoveries: {len(discoveries)} total")
        print(f"observed clean: {clean}")
        print(f"observed with bail evidence: {bailed}")
        print(f"still pending outcome: {pending}")
        print(f"clean call-gap targets: {len(clean_call_gap_targets)}")
        print(f"ram routines: {len(ram_routines)}")
        if summary:
            print(
                "run stats: "
                f"{summary.get('frames', 0)} frames, "
                f"{summary.get('distinct_sites', 0)} distinct sites, "
                f"{summary.get('clean_hits', 0)} clean hits"
            )
        return 0
    finally:
        bundle.close()


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("bundle", type=Path, help="coverage bundle folder or .zip")
    args = parser.parse_args()
    try:
        return inspect(args.bundle)
    except Exception as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
