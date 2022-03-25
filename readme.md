## Experimental project

### Quick how-to

 - Download [custom build of PCSX264](https://github.com/ThirteenAG/pcsx2/actions), that is capable of loading **scripts/PCSX2PluginInjector.asi** from this project. (not available at the moment)

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

 - **Enable 128 MB of RAM** option in PCSX2 settings should be set to **on** in order to use plugins.

 - Plugins should be placed inside a directory with a name that starts with game **crc**.

 - Plugins must have base address that doesn't conflict with main game and with other plugins (including **PCSX2PluginInvoker.elf**).

 - To find out minimum base address for new plugin, check the log file. Normally anything higher than **0x2001000** should work.

 - Some games use multiple elf files. To ensure that the plugin will be injected into correct game executable, there's an ini option:

```ini
[MAIN]
ElfPattern = 10 00 BF FF 00 00 B0 7F 30 00 A4 AF 40 00 A5 AF
```

 - Set `ElfPattern` to something unique from target elf. 

 - This ini file is also written to **PluginData** symbol of the injected plugin, e.g. `char PluginData[100] = { 0 };`. Use this to read ini inside the plugin.

- Demo Plugin 1 is compatible with **GTAVCS [SLUS-21590]**. It renders few coronas at the beginning of the game:

![](https://i.imgur.com/qYbBtr3.png)

- Demo Plugin 2 is for **Splinter Cell Double Agent [SLUS-21356]**, it makes the game stretched via redirecting code to plugin's function:

![](https://i.imgur.com/rBmx5Pc.png)

Log example:

```bat
[22:20:27] [info] Starting PCSX2PluginInjector, game crc: 0x4F32A11F
[22:20:27] [info] EE Memory starts at: 0x7FF720000000
[22:20:27] [info] Game Base Address: 0x100000
[22:20:27] [info] Game Region End: 0x74DF54
[22:20:27] [info] Loading PCSX2PluginInvoker.elf
[22:20:27] [info] PCSX2PluginInvoker.elf base address: 0x2000000
[22:20:27] [info] PCSX2PluginInvoker.elf entry point: 0x2000000
[22:20:27] [info] Injecting PCSX2PluginInvoker.elf...
[22:20:27] [info] Finished injecting PCSX2PluginInvoker.elf, 4566 bytes written at 0x2000000
[22:20:27] [info] Hooking game's entry point function...
[22:20:27] [info] Finished hooking entry point function at 0x1DD8C0
[22:20:27] [info] Looking for plugins in scripts
[22:20:27] [info] Loading 4F32A11F-GTAVCS-[SLUS-21590]\GTAVCS.PCSX2.WidescreenFix.elf
[22:20:27] [info] GTAVCS.PCSX2.WidescreenFix.elf base address: 0x2002000
[22:20:27] [info] GTAVCS.PCSX2.WidescreenFix.elf entry point: 0x2002248
[22:20:27] [info] GTAVCS.PCSX2.WidescreenFix.elf size: 700059 bytes
[22:20:27] [info] Loading GTAVCS.PCSX2.WidescreenFix.ini
[22:20:27] [info] Injecting GTAVCS.PCSX2.WidescreenFix.elf...
[22:20:27] [info] Finished injecting GTAVCS.PCSX2.WidescreenFix.elf, 700059 bytes written at 0x2002000
[22:20:27] [info] Injecting GTAVCS.PCSX2.WidescreenFix.ini...
[22:20:27] [info] GTAVCS.PCSX2.WidescreenFix.ini was successfully injected
[22:20:27] [info] Loading 4F32A11F-GTAVCS-[SLUS-21590]\PCSX2PluginDemo.elf
[22:20:27] [info] PCSX2PluginDemo.elf base address: 0x2020000
[22:20:27] [info] PCSX2PluginDemo.elf entry point: 0x2020270
[22:20:27] [info] PCSX2PluginDemo.elf size: 2945 bytes
[22:20:27] [info] Loading PCSX2PluginDemo.ini
[22:20:27] [info] Injecting PCSX2PluginDemo.elf...
[22:20:27] [info] Finished injecting PCSX2PluginDemo.elf, 2945 bytes written at 0x2020000
[22:20:27] [info] Injecting PCSX2PluginDemo.ini...
[22:20:27] [info] PCSX2PluginDemo.ini was successfully injected
[22:20:27] [info] Suggested minimum base address for new plugins: 0x20ACE9B

[22:20:35] [info] Starting PCSX2PluginInjector, game crc: 0xC0498D24
[22:20:35] [info] EE Memory starts at: 0x7FF720000000
[22:20:35] [info] Game Base Address: 0x200000
[22:20:35] [info] Game Region End: 0x36D900
[22:20:35] [info] Loading PCSX2PluginInvoker.elf
[22:20:35] [info] PCSX2PluginInvoker.elf base address: 0x2000000
[22:20:35] [info] PCSX2PluginInvoker.elf entry point: 0x2000000
[22:20:35] [info] Injecting PCSX2PluginInvoker.elf...
[22:20:35] [info] Finished injecting PCSX2PluginInvoker.elf, 4566 bytes written at 0x2000000
[22:20:35] [info] Hooking game's entry point function...
[22:20:36] [info] Finished hooking entry point function at 0x2001F8
[22:20:36] [info] Looking for plugins in scripts
[22:20:36] [info] Loading C0498D24-SCDA-[SLUS-21356]\PCSX2PluginDemo2.elf
[22:20:36] [info] PCSX2PluginDemo2.elf base address: 0x2020000
[22:20:36] [info] PCSX2PluginDemo2.elf entry point: 0x2020030
[22:20:36] [info] PCSX2PluginDemo2.elf size: 905 bytes
[22:20:36] [info] Loading PCSX2PluginDemo2.ini
[22:20:36] [warning] Pattern "10 00 BF FF 00 00 B0 7F 30 00 A4 AF 40 00 A5 AF" is not found in this elf, PCSX2PluginDemo2.elf will not be loaded at this time
[22:20:36] [info] Some plugins has be loaded in another elf, creating thread to handle it
[22:20:36] [info] Suggested minimum base address for new plugins: 0x20011D6

[22:20:41] [info] Starting PCSX2PluginInjector, game crc: 0xC0498D24
[22:20:41] [info] EE Memory starts at: 0x7FF720000000
[22:20:41] [info] Game Base Address: 0x200000
[22:20:41] [info] Game Region End: 0x36D900
[22:20:41] [info] Loading PCSX2PluginInvoker.elf
[22:20:41] [info] PCSX2PluginInvoker.elf base address: 0x2000000
[22:20:41] [info] PCSX2PluginInvoker.elf entry point: 0x2000000
[22:20:41] [info] Injecting PCSX2PluginInvoker.elf...
[22:20:41] [info] Finished injecting PCSX2PluginInvoker.elf, 4566 bytes written at 0x2000000
[22:20:41] [info] Hooking game's entry point function...
[22:20:42] [info] Finished hooking entry point function at 0x1001F8
[22:20:42] [info] Looking for plugins in scripts
[22:20:42] [info] Loading C0498D24-SCDA-[SLUS-21356]\PCSX2PluginDemo2.elf
[22:20:42] [info] PCSX2PluginDemo2.elf base address: 0x2020000
[22:20:42] [info] PCSX2PluginDemo2.elf entry point: 0x2020030
[22:20:42] [info] PCSX2PluginDemo2.elf size: 905 bytes
[22:20:42] [info] Loading PCSX2PluginDemo2.ini
[22:20:42] [info] Injecting PCSX2PluginDemo2.elf...
[22:20:42] [info] Finished injecting PCSX2PluginDemo2.elf, 905 bytes written at 0x2020000
[22:20:42] [info] Suggested minimum base address for new plugins: 0x2020389
```