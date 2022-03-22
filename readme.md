## Experimental project

### Quick how-to

 - Download [custom build of PCSX264](https://github.com/ThirteenAG/pcsx2/suites/5738997033/artifacts/190276266), that is capable of loading **scripts/PCSX2PluginInjector.asi** from this project.

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
        └───4F32A11F-GTAVCS-[SLUS-21590]
                PCSX2PluginDemo.elf
                PCSX2PluginDemo.ini
```

 - Plugins should be placed inside a directory with a name that starts with game **crc**.

 - Plugins must have base address that doesn't conflict with main game and with other plugins (including **PCSX2PluginInvoker.elf**). *This design is WIP, might be changed in the future.*

 - Use **PCSX2PluginInvoker.elf** in combination with **PCSX2PluginDummy.elf** (invoker will not be injected without any plugins) to find out minimum base address for new plugin. After the game is loaded, check memory address of **MallocReturnAddr** symbol (address can be found inside **PCSX2PluginInvoker.map**). *This design is WIP, might be changed in the future.*

 - Plugins are required to have an **ini file** with the same name as elf. Under **[MAIN]** section, add **Malloc** param with an address of ingame malloc function. E.g.:
 
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
*This design is WIP, might be changed in the future.*
 - This ini file is also written to **PluginData** symbol of the injected plugin, e.g. `char PluginData[100] = { 0 };`.

- Demo plugin (and most likely PCSX2PluginInvoker too) is only compatible with **GTAVCS [SLUS-21590]**. It renders few coronas at the beginning of the game:

![](https://i.imgur.com/qYbBtr3.png)

Log example:

```
[19:05:46] [info] Starting PCSX2PluginInjector, game crc: 0x4F32A11F
[19:05:46] [info] EE Memory starts at: 0x7FF6D0000000
[19:05:46] [info] Looking for plugins in pcsx2\bin\scripts\PLUGINS
[19:05:46] [info] Loading PCSX2PluginInvoker.elf
[19:05:46] [info] PCSX2PluginInvoker.elf base address: 0x75E000
[19:05:46] [info] PCSX2PluginInvoker.elf entry point: 0x75E000
[19:05:46] [info] PCSX2PluginInvoker.elf was successfully injected
[19:05:46] [info] Loading PCSX2PluginDemo.elf
[19:05:46] [info] PCSX2PluginDemo.elf base address: 0x760000
[19:05:46] [info] PCSX2PluginDemo.elf entry point: 0x760270
[19:05:46] [info] PCSX2PluginDemo.elf was successfully injected
[19:05:46] [info] Finished injecting plugins, exiting...
```