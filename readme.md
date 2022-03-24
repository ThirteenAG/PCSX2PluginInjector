## Experimental project

### Quick how-to

 - Download [custom build of PCSX264](https://github.com/ThirteenAG/pcsx2/actions), that is capable of loading **scripts/PCSX2PluginInjector.asi** from this project.

- Compile the project, copy contents of **data** folder to PCSX264 root dir, where **pcsx2x64.exe** is located.

- Directory tree:

```
│   pcsx2x64.exe
│
├───scripts
    │   PCSX2PluginInjector.asi
    │   PCSX2PluginInjector.log
    │
    └───PLUGINS
        │   PCSX2PluginInvoker.elf
        │
        ├───4F32A11F-GTAVCS-[SLUS-21590]
        │       PCSX2PluginDemo.elf
        │       PCSX2PluginDemo.ini
        │
        └───C0498D24-SCDA-[SLUS-21356]
                PCSX2PluginDemo2.elf
                PCSX2PluginDemo2.ini
```

 - Plugins should be placed inside a directory with a name that starts with game **crc**.

 - Plugins must have base address that doesn't conflict with main game and with other plugins (including **PCSX2PluginInvoker.elf**). *This design is WIP, might be changed in the future.*

 - Use **PCSX2PluginInvoker.elf** in combination with **PCSX2PluginDummy.elf** (invoker will not be injected without any plugins) to find out minimum base address for new plugin. After the game is loaded, check memory address returned by malloc inside invoker, or use the address of memory where the game will not write anything (if it's possible to find). *This design is WIP, might be changed in the future.*

 - Plugins are required to have an **ini file** with the same name as elf. Under **[MAIN]** section, add **Malloc** param with an address of ingame malloc function, or set to zero, if malloc is not required for plugin's code to stay intact. E.g.:
 
 ```ini
[MAIN]
Malloc = 0x2BD178
 ```

 Alternatively:

 ```ini
[MAIN]
MallocPatternString = F0 FF BD 27 00 00 B0 FF 08 00 BF FF ? ? ? ? 2D 80 80 00 2D 28 00 02 ? ? ? ? 2D 20 40 00 00 00 B0 DF 08 00 BF DF 08 00 E0 03 10 00 BD 27
MallocPatternIndex = 2
MallocPatternOffset = 0
 ```

- Some games use multiple elf files. To ensure that the plugin will be injected into correct game executable, there's two ini options:

```ini
[MAIN]
MultipleElfs = 1
ElfPattern = 10 00 BF FF 00 00 B0 7F 30 00 A4 AF 40 00 A5 AF
```

- Set `MultipleElfs` parameter to `1` and `ElfPattern` to something unique from target elf. 

 - This ini file is also written to **PluginData** symbol of the injected plugin, e.g. `char PluginData[100] = { 0 };`.

- Demo Plugin 1 is compatible with **GTAVCS [SLUS-21590]**. It renders few coronas at the beginning of the game:

![](https://i.imgur.com/qYbBtr3.png)

- Demo Plugin 2 is for **Splinter Cell Double Agent [SLUS-21356]**, it makes the game stretched via redirecting code to plugin's function:

![](https://i.imgur.com/rBmx5Pc.png)

Log example:

```
[15:20:49] [info] Starting PCSX2PluginInjector, game crc: 0x4F32A11F
[15:20:49] [info] EE Memory starts at: 0x7FF6E0000000
[15:20:49] [info] Game Base Address: 0x100000
[15:20:49] [info] Game Region End: 0x74DF54
[15:20:49] [info] Looking for plugins in pcsx2\bin\scripts\PLUGINS
[15:20:49] [info] Loading PCSX2PluginInvoker.elf
[15:20:49] [info] PCSX2PluginInvoker.elf base address: 0x1FFF000
[15:20:49] [info] PCSX2PluginInvoker.elf entry point: 0x1FFF000
[15:20:49] [info] PCSX2PluginInvoker.elf was successfully injected
[15:20:49] [info] MultipleElfs ini parameter is not set, plugins will not be injected if the game loads another elf
[15:20:49] [info] Loading GTAVCS.PCSX2.WidescreenFix.elf
[15:20:49] [info] GTAVCS.PCSX2.WidescreenFix.elf base address: 0x75E000
[15:20:49] [info] GTAVCS.PCSX2.WidescreenFix.elf entry point: 0x75E278
[15:20:49] [info] GTAVCS.PCSX2.WidescreenFix.elf size: 345052 bytes
[15:20:49] [info] GTAVCS.PCSX2.WidescreenFix.elf was successfully injected
[15:20:49] [info] Finished injecting plugins, exiting...
[15:21:05] [info] Starting PCSX2PluginInjector, game crc: 0xC0498D24
[15:21:05] [info] EE Memory starts at: 0x7FF6E0000000
[15:21:05] [info] Game Base Address: 0x200000
[15:21:05] [info] Game Region End: 0x36D900
[15:21:05] [info] Looking for plugins in pcsx2\bin\scripts\PLUGINS
[15:21:05] [warning] PCSX2PluginDemo2.ini does not contain 'malloc' address in game elf, make sure plugin will not be overwritten in memory
[15:21:05] [info] Loading PCSX2PluginInvoker.elf
[15:21:05] [info] PCSX2PluginInvoker.elf base address: 0x1FFF000
[15:21:05] [info] PCSX2PluginInvoker.elf entry point: 0x1FFF000
[15:21:05] [info] PCSX2PluginInvoker.elf was successfully injected
[15:21:05] [info] MultipleElfs parameter is set, creating thread to handle it
[15:21:05] [info] Loading PCSX2PluginDemo2.elf
[15:21:05] [warning] Pattern "10 00 BF FF 00 00 B0 7F 30 00 A4 AF 40 00 A5 AF" is not found in this elf, PCSX2PluginDemo2.elf will not be loaded
[15:21:05] [info] Finished injecting plugins, exiting...
[15:21:12] [info] Starting PCSX2PluginInjector, game crc: 0xC0498D24
[15:21:12] [info] EE Memory starts at: 0x7FF6E0000000
[15:21:12] [info] Game Base Address: 0x200000
[15:21:12] [info] Game Region End: 0x36D900
[15:21:12] [info] Looking for plugins in pcsx2\bin\scripts\PLUGINS
[15:21:12] [warning] PCSX2PluginDemo2.ini does not contain 'malloc' address in game elf, make sure plugin will not be overwritten in memory
[15:21:12] [info] Loading PCSX2PluginInvoker.elf
[15:21:12] [info] PCSX2PluginInvoker.elf base address: 0x1FFF000
[15:21:12] [info] PCSX2PluginInvoker.elf entry point: 0x1FFF000
[15:21:12] [info] PCSX2PluginInvoker.elf was successfully injected
[15:21:12] [info] MultipleElfs parameter is set, creating thread to handle it
[15:21:12] [info] Loading PCSX2PluginDemo2.elf
[15:21:12] [info] PCSX2PluginDemo2.elf base address: 0x1D87CD8
[15:21:12] [info] PCSX2PluginDemo2.elf entry point: 0x1D87D08
[15:21:12] [info] PCSX2PluginDemo2.elf size: 905 bytes
[15:21:12] [info] PCSX2PluginDemo2.elf was successfully injected
[15:21:12] [info] Finished injecting plugins, exiting...
```