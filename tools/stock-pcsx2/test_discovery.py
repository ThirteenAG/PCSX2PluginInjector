"""PE/selection regression tests; optional read-only DIA integration tests."""
import argparse
import copy
import json
from pathlib import Path
import struct
import subprocess
import tempfile
import unittest
import uuid

from discover import DiscoveryError, PE, canonical_hash, generate, select_symbol
from fetch import pair

PAIR = None
DIA = None
HELPER = None


def image() -> bytearray:
    data = bytearray(0x800)
    data[:2] = b"MZ"
    struct.pack_into("<I", data, 0x3C, 0x80)
    data[0x80:0x84] = b"PE\0\0"
    struct.pack_into("<HHI", data, 0x84, 0x8664, 2, 12345)
    struct.pack_into("<H", data, 0x94, 240)
    struct.pack_into("<H", data, 0x98, 0x20B)
    struct.pack_into("<Q", data, 0x98 + 24, 0x140000000)
    struct.pack_into("<I", data, 0x98 + 56, 0x4000)
    struct.pack_into("<I", data, 0x98 + 108, 16)
    struct.pack_into("<II", data, 0x98 + 112 + 6 * 8, 0x1080, 28)
    for index, (name, rva, raw, flags) in enumerate(((b".text", 0x1000, 0x200, 0x60000020),
                                                   (b".data", 0x2000, 0x600, 0xC0000040))):
        start = 0x98 + 240 + 40 * index
        struct.pack_into("<8sIIII", data, start, name, 0x400, rva, 0x200, raw)
        struct.pack_into("<I", data, start + 36, flags)
    record = b"RSDS" + uuid.UUID("00112233-4455-6677-8899-aabbccddeeff").bytes_le + struct.pack("<I", 1) + b"pcsx2.pdb\0"
    struct.pack_into("<IIHHIIII", data, 0x280, 0, 0, 0, 0, 2, len(record), 0x1100, 0x300)
    data[0x300:0x300 + len(record)] = record
    return data


class PETests(unittest.TestCase):
    def test_identity(self):
        pe = PE(image())
        self.assertEqual(pe.guid, "00112233-4455-6677-8899-aabbccddeeff")
        self.assertEqual(pe.age, 1)
        self.assertEqual(pe.at_rva(0x1100, 4), b"RSDS")

    def test_all_truncations(self):
        data = image()
        for size in range(len(data)):
            with self.subTest(size=size), self.assertRaises(DiscoveryError):
                PE(data[:size])

    def reject_mutation(self, offset, value):
        data = image()
        data[offset:offset + len(value)] = value
        with self.assertRaises(DiscoveryError):
            PE(data)

    def test_bad_dos(self):
        self.reject_mutation(0, b"XX")

    def test_bad_pe_pointer(self):
        self.reject_mutation(0x3C, struct.pack("<I", 0xFFFFFF00))

    def test_wrong_machine(self):
        self.reject_mutation(0x84, struct.pack("<H", 0x14C))

    def test_wrong_optional_magic(self):
        self.reject_mutation(0x98, struct.pack("<H", 0x10B))

    def test_no_directory(self):
        self.reject_mutation(0x98 + 108, struct.pack("<I", 6))

    def test_bad_directory_size(self):
        self.reject_mutation(0x98 + 112 + 6 * 8 + 4, struct.pack("<I", 27))

    def test_bad_codeview(self):
        self.reject_mutation(0x300, b"NB10")

    def test_no_pdb_path_terminator(self):
        self.reject_mutation(0x318, b"x" * 10)

    def test_codeview_outside_file(self):
        self.reject_mutation(0x280 + 24, struct.pack("<I", 0xFFFF0000))

    def test_missing_codeview(self):
        self.reject_mutation(0x280 + 12, struct.pack("<I", 0))

    def test_bss_not_file_backed(self):
        with self.assertRaises(DiscoveryError):
            PE(image()).at_rva(0x2300, 1)

    def test_overlapping_sections(self):
        data = image()
        struct.pack_into("<I", data, 0x98 + 240 + 40 + 12, 0x1000)
        with self.assertRaises(DiscoveryError):
            PE(data)

    def test_image_extent(self):
        pe = PE(image())
        pe.image_size = 0x1100
        with self.assertRaises(DiscoveryError):
            pe.section(0x1100, 4)


class SymbolTests(unittest.TestCase):
    def setUp(self):
        self.pe = PE(image())
        self.spec = {"kind": "function"}
        self.query = {"matches": [{"name": "function", "tag": 5, "rva": 0x1000,
                                   "size": 64, "location": 1, "type": {"tag": 13, "size": 0}}]}

    def test_function_bytes(self):
        selected, issue = select_symbol(self.pe, self.spec, self.query)
        self.assertIsNone(issue)
        self.assertEqual(selected["extent"], 64)
        self.assertEqual(len(selected["entry_bytes"]), 64)
        self.assertEqual(len(selected["code_sha256"]), 64)

    def test_duplicate_rejected(self):
        self.query["matches"] *= 2
        self.assertIsNone(select_symbol(self.pe, self.spec, self.query)[0])

    def test_missing_rejected(self):
        self.query["matches"] = []
        self.assertIsNone(select_symbol(self.pe, self.spec, self.query)[0])

    def test_inlined_rejected(self):
        self.query["matches"][0]["rva"] = None
        self.assertIsNone(select_symbol(self.pe, self.spec, self.query)[0])

    def test_wrong_tag(self):
        self.query["matches"][0]["tag"] = 7
        self.assertIsNone(select_symbol(self.pe, self.spec, self.query)[0])

    def test_wrong_section(self):
        self.query["matches"][0]["rva"] = 0x2000
        self.assertIsNone(select_symbol(self.pe, self.spec, self.query)[0])

    def test_data_extent_uses_type(self):
        value = self.query["matches"][0]
        value.update(tag=7, rva=0x2000, size=0, type={"tag": 16, "size": 8})
        selected, issue = select_symbol(self.pe, {"kind": "data"}, self.query)
        self.assertIsNone(issue)
        self.assertEqual(selected["extent"], 8)
        self.assertNotIn("entry_bytes", selected)

    def test_zero_length(self):
        self.query["matches"][0]["size"] = 0
        self.assertIsNone(select_symbol(self.pe, self.spec, self.query)[0])

    def test_hash_stable(self):
        self.assertEqual(canonical_hash({"a": 1, "b": 2}), canonical_hash({"b": 2, "a": 1}))
        self.assertNotEqual(canonical_hash({"a": 1}), canonical_hash({"a": 2}))


class ReleaseTests(unittest.TestCase):
    def setUp(self):
        self.release = {"tag_name": "v2.8.2", "draft": False, "assets": []}
        for suffix in (".7z", "-symbols.7z"):
            name = "pcsx2-v2.8.2-windows-x64-Qt" + suffix
            self.release["assets"].append(dict(name=name, state="uploaded", size=100,
                browser_download_url="https://github.com/PCSX2/pcsx2/releases/download/v2.8.2/" + name))

    def test_complete_pair(self):
        self.assertEqual(len(pair(self.release)), 2)

    def test_missing_symbols(self):
        self.release["assets"].pop()
        with self.assertRaises(ValueError):
            pair(self.release)

    def test_duplicate_assets(self):
        self.release["assets"].append(copy.deepcopy(self.release["assets"][0]))
        with self.assertRaises(ValueError):
            pair(self.release)

    def test_wrong_origin(self):
        self.release["assets"][0]["browser_download_url"] = "https://example.org/plugin.exe"
        with self.assertRaises(ValueError):
            pair(self.release)

    def test_path_tag(self):
        self.release["tag_name"] = "../../outside"
        with self.assertRaises(ValueError):
            pair(self.release)


class DIATests(unittest.TestCase):
    def setUp(self):
        if PAIR is None:
            self.skipTest("Pass --pair, --dia and --helper for read-only DIA integration")

    def test_real_pair(self):
        root = Path(__file__).resolve().parents[2]
        result = generate(Path(PAIR["exe"]), Path(PAIR["pdb"]), HELPER, DIA,
                          Path(__file__).with_name("contract.json"), PAIR["version"],
                          root / "configs/stock-pcsx2/reference-abi.json", Path(PAIR["provenance"]),
                          root / "configs/stock-pcsx2/source-target.json")
        self.assertFalse(result["runtime_enabled"])
        self.assertTrue(result["validation"]["identity_verified"])
        self.assertEqual(result["source_matches_target"],
                         result["provenance"]["upstream_commit"] == result["source_target"]["upstream_commit"])
        # ABI drift/missing symbols are useful review reports, not parser test
        # failures. Let future CI runs publish their blocked-candidate findings.
        if any(i["required"] for i in result["issues"]):
            self.assertEqual(result["status"], "discovery-blocked")
        elif not result["source_matches_target"]:
            self.assertEqual(result["status"], "source-revision-mismatch")
        for symbol in result["symbols"].values():
            self.assertGreater(symbol["rva"], 0)
            self.assertGreater(symbol["extent"], 0)

    def test_wrong_guid_and_age(self):
        pe = PE(Path(PAIR["exe"]).read_bytes())
        for guid, age in ((str(uuid.uuid4()), pe.age), (pe.guid, pe.age + 1)):
            process = subprocess.run([str(HELPER), str(DIA), PAIR["pdb"], "{" + guid + "}", str(age),
                                      "symbol", "_cpuRegistersPack"], capture_output=True, text=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(process.stdout, "")
            self.assertIn("validate PDB GUID/age", process.stderr)

    def test_failure_preserves_previous_output(self):
        with tempfile.TemporaryDirectory() as temporary:
            output = Path(temporary) / "candidate.json"
            output.write_text("previous candidate")
            bad_exe = Path(temporary) / "bad.exe"
            bad_exe.write_bytes(b"MZ")
            process = subprocess.run(["python", str(Path(__file__).with_name("discover.py")), "--exe", str(bad_exe),
                "--pdb", PAIR["pdb"], "--helper", str(HELPER), "--dia", str(DIA), "--version", PAIR["version"],
                "--output", str(output)], capture_output=True, text=True)
            self.assertNotEqual(process.returncode, 0)
            self.assertEqual(output.read_text(), "previous candidate")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pair", type=Path)
    parser.add_argument("--dia", type=Path)
    parser.add_argument("--helper", type=Path)
    args, remaining = parser.parse_known_args()
    if args.pair:
        if not args.dia or not args.helper:
            parser.error("--pair requires --dia and --helper")
        PAIR = json.loads(args.pair.read_text())
        DIA, HELPER = args.dia, args.helper
    unittest.main(argv=[__file__] + remaining)
