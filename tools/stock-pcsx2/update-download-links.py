"""Keep public upstream downloads bound to the adapter's source revision."""
import argparse
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
BEGIN, END = "<!-- plugin-upstream:begin -->", "<!-- plugin-upstream:end -->"


def replace(text, block):
    if BEGIN in text:
        if text.count(BEGIN) != 1 or text.count(END) != 1:
            raise ValueError("Ambiguous upstream download section")
        before, rest = text.split(BEGIN, 1)
        _, after = rest.split(END, 1)
        return before + block + after
    heading, separator, rest = text.partition("\n")
    return heading + separator + "\n" + block + "\n" + rest


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--fork", type=Path)
    parser.add_argument("--check", action="store_true")
    args = parser.parse_args()
    target = json.loads((ROOT / "configs/stock-pcsx2/source-target.json").read_text(encoding="utf-8"))
    tag, commit = target["upstream_tag"], target["upstream_commit"]
    if not tag.startswith("v") or any(c not in "v0123456789." for c in tag):
        raise ValueError("Invalid release tag")
    base = "https://github.com/PCSX2/pcsx2/releases/"
    block = f"""{BEGIN}
## Original PCSX2 download for Plugin Injector

The current Windows x64 adapter targets **PCSX2 {tag}**, the upstream development
sources used by this fork (`{commit}`).

- [Download original PCSX2 {tag} for Windows x64]({base}download/{tag}/pcsx2-{tag}-windows-x64-Qt.7z)
- [Release details and optional debugging symbols]({base}tag/{tag})

Use this exact build with the corresponding Plugin Injector package. Other
versions are rejected before installing hooks. This is a pinned development build;
the discovery CI never enables newer builds automatically. Enable **128 MB RAM** in
PCSX2's Advanced settings before using guest plugins.
{END}"""
    outputs = [(ROOT / "README.md", replace((ROOT / "README.md").read_text(encoding="utf-8"), block)),
               (ROOT / "docs/stock-pcsx2-download.md", block + "\n")]
    if args.fork:
        # Keep the fork's upstream README untouched; its notes belong in releases.
        outputs += [(args.fork / "docs/plugin-upstream-download.md", block + "\n")]
    for path, content in outputs:
        if args.check:
            if not path.exists() or path.read_text(encoding="utf-8") != content:
                raise ValueError(f"Source-bound download links need updating: {path}")
        else:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8")


if __name__ == "__main__":
    main()
