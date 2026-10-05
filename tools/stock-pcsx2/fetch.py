"""Fetch an official release's matching x64 executable and PDB for discovery."""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import urllib.request

API = "https://api.github.com/repos/PCSX2/pcsx2"


def metadata(endpoint: str):
    headers = {"User-Agent": "PCSX2PluginInjector-symbol-discovery", "Accept": "application/vnd.github+json"}
    token = os.environ.get("GH_TOKEN")
    if token:
        headers["Authorization"] = "Bearer " + token
    with urllib.request.urlopen(urllib.request.Request(API + endpoint, headers=headers), timeout=60) as response:
        return json.load(response)


def pair(release: dict) -> tuple[dict, dict]:
    tag = release["tag_name"]
    if not re.fullmatch(r"v\d+\.\d+\.\d+", tag) or release.get("draft"):
        raise ValueError("Expected a published version tag")
    base = f"pcsx2-{tag}-windows-x64-Qt"
    assets = []
    for name in (base + ".7z", base + "-symbols.7z"):
        matches = [a for a in release["assets"] if a["name"] == name and a["state"] == "uploaded"]
        if len(matches) != 1:
            raise ValueError(f"Missing unique executable/symbol asset: {name}")
        asset = matches[0]
        expected_url = f"https://github.com/PCSX2/pcsx2/releases/download/{tag}/{name}"
        if asset["browser_download_url"] != expected_url or asset["size"] <= 0:
            raise ValueError("Unexpected release asset URL/size")
        assets.append(asset)
    return assets[0], assets[1]


def choose(channel: str, tag: str | None) -> dict:
    if tag:
        if not re.fullmatch(r"v\d+\.\d+\.\d+", tag):
            raise ValueError("Invalid version tag")
        release = metadata("/releases/tags/" + tag)
        pair(release)
        return release
    if channel == "stable":
        release = metadata("/releases/latest")
        if release["prerelease"]:
            raise ValueError("Stable endpoint returned a prerelease")
        pair(release)
        return release
    # Published tags are authoritative, not moving CI artifacts with unmatched
    # symbols. Do not silently fall back to an older release if the newest lacks
    # its pair: report the incomplete publication and retry on the next run.
    releases = metadata("/releases?per_page=100")
    releases = [r for r in releases if r["prerelease"] and not r["draft"]]
    if not releases:
        raise ValueError("No development release found")
    release = max(releases, key=lambda r: r["published_at"])
    pair(release)
    return release


def archive(asset: dict, destination: Path, cache: Path | None) -> dict:
    path = destination / asset["name"]
    cached = cache / asset["name"] if cache else None
    if cached is not None and cached.is_file():
        shutil.copyfile(cached, path)
    else:
        temporary = path.with_suffix(".download")
        request = urllib.request.Request(asset["browser_download_url"], headers={"User-Agent": "PCSX2PluginInjector-symbol-discovery"})
        with urllib.request.urlopen(request, timeout=60) as response, temporary.open("wb") as output:
            shutil.copyfileobj(response, output)
        temporary.replace(path)
    if path.stat().st_size != asset["size"]:
        raise ValueError(f"Archive size mismatch: {path.name}")
    with path.open("rb") as source:
        checksum = hashlib.file_digest(source, "sha256").hexdigest()
    published = asset.get("digest")
    if published and published != "sha256:" + checksum:
        raise ValueError(f"Published archive digest mismatch: {path.name}")
    return dict(name=path.name, url=asset["browser_download_url"], sha256=checksum,
                size=asset["size"], published_digest_verified=bool(published))


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--channel", choices=("stable", "development"), required=True)
    parser.add_argument("--tag")
    parser.add_argument("--destination", type=Path, required=True)
    parser.add_argument("--archive-cache", type=Path, help="Optional local archives, still checked against release metadata")
    parser.add_argument("--seven-zip", default=shutil.which("7z") or r"C:\Program Files\7-Zip\7z.exe")
    args = parser.parse_args()
    try:
        release = choose(args.channel, args.tag)
        tag = release["tag_name"]
        root = args.destination.resolve() / tag
        root.mkdir(parents=True, exist_ok=True)
        assets = pair(release)
        evidence = [archive(asset, root, args.archive_cache) for asset in assets]
        for asset, subdir, name in zip(assets, ("binary", "symbols"), ("pcsx2-qt.exe", "pcsx2-qt.pdb")):
            output = root / subdir
            output.mkdir(exist_ok=True)
            # Extract only the two files needed for analysis. Never run upstream
            # executables or unpack them over the user's emulator installation.
            subprocess.run([args.seven_zip, "x", str(root / asset["name"]), "-o" + str(output), name, "-y"],
                           check=True, stdout=subprocess.DEVNULL)
            if not (output / name).is_file():
                raise ValueError("Expected executable/PDB absent from official archive")
        commit = metadata("/commits/" + tag)["sha"]
        if not re.fullmatch(r"[0-9a-f]{40}", commit):
            raise ValueError("Invalid upstream commit ID")
        inputs = {}
        for role, path in (("exe", root / "binary" / "pcsx2-qt.exe"), ("pdb", root / "symbols" / "pcsx2-qt.pdb")):
            with path.open("rb") as source:
                inputs[role] = hashlib.file_digest(source, "sha256").hexdigest()
        manifest = dict(schema_version=1, channel=args.channel, version=tag, upstream_commit=commit,
                        release_url=release["html_url"], assets=evidence, inputs=inputs)
        (root / "release.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
        index = dict(version=tag, exe=str(root / "binary" / "pcsx2-qt.exe"),
                     pdb=str(root / "symbols" / "pcsx2-qt.pdb"), provenance=str(root / "release.json"))
        (args.destination / f"{args.channel}.json").write_text(json.dumps(index, indent=2) + "\n", encoding="utf-8")
        print(f"{tag}: verified archive pair, extracted executable/PDB to {root}")
        return 0
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Upstream fetch rejected: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
