"""Generate review candidates from a Windows x64 PCSX2 executable/PDB pair.

No emulator execution, runtime patching, signature search or third-party Python
packages are involved. The DIA helper must validate the exact CodeView identity.
"""
from __future__ import annotations

import argparse
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import uuid


class DiscoveryError(Exception):
    pass


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


class PE:
    def __init__(self, data: bytes):
        self.data = bytes(data)
        if self.read(0, 2) != b"MZ":
            raise DiscoveryError("Missing DOS header")
        pe = self.number(0x3C, "<I")
        if self.read(pe, 4) != b"PE\0\0":
            raise DiscoveryError("Missing PE header")
        machine, count, self.timestamp = struct.unpack("<HHI", self.read(pe + 4, 8))
        if machine != 0x8664:
            raise DiscoveryError("Only Windows x64 images are supported")
        optional_size = self.number(pe + 20, "<H")
        optional = pe + 24
        if optional_size < 112 + 7 * 8 or self.number(optional, "<H") != 0x20B:
            raise DiscoveryError("Missing PE32+ debug directory")
        self.image_base = self.number(optional + 24, "<Q")
        self.image_size = self.number(optional + 56, "<I")
        if self.number(optional + 108, "<I") < 7:
            raise DiscoveryError("Missing PE debug directory")
        self.sections = []
        for i in range(count):
            start = optional + optional_size + i * 40
            name, virtual_size, rva, raw_size, offset = struct.unpack("<8sIIII", self.read(start, 24))
            flags = self.number(start + 36, "<I")
            self.read(offset, raw_size)
            self.sections.append(dict(name=name.rstrip(b"\0").decode("ascii"), rva=rva,
                                      virtual_size=virtual_size, raw_size=raw_size, offset=offset, flags=flags))
        debug_rva, debug_size = struct.unpack("<II", self.read(optional + 112 + 6 * 8, 8))
        if not debug_size or debug_size % 28:
            raise DiscoveryError("Invalid debug directory")
        entries = self.at_rva(debug_rva, debug_size)
        identities = set()
        for start in range(0, debug_size, 28):
            _, _, _, _, kind, size, _, offset = struct.unpack("<IIHHIIII", entries[start:start + 28])
            if kind != 2:
                continue
            record = self.read(offset, size)
            if len(record) < 25 or record[:4] != b"RSDS" or b"\0" not in record[24:]:
                raise DiscoveryError("Invalid RSDS CodeView record")
            identities.add((str(uuid.UUID(bytes_le=record[4:20])), struct.unpack("<I", record[20:24])[0]))
        if len(identities) != 1:
            raise DiscoveryError("Expected one unambiguous PDB GUID/age")
        self.guid, self.age = identities.pop()

    def read(self, offset: int, size: int) -> bytes:
        if offset < 0 or size < 0 or offset + size > len(self.data):
            raise DiscoveryError("Truncated or out-of-bounds PE data")
        return self.data[offset:offset + size]

    def number(self, offset: int, fmt: str) -> int:
        return struct.unpack(fmt, self.read(offset, struct.calcsize(fmt)))[0]

    def section(self, rva: int, size: int) -> dict:
        matches = [s for s in self.sections if s["rva"] <= rva and
                   rva + size <= s["rva"] + max(s["virtual_size"], s["raw_size"])]
        if len(matches) != 1:
            raise DiscoveryError(f"RVA 0x{rva:x} is outside a unique section")
        if rva + size > self.image_size:
            raise DiscoveryError("Symbol exceeds image bounds")
        return matches[0]

    def at_rva(self, rva: int, size: int) -> bytes:
        section = self.section(rva, size)
        relative = rva - section["rva"]
        if relative + size > section["raw_size"]:
            raise DiscoveryError("RVA range has no file backing")
        return self.read(section["offset"] + relative, size)


def canonical_hash(value) -> str:
    return digest(json.dumps(value, sort_keys=True, separators=(",", ":")).encode())


