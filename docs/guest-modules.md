# Relocatable PS2 guest plugins

Plugins are ELF32, little endian MIPS `ET_REL` modules. The injector assigns each
module a base in extended EE RAM; projects never reserve a unique link address.
Old fixed-address `ET_EXEC` binaries are rejected with an update/rebuild warning.
The old invoker and dummy are no longer loaded or packaged.

## Build on Windows

Initialize the updated `external/ps2sdk` submodule. The canonical builder and
runtime live in that SDK's `plugins` directory. No standalone PS2SDK CRT runs.
For an existing plugin, keep its game patches in `init()` and create:

```json
{
  "sources": ["main.c", "other.c"],
  "output": "../../data/MyPlugin/PLUGINS/MyPlugin.elf"
}
```

Paths are relative to `module.json`. Optional `includes`, `defines`, and
`link_options` arrays supply project settings. C++ source is selected by its file
extension; export `extern "C" void init()` and retain C linkage on data exports
such as `CompatibleCRCList`. Sources, objects, and outputs may have spaces in
their paths. The compiler/linker receive normalized Windows paths.

```powershell
./external/ps2sdk/plugins/build-module.ps1 -Project source/MyPlugin/module.json
./external/ps2sdk/plugins/build-module.ps1 -Project source/MyPlugin/module.json -Clean
```

The injector's `tools/build-module.ps1` forwards the same options, adds the API
include directory, and accepts `-SdkRoot`. Every output gets separate `.elf.objects`
intermediates. Failed compilation or unresolved strong imports preserve the last
successful ELF and map. Clean removes only the selected output and intermediates.

The runtime provides a default 64 KiB stack and 1 MiB private heap. Override these
with `MODULE_STACK_SIZE` and `MODULE_HEAP_SIZE` in the project's `defines` array.
Its 16-byte-aligned malloc/calloc/realloc/free and Newlib reentrant allocation
helpers use that heap. C++ constructors run once before `init()`. The profile
supports allocation, strings, vectors, and ordinary exception-disabled STL use;
it disables RTTI, exceptions, and thread-safe static initialization. It does not
boot another kernel, provide general file/thread services, or own the game's heap.
Allocation failure in throwing `new`/STL paths traps rather than unwinding.

Global destructors can be registered with `__cxa_atexit`; reset discards the
module's memory. Automatic unload/finalizer callbacks and state persistence are
future work. Alignment beyond the EE's ordinary 16-byte allocation contract and
TLS are outside this profile.

For freestanding code with its own entry/runtime, use `-NoRuntime` and export
`PCSX2F_MODULE(entry, stack_bytes, heap_bytes)` from `guest_module.h`. The entry
receives a versioned `PCSX2FModuleContext`, including its assigned base, heap
range, and original game GP. See `source/GuestModuleProbe/main.c`.

## Loading and execution

The loader supports REL/RELA forms of R_MIPS_NONE, R_MIPS_32, R_MIPS_26,
R_MIPS_HI16, and R_MIPS_LO16. It handles shared HI16/LO16 pairs and signed jump
addends. Required unresolved symbols, PIC/GOT/GP-relative relocations, TLS, invalid
constructor targets, and unsupported ELF features fail before guest writes.
Undefined weak imports resolve to zero. SDK objects carrying CPIC alone are
accepted when their actual relocations satisfy the absolute-code profile.
Packed data pointers may be unaligned; instruction relocations must be aligned.

The CPU-thread ELF callback writes and queues a complete load transaction. A
successful commit reserves RAM above 32 MiB from game heap/memory-size syscalls.
The emulator must have 128 MiB RAM enabled. BSS, stack, and heap start zeroed.
Execution starts after the existing EI readiness boundary near the game entry,
outside a branch delay slot. Each module gets its own stack and retains game GP
for existing hook helpers. A private return syscall advances to the next module,
then restores the full GPR, HI/LO, FPU, SA, and game PC context. Guest cycles advance.

Existing compatibility lists, INI, display/input, OSD, cheat, and CLEO buffer
exports retain their layouts. Bounds are checked against allocated sections.
INI payloads begin with their actual 32-bit length; oversized input is truncated
with a warning. `GetPluginSymbolAddr` resolves the current relocated module only.
Native consumers must refresh cached addresses across reset and ELF transitions.

Reset, ELF transitions, and shutdown clear registrations and runtime state.
Save/load states remain blocked while modules are active; memory-card saves work
normally. The fork retains guest before-UI rain rendering. Original PCSX2 uses
the native rain plugin's presentation fallback and a stock ImGui OSD adapter.
PINE is not used.

## Verification and guest debugging

`GuestModuleLoaderTests.vcxproj` covers malformed/truncated files, transactional
rejection, relocation arithmetic, constructor validation, packed data pointers,
CPIC, weak imports, BSS, buffers, capacity, and rebasing. Pass `--modules` followed
by rebuilt ELF paths to validate their compiler output at two bases.
The fork's `tests/guest-modules/RuntimeTests.vcxproj` and injector's
`StockRuntimeTests.vcxproj` test their production state machines and register
restoration. The stock test additionally maps the pinned original executable and
checks its hash/PDB identity, symbol bounds, and ASLR-aware instruction prefixes.

`ModuleProbeCpp` exercises static constructors, strings/vectors, aligned calloc,
reallocation/content preservation, overflow rejection, and heap reuse. On
2026-10-05 it and both C probes completed in original v2.9.94, alongside the
GTAVCS fix set and CLEO, with both EE interpreter and recompiler execution. The
user previously confirmed fork probes and reset in GTAVCS and Burnout 3.
These startup checks supplement, rather than establish, full gameplay coverage.

The Windows PCSX2 debugger can inspect guest EE memory/code. The loader log gives
each module's actual base and entry; generated ELF/map files retain debug symbols.
Automatic rebased symbol registration is deferred to the later hooking/tooling
phase. PSP retains its current ABI, waits, and game patches in this migration.

The `includes/pcsx2` and `includes/psp` move to the injector submodule and separate
C/C++ hooking-library redesign follow after this infrastructure migration is
reviewed and committed. They are intentionally outside the current code changes.
