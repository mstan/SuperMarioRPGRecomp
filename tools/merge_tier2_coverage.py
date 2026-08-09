#!/usr/bin/env python3
"""Merge SNESRecomp tier-2 coverage manifests without losing bail evidence."""

from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import subprocess
import tempfile


SCHEMA = "snesrecomp tier2 coverage v1"
DISCOVERY_SCHEMA = "snesrecomp tier2 discovery v1"


def parse_capture(raw: str, spec: str) -> dict:
    try:
        data = json.loads(raw)
    except json.JSONDecodeError:
        data = None
    if isinstance(data, dict) and data.get("schema") == SCHEMA:
        return data

    records = []
    for line_number, line in enumerate(raw.splitlines(), 1):
        if not line.strip():
            continue
        try:
            item = json.loads(line)
        except json.JSONDecodeError as exc:
            raise ValueError(f"{spec}:{line_number}: torn/invalid JSONL: {exc}") from exc
        if item.get("schema") != DISCOVERY_SCHEMA:
            raise ValueError(
                f"{spec}:{line_number}: unsupported schema {item.get('schema')!r}"
            )
        records.append(item)
    if not records:
        schema = data.get("schema") if isinstance(data, dict) else None
        raise ValueError(f"{spec}: unsupported schema {schema!r}")

    titles = {item.get("rom_title") for item in records}
    if len(titles) != 1:
        raise ValueError(f"{spec}: journal ROM titles differ: {sorted(titles)}")
    discoveries = []
    for item in records:
        record = dict(item)
        for metadata in ("schema", "capture_id", "rom_title"):
            record.pop(metadata, None)
        discoveries.append(record)
    return {
        "schema": SCHEMA,
        "rom_title": titles.pop(),
        "total_tier_hits": sum(
            int(item.get("clean_hits", 0)) + int(item.get("bail_hits", 0))
            for item in discoveries
        ),
        "distinct_sites": len(discoveries),
        "overflowed_tuples": 0,
        "journal_write_failures": 0,
        "discoveries": discoveries,
        "ram_routines_overflow": 0,
        "ram_routines": [],
    }


def load(spec: str) -> dict:
    if spec.startswith("git:"):
        raw = subprocess.check_output(
            ["git", "show", spec.removeprefix("git:")],
            text=True,
            encoding="utf-8",
        )
    else:
        path = Path(spec)
        raw = path.read_text(encoding="utf-8")
    return parse_capture(raw, spec)


def discovery_key(item: dict) -> tuple[str, str, str, str]:
    return (
        item["site_pc24"],
        item["target_pc24"],
        item["entry_mx"],
        item["site_kind"],
    )


def ram_routine_key(item: dict) -> tuple[str, str, str, str]:
    return (
        item["entry_pc24"],
        item["entry_mx"],
        item.get("hash", ""),
        item.get("bytes", ""),
    )


def merge_counter_record(current: dict, incoming: dict) -> None:
    for field in ("clean_hits", "bail_hits", "hits"):
        if field in current or field in incoming:
            current[field] = int(current.get(field, 0)) + int(
                incoming.get(field, 0)
            )
    if "first_frame" in incoming:
        if "first_frame" not in current or incoming["first_frame"] < current["first_frame"]:
            current["first_frame"] = incoming["first_frame"]
            if "first_caller" in incoming:
                current["first_caller"] = incoming["first_caller"]
    if "last_frame" in incoming:
        current["last_frame"] = max(
            int(current.get("last_frame", incoming["last_frame"])),
            int(incoming["last_frame"]),
        )
    if incoming.get("nondeterministic"):
        current["nondeterministic"] = True
    if not incoming.get("terminated", True):
        current["terminated"] = False
    if "outcome_pending" in current or "outcome_pending" in incoming:
        current["outcome_pending"] = bool(current.get("outcome_pending")) and bool(
            incoming.get("outcome_pending")
        )
        if int(current.get("clean_hits", 0)) or int(current.get("bail_hits", 0)):
            current["outcome_pending"] = False


def merge_records(
    destination: list[dict], incoming: list[dict], key_fn
) -> list[dict]:
    merged = [dict(item) for item in destination]
    index = {key_fn(item): item for item in merged}
    for item in incoming:
        key = key_fn(item)
        if key in index:
            merge_counter_record(index[key], item)
        else:
            record = dict(item)
            merged.append(record)
            index[key] = record
    return merged


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("output", type=Path)
    parser.add_argument(
        "inputs",
        nargs="+",
        help=(
            "one artifact per run: prefer its final .json manifest, or use "
            "its .jsonl journal when the manifest is missing; git:<rev>:<path> "
            "is also accepted"
        ),
    )
    parser.add_argument(
        "--unsafe-target",
        action="append",
        default=[],
        help="24-bit PC that failed an AOT semantic qualification",
    )
    parser.add_argument(
        "--qualified-target",
        action="append",
        default=[],
        help="24-bit PC approved by a semantic AOT qualification",
    )
    parser.add_argument(
        "--ram-input",
        action="append",
        default=[],
        help=(
            "input spec whose RAM-routine records are trusted; repeat as "
            "needed (default: accept RAM records from every input)"
        ),
    )
    args = parser.parse_args()

    manifests = [(spec, load(spec)) for spec in args.inputs]
    titles = {manifest.get("rom_title") for _, manifest in manifests}
    if len(titles) != 1:
        raise ValueError(f"ROM titles differ: {sorted(titles)}")

    result = dict(manifests[0][1])
    result["discoveries"] = []
    result["ram_routines"] = []
    result["total_tier_hits"] = 0
    result["overflowed_tuples"] = 0
    result["journal_write_failures"] = 0
    result["ram_routines_overflow"] = 0
    unsafe_targets: set[int] = set()
    qualified_targets: set[int] = set()
    trusted_ram_inputs = set(args.ram_input)
    unknown_ram_inputs = trusted_ram_inputs.difference(args.inputs)
    if unknown_ram_inputs:
        raise ValueError(
            "--ram-input must exactly match an input spec: "
            + ", ".join(sorted(unknown_ram_inputs))
        )
    for spec, manifest in manifests:
        result["total_tier_hits"] += int(manifest.get("total_tier_hits", 0))
        result["overflowed_tuples"] += int(
            manifest.get("overflowed_tuples", 0)
        )
        result["journal_write_failures"] += int(
            manifest.get("journal_write_failures", 0)
        )
        result["discoveries"] = merge_records(
            result["discoveries"],
            manifest.get("discoveries", []),
            discovery_key,
        )
        if not trusted_ram_inputs or spec in trusted_ram_inputs:
            result["ram_routines_overflow"] += int(
                manifest.get("ram_routines_overflow", 0)
            )
            result["ram_routines"] = merge_records(
                result["ram_routines"],
                manifest.get("ram_routines", []),
                ram_routine_key,
            )
        for value in manifest.get("unsafe_aot_targets", []):
            unsafe_targets.add(int(str(value), 0) & 0xFFFFFF)
        for value in manifest.get("qualified_aot_targets", []):
            qualified_targets.add(int(str(value), 0) & 0xFFFFFF)
    for value in args.unsafe_target:
        unsafe_targets.add(int(value, 0) & 0xFFFFFF)
    for value in args.qualified_target:
        qualified_targets.add(int(value, 0) & 0xFFFFFF)
    result["distinct_sites"] = len(result["discoveries"])
    result["unsafe_aot_targets"] = [
        f"0x{target:06X}" for target in sorted(unsafe_targets)
    ]
    result["qualified_aot_targets"] = [
        f"0x{target:06X}" for target in sorted(qualified_targets)
    ]

    args.output.parent.mkdir(parents=True, exist_ok=True)
    fd, temporary = tempfile.mkstemp(
        prefix=args.output.name + ".", suffix=".tmp", dir=args.output.parent
    )
    try:
        with os.fdopen(fd, "w", encoding="utf-8", newline="\n") as stream:
            json.dump(result, stream, indent=2)
            stream.write("\n")
        os.replace(temporary, args.output)
    except BaseException:
        try:
            os.unlink(temporary)
        except FileNotFoundError:
            pass
        raise

    print(
        f"{args.output}: {len(result['discoveries'])} discoveries, "
        f"{len(result['ram_routines'])} RAM routines"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
