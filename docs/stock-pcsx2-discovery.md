# Stock PCSX2: pinned runtime and symbol discovery

The injector's stock backend targets the original upstream source revision used
by the fork. This is development-source support, not a collection of adapters
for old stable releases. Source updates, adapter changes and the matching build
configuration must move together.

The current fork HEAD is `badda91282bb964f8ab116a9f145a2d3774871b6`.
Its upstream merge base is `81526d4dc7cc70e4ae75abb35a789417456c6d43`, tagged
`v2.9.94`. These values are recorded in
`configs/stock-pcsx2/source-target.json`. The uncommitted fork module-runtime
changes are additional fork behavior, not changes to that upstream source base.

## What this checkpoint implements

- A Windows DIA tool that validates the executable's CodeView PDB GUID/age,
  then extracts typed, named symbols and structure/member layouts.
- A bounded PE reader that checks x64/image/section facts and records the
  executable/PDB hashes, function bytes and ABI fingerprints. Addresses are
  module-relative RVAs, independent of ASLR.
- Official executable/symbol archive acquisition with release metadata,
  published digest checks when available, extracted-file hashes, and the
  upstream tag's exact commit. Only the executable and PDB are extracted;
  the emulator is never executed during discovery.
- Generated JSON candidates and Markdown reports. Every candidate currently
  has `runtime_enabled: false`. The ABI reference records facts from the
  source-matched stock build; it does not approve runtime compatibility.
- A separate daily/manual CI workflow. One job analyzes the source-matched
  development release; another reports the latest development release. A
  differing upstream revision is explicitly marked `source-revision-mismatch`.
- A draft PR workflow that updates only generated candidates/reports. It does
  not merge, enable a backend, or synthesize adapter code when the ABI changes.
  Pull-request runs perform discovery/tests without publishing a PR.

For the source-matched `v2.9.94` and newer `v2.9.96`, discovery finds 35 addressed
symbols and 11 record layouts. The requested standalone `VMManager::ClearELFInfo`
symbol is absent in both PDBs; it is optional because ELF loading, reset and
shutdown are independently identified and implemented in the runtime adapter.
No byte-pattern fallback guesses a replacement.

Both development builds have `Throttle(bool)` and the same extracted ABI facts
for the selected roles/layouts. Their code/RVAs and source revisions differ;
matching an ABI fingerprint alone does not authorize a newer build. The user-
provided `v2.8.2` stable build was analyzed only as a historical check, not added
to supported targets. Its reports are in the ignored local build directory.

## Local use on Windows

Visual Studio with C++/ATL and DIA SDK, Python 3.11+, and 7-Zip are required.
No Python packages, PS2SDK build, or emulator process are needed for discovery.

From the injector repository, analyze a local pair:

```powershell
./tools/stock-pcsx2/discover.ps1 `
    -Executable 'C:/path/to/pcsx2-qt.exe' `
    -Pdb 'C:/path/to/pcsx2-qt.pdb' `
    -Version v2.9.94 -Output build/stock-pcsx2/local.json
```

This verifies binary/PDB identity but leaves the source revision unverified
without release provenance. To obtain that provenance from the official pair:

```powershell
python tools/stock-pcsx2/fetch.py --channel development --tag v2.9.94 `
    --destination build/stock-pcsx2/upstream
$pair = Get-Content build/stock-pcsx2/upstream/development.json -Raw | ConvertFrom-Json
./tools/stock-pcsx2/discover.ps1 -Executable $pair.exe -Pdb $pair.pdb `
    -Version $pair.version -Provenance $pair.provenance `
    -Output "configs/stock-pcsx2/candidates/$($pair.version).json"
```

`fetch.py --archive-cache C:/path/to/archives` reuses downloaded archives after
checking them against official release metadata. The user's desktop downloads
and installed emulator directories are read only.

After updating the fork's upstream source base, record its new binding:

```powershell
./tools/stock-pcsx2/bind-source.ps1 -ForkRoot Z:/GitHub/PCSX2-Fork-With-Plugins
```

This reads Git metadata; it does not fetch, switch branches or modify the fork.
It requires an up-to-date upstream ref and an exact published version tag at
the merge base. If no release matches the source revision, stop the discovery
approval rather than using a nearby build. Obtain a binary/PDB pair built from
that exact revision. Regenerate and review the ABI reference when updating the
source target; ordinary discovery runs never overwrite the reference.

## CI and review

`Stock PCSX2 symbol discovery` in Actions runs daily and can be dispatched with
an optional dev tag to compare alongside the source-matched release. Disable
`open_pr` for an artifact/report-only manual run. The workflow must first be
committed/pushed to run on GitHub; no hosted run or PR was created locally.

The repository's Actions settings must allow creating PRs. The default
`GITHUB_TOKEN` is sufficient for the draft PR itself. Optional
`PCSX2_DISCOVERY_TOKEN` can use a repository-scoped token if CI on that generated
PR must also be triggered. The workflow preserves candidates as disabled and
never edits the ABI reference/source target automatically.

Local validation: 32 regression tests passed for the two development pairs,
including all 2048 prefixes of a PE fixture, ambiguity/section checks, complete
release pairing, real PDB GUID/age rejection, and preserving previous output
on rejection. The DIA helper builds with warnings treated as errors. Archive
acquisition/extraction and the PowerShell discovery wrapper ran locally. The
hosted workflow still needs its first GitHub run after publishing.

## Runtime packaging and validation

`StockPCSX2.cpp` supplies the reviewed v2.9.94 adapter. Its locations come from
`data/PCSX2PluginInjector.stock.ini`; typed layout/source fingerprints are compiled
into `StockAbiReference.h`. The executable SHA-256, mapped CodeView GUID/age,
source revision, ABI hash, section bounds/permissions, and ASLR-aware instruction
prefixes must match before hooks are prepared. Hooks are prepared disabled and
activated only after the injector publishes all lifecycle callbacks. Partial
preparation/activation failures roll back. A fork exporting the guest API takes
priority; an incompatible fork does not fall back to guessed hooks.

The backend implements CPU-thread loading, reset/shutdown/ELF transitions,
EI/SYSCALL startup and return, 32 MiB game heap reporting with reserved extended
RAM, private stacks, complete register restoration, throttling, save-state
restriction, input/display metadata, and ImGui OSD. Upstream recompiler call paths
use the hooked interpreter helpers. Original PCSX2 keeps native rain's presentation
fallback; the fork's guest before-UI rendering exports remain available there.
PINE is not used.

Ordinary CI generates `Activation=disabled` profiles. A reviewed exact-source
candidate can be emitted with `emit-runtime-config.py --activate`; the packaged
v2.9.94 profile uses `Activation=source-matched`. This does not enable other builds.
`--test-checkpoint` remains available for local acceptance runs. The JSON reference
and disabled discovery candidates are drift reports, separate from runtime approval.

On 2026-10-05, the original official v2.9.94 executable ran both C probes and the
C++ constructor/heap probe alongside GTAVCS's three existing fixes, guest/native
rain, and CLEO. Startup completed with both EE execution modes. LCS's widescreen
fix, rain, and CLEO also completed startup. These are startup checks, not a claim
of complete gameplay equivalence. Production runtime/PE validation tests and
real-PDB rejection tests supplement them.

The user-requested pinned original download is generated into the injector README and
release-note snippets by `update-download-links.py`. Release jobs insert the
snippet while preserving existing release notes. Use `--check` to catch stale
links and `--fork <checkout>` to update both repos' release snippets after a
source-target change. The fork's upstream README remains untouched.