def select_symbol(pe: PE, spec: dict, query: dict) -> tuple[dict | None, str | None]:
    # Do not pick one overload, folded alias, or duplicate static symbol arbitrarily.
    matches = [m for m in query["matches"] if m["rva"] is not None]
    if len(matches) != 1:
        return None, f"Expected one addressed symbol, found {len(matches)}"
    result = dict(matches[0])
    function = spec["kind"] == "function"
    if result["tag"] != (5 if function else 7) or result["type"] is None:
        return None, "Missing typed function/data symbol"
    extent = result["size"] if function else result["type"]["size"]
    if extent <= 0:
        return None, "Missing nonzero symbol extent"
    section = pe.section(result["rva"], extent)
    if not section["flags"] & 0x40000000:
        return None, "Symbol is not readable"
    if function != bool(section["flags"] & 0x20000000):
        return None, "Function/data section permission mismatch"
    if not function and not section["flags"] & 0x80000000:
        return None, "Runtime data is not writable"
    result["extent"] = extent
    result["section"] = section["name"]
    result["abi_sha256"] = canonical_hash(result["type"])
    if function:
        code = pe.at_rva(result["rva"], extent)
        result["code_sha256"] = digest(code)
        result["entry_bytes"] = code[:min(32, extent)].hex()
    return result, None


def generate(executable: Path, pdb: Path, helper: Path, dia: Path, contract_path: Path, version: str,
             reference_path: Path | None = None, provenance_path: Path | None = None,
             source_target_path: Path | None = None) -> dict:
    pe = PE(executable.read_bytes())
    contract = json.loads(contract_path.read_text(encoding="utf-8"))
    command = [str(helper), str(dia), str(pdb), "{" + pe.guid + "}", str(pe.age)]
    for spec in contract["symbols"]:
        command.extend(["symbol", spec["name"]])
    for spec in contract["layouts"]:
        command.extend(["type", spec["name"]])
    completed = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", check=False)
    if completed.returncode:
        raise DiscoveryError(completed.stderr.strip() or "DIA query failed")
    facts = json.loads(completed.stdout)
    if facts["machine"] != 0x8664:
        raise DiscoveryError("PDB machine does not match x64 PE")
    queries = {(q["kind"], q["name"]): q for q in facts["queries"]}
    issues, symbols, layouts = [], {}, {}
    for spec in contract["symbols"]:
        value, problem = select_symbol(pe, spec, queries["symbol", spec["name"]])
        if problem:
            issues.append(dict(role=spec["role"], required=spec.get("required", True), reason=problem))
        else:
            symbols[spec["role"]] = value
    for spec in contract["layouts"]:
        matches = queries["type", spec["name"]]["matches"]
        if len(matches) != 1 or not matches[0]["size"] or not matches[0].get("members"):
            issues.append(dict(role=spec["name"], required=True, reason=f"Expected one complete record layout, found {len(matches)}"))
            continue
        layout = dict(matches[0])
        del layout["rva"]
        del layout["location"]
        missing = sorted(set(spec["members"]) - {m["name"] for m in layout["members"]})
        if missing:
            issues.append(dict(role=spec["name"], required=True, reason="Missing members: " + ", ".join(missing)))
        layout["abi_sha256"] = canonical_hash(layout)
        layouts[spec["name"]] = layout
    abi_changes = []
    reference_identity = None
    if reference_path is not None:
        reference = json.loads(reference_path.read_text(encoding="utf-8"))
        if reference["contract_sha256"] != canonical_hash(contract):
            raise DiscoveryError("ABI reference uses a different discovery contract; regenerate it for review")
        reference_identity = reference["identity"]
        for group, observed in (("symbols", symbols), ("layouts", layouts)):
            for role, previous in reference[group].items():
                current = observed.get(role, {}).get("abi_sha256")
                if current != previous:
                    abi_changes.append(dict(group=group, role=role, previous=previous, current=current))
    status = "needs-adapter-review"
    if abi_changes:
        status = "abi-review-required"
    if any(i["required"] for i in issues):
        status = "discovery-blocked"
    exe_hash, pdb_hash = digest(pe.data), digest(pdb.read_bytes())
    provenance = None
    if provenance_path is not None:
        provenance = json.loads(provenance_path.read_text(encoding="utf-8"))
        if provenance["version"] != version or provenance["inputs"] != {"exe": exe_hash, "pdb": pdb_hash}:
            raise DiscoveryError("Release provenance does not match the analyzed files/version")
    source_target = None
    source_matches_target = False
    if source_target_path is not None:
        source_target = json.loads(source_target_path.read_text(encoding="utf-8"))
        source_matches_target = bool(provenance and provenance["upstream_commit"] == source_target["upstream_commit"])
        if not source_matches_target and status != "discovery-blocked":
            status = "source-revision-mismatch" if provenance else "source-revision-unverified"
    return dict(schema_version=1, version=version, contract_sha256=canonical_hash(contract),
                source_target=source_target, source_matches_target=source_matches_target,
                provenance=provenance,
                identity=dict(executable=executable.name, sha256=exe_hash, machine="amd64",
                              image_size=pe.image_size, timestamp=pe.timestamp,
                              pdb_guid=pe.guid, pdb_age=pe.age, pdb_sha256=pdb_hash),
                status=status, abi_reference=reference_identity, abi_changes=abi_changes,
                runtime_enabled=False, symbols=symbols, layouts=layouts, issues=issues,
                validation=dict(identity_verified=True, sections_verified=True, emulation_tested=False))


