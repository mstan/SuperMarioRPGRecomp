#!/usr/bin/env python3
"""Build an address-only SMRPG symbol catalog from Yoshifanatic1's disassembly.

The generated catalog is a local analysis artifact, not a source-code import.
It records labels, source line numbers, and direct label references so live TCP
trace PCs can be correlated with the public disassembly without copying its
assembly bodies into this project.
"""

from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess


SOURCE_URL = (
    "https://github.com/Yoshifanatic1/Super-Mario-RPG-Disassembly"
)
PINNED_COMMIT = "57cb707669d71bb55817a0f88d28b3018c8bec57"
EXPECTED_ROM_MD5 = "d0b68d68d9efc0558242f5476d1c5b81"
SCHEMA = "snesrecomp smrpg public symbol catalog v1"

LABEL_RE = re.compile(
    r"^(?P<name>(?P<kind>CODE|DATA|UNK)_(?P<pc>[C-F][0-9A-F]{5})):\s*$"
)
REFERENCE_RE = re.compile(r"\b(?:CODE|DATA|UNK)_[C-F][0-9A-F]{5}\b")


def file_digest(path: Path, algorithm: str) -> str:
    digest = hashlib.new(algorithm)
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def source_commit(source_root: Path) -> str:
    return subprocess.check_output(
        ["git", "-C", str(source_root), "rev-parse", "HEAD"],
        text=True,
        encoding="utf-8",
    ).strip()


def parse_catalog(source_file: Path) -> tuple[list[dict], list[dict]]:
    labels: list[dict] = []
    references: list[dict] = []
    current_label: str | None = None
    for line_number, line in enumerate(
        source_file.read_text(encoding="utf-8", errors="replace").splitlines(),
        start=1,
    ):
        match = LABEL_RE.match(line)
        if match:
            current_label = match.group("name")
            labels.append(
                {
                    "name": current_label,
                    "pc24": f"0x{int(match.group('pc'), 16):06X}",
                    "kind": match.group("kind").lower(),
                    "line": line_number,
                }
            )
            continue
        if current_label is None:
            continue
        for target in dict.fromkeys(REFERENCE_RE.findall(line)):
            if target != current_label:
                references.append(
                    {
                        "source": current_label,
                        "target": target,
                        "line": line_number,
                    }
                )
    return labels, references


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--source-root",
        required=True,
        type=Path,
        help="checkout of Yoshifanatic1/Super-Mario-RPG-Disassembly",
    )
    parser.add_argument(
        "--rom",
        required=True,
        type=Path,
        help="canonical US SMRPG ROM used by this recomp project",
    )
    parser.add_argument("--output", required=True, type=Path)
    parser.add_argument(
        "--allow-unpinned",
        action="store_true",
        help="allow a source commit other than the reviewed pin",
    )
    args = parser.parse_args()

    source_root = args.source_root.resolve()
    source_file = source_root / "SMRPG" / "Routine_Macros_SMRPG.asm"
    if not source_file.is_file():
        raise FileNotFoundError(source_file)
    commit = source_commit(source_root)
    if commit != PINNED_COMMIT and not args.allow_unpinned:
        raise ValueError(
            f"source commit {commit} does not match pin {PINNED_COMMIT}"
        )

    rom_md5 = file_digest(args.rom.resolve(), "md5")
    if rom_md5.lower() != EXPECTED_ROM_MD5:
        raise ValueError(
            f"ROM MD5 {rom_md5} does not match {EXPECTED_ROM_MD5}"
        )

    labels, references = parse_catalog(source_file)
    label_names = {item["name"] for item in labels}
    references = [
        item for item in references if item["target"] in label_names
    ]
    result = {
        "schema": SCHEMA,
        "source_url": SOURCE_URL,
        "source_commit": commit,
        "source_file": "SMRPG/Routine_Macros_SMRPG.asm",
        "source_sha256": file_digest(source_file, "sha256"),
        "rom_md5": rom_md5,
        "policy": (
            "Address metadata for analysis only; labels are not assumed to be "
            "function entries or AOT-safe boundaries."
        ),
        "labels": labels,
        "direct_references": references,
    }

    args.output.parent.mkdir(parents=True, exist_ok=True)
    temporary = args.output.with_suffix(args.output.suffix + ".tmp")
    with temporary.open("w", encoding="utf-8", newline="\n") as stream:
        json.dump(result, stream, indent=2)
        stream.write("\n")
    temporary.replace(args.output)
    counts = {
        kind: sum(item["kind"] == kind for item in labels)
        for kind in ("code", "data", "unk")
    }
    print(
        f"{args.output}: {len(labels)} labels "
        f"({counts['code']} code, {counts['data']} data, "
        f"{counts['unk']} unknown), {len(references)} references"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
