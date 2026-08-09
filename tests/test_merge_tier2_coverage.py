import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
MERGER = ROOT / "tools" / "merge_tier2_coverage.py"


def discovery(site, target, kind, clean, bail, pending, capture="run"):
    return {
        "schema": "snesrecomp tier2 discovery v1",
        "capture_id": capture,
        "rom_title": "super_mario_rpg",
        "site_pc24": site,
        "target_pc24": target,
        "entry_mx": "M1X1",
        "site_kind": kind,
        "clean_hits": clean,
        "bail_hits": bail,
        "outcome_pending": pending,
        "first_frame": 10,
        "last_frame": 10,
    }


class MergeTier2CoverageTest(unittest.TestCase):
    def test_manifest_and_append_journal_merge_without_losing_pending_set(self):
        with tempfile.TemporaryDirectory() as td:
            tmp = Path(td)
            base = tmp / "base.json"
            journal = tmp / "capture.jsonl"
            output = tmp / "merged.json"
            base.write_text(
                json.dumps(
                    {
                        "schema": "snesrecomp tier2 coverage v1",
                        "rom_title": "super_mario_rpg",
                        "total_tier_hits": 4,
                        "distinct_sites": 1,
                        "overflowed_tuples": 0,
                        "journal_write_failures": 0,
                        "discoveries": [
                            {
                                "site_pc24": "0xC00001",
                                "target_pc24": "0xC01001",
                                "entry_mx": "M1X1",
                                "site_kind": "call_gap",
                                "clean_hits": 4,
                                "bail_hits": 0,
                                "first_frame": 1,
                                "last_frame": 4,
                            }
                        ],
                        "ram_routines_overflow": 0,
                        "ram_routines": [],
                    }
                ),
                encoding="utf-8",
            )
            records = [
                discovery("0xC00001", "0xC01001", "call_gap", 0, 0, True),
                discovery("0xC00002", "0xC01002", "dispatch", 0, 0, True),
                discovery("0xC00003", "0xC01003", "call_gap", 1, 0, False),
                discovery("0xC00003", "0xC01003", "goto_gap", 0, 1, False),
            ]
            journal.write_text(
                "".join(json.dumps(item) + "\n" for item in records),
                encoding="utf-8",
            )

            subprocess.run(
                [sys.executable, str(MERGER), str(output), str(base), str(journal)],
                check=True,
                cwd=ROOT,
            )
            merged = json.loads(output.read_text(encoding="utf-8"))
            self.assertEqual(merged["distinct_sites"], 4)
            self.assertEqual(merged["overflowed_tuples"], 0)
            self.assertEqual(merged["journal_write_failures"], 0)
            by_key = {
                (item["site_pc24"], item["site_kind"]): item
                for item in merged["discoveries"]
            }
            self.assertEqual(by_key[("0xC00001", "call_gap")]["clean_hits"], 4)
            self.assertFalse(by_key[("0xC00001", "call_gap")]["outcome_pending"])
            self.assertTrue(by_key[("0xC00002", "dispatch")]["outcome_pending"])
            self.assertIn(("0xC00003", "call_gap"), by_key)
            self.assertIn(("0xC00003", "goto_gap"), by_key)

    def test_torn_journal_line_is_rejected(self):
        with tempfile.TemporaryDirectory() as td:
            journal = Path(td) / "torn.jsonl"
            journal.write_text('{"schema":"snesrecomp tier2 discovery v1"', encoding="utf-8")
            result = subprocess.run(
                [sys.executable, str(MERGER), str(Path(td) / "out.json"), str(journal)],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("torn/invalid JSONL", result.stderr)


if __name__ == "__main__":
    unittest.main()