def report(candidate: dict) -> str:
    lines = [f"# PCSX2 {candidate['version']} stock adapter discovery", "",
             f"Status: **{candidate['status']}**. Runtime enabled: **false**.", "",
             "The executable/PDB GUID and age match. Named RVAs and layout facts were read",
             "without executing the emulator. These facts do not establish runtime compatibility.", "",
             f"Executable SHA-256: `{candidate['identity']['sha256']}`", "",
             f"PDB identity: `{candidate['identity']['pdb_guid']}` age {candidate['identity']['pdb_age']}", "",
             "| Role | Symbol | RVA | Extent |", "| --- | --- | --- | --- |"]
    for role, value in candidate["symbols"].items():
        lines.append(f"| {role} | `{value['name']}` | `0x{value['rva']:08x}` | {value['extent']} |")
    lines += ["", "| Layout | Size | ABI SHA-256 |", "| --- | --- | --- |"]
    for name, value in candidate["layouts"].items():
        lines.append(f"| `{name}` | {value['size']} | `{value['abi_sha256']}` |")
    lines += ["", "## Review findings", ""]
    if candidate["source_target"]:
        target = candidate["source_target"]
        lines += [f"Fork source target: `{target['upstream_tag']}` / `{target['upstream_commit']}`.", "",
                  f"Analyzed upstream source matches target: **{str(candidate['source_matches_target']).lower()}**.", ""]
    lines += [f"- {'Required' if i['required'] else 'Optional'} `{i['role']}`: {i['reason']}." for i in candidate["issues"]]
    if not candidate["issues"]:
        lines.append("All requested symbols and layouts were found.")
    if candidate["abi_changes"]:
        lines += ["", "ABI changes relative to the recorded reference:", ""]
        lines += [f"- `{c['group']}.{c['role']}` changed or is missing." for c in candidate["abi_changes"]]
    lines += ["", "The reference is a drift detector, not a compatibility approval. Configuration",
              "layout changes require review even when the guest register layout is unchanged."]
    lines += ["", "## Adapter work before activation", "",
              "- Hook ELF initialization and transitions on the CPU thread, with reset/shutdown cleanup.",
              "- Review EI/SYSCALL boundaries and recompiler call paths for the guest startup/return protocol.",
              "- Reserve extended EE RAM while reporting 32 MiB to game heap/memory syscalls.",
              "- Preserve complete game registers and isolate each module's stack and heap.",
              "- Enforce the current save-state restriction and clear execution caches after loading.",
              "- Test both probes, reset, ELF transitions and both EE execution modes before activation.",
              "", "OSD and before-UI host rendering require separate stock adapters; symbol discovery",
              "does not supply the fork's renderer exports. PINE is not used.", ""]
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, required=True)
    parser.add_argument("--pdb", type=Path, required=True)
    parser.add_argument("--helper", type=Path, required=True)
    parser.add_argument("--dia", type=Path, required=True)
    parser.add_argument("--version", required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--contract", type=Path, default=Path(__file__).with_name("contract.json"))
    parser.add_argument("--reference", type=Path, help="Recorded ABI facts to compare, never runtime approval")
    parser.add_argument("--provenance", type=Path, help="Verified release download manifest")
    parser.add_argument("--source-target", type=Path,
                        default=Path(__file__).resolve().parents[2] / "configs/stock-pcsx2/source-target.json")
    args = parser.parse_args()
    try:
        candidate = generate(args.exe, args.pdb, args.helper, args.dia, args.contract, args.version,
                             args.reference, args.provenance, args.source_target)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        # Write only after identity and all file-bound checks have passed.
        args.output.write_text(json.dumps(candidate, indent=2) + "\n", encoding="utf-8")
        args.output.with_suffix(".md").write_text(report(candidate), encoding="utf-8")
        print(f"{args.version}: {candidate['status']}; {len(candidate['symbols'])} symbols, "
              f"{len(candidate['layouts'])} layouts, {len(candidate['issues'])} review findings")
        return 0
    except (DiscoveryError, OSError, ValueError, KeyError) as error:
        print(f"Discovery rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
