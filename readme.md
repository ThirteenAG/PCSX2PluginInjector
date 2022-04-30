# PCSX2 Fork Plugin Injector

## Using

 - Download [PCSX2 Fork With Plugins](https://github.com/ASI-Factory/PCSX2-Fork-With-Plugins/releases/tag/latest), this project is already included in it. (**Windows only**).

 - Copy **.elf** plugins to **PLUGINS** directory, e.g. **[GTAVCS.PCSX2.WidescreenFix.elf](https://thirteenag.github.io/wfp#gtavcs)**.

## Limitations

 - Save states between regular PCSX2 version and the fork will not be compatible.

 - Do not open issues in PCSX2 repository when using the fork. Reproduce them in regular PCSX2 build first.

## Plugin development how-to 

 - Compile **PCSX2PluginInjector** solution, copy contents of **data** folder to PCSX264 root dir, where **pcsx2x64.exe** is located, or use [PCSX2PluginInjectorDemo.zip](https://github.com/ThirteenAG/PCSX2PluginInjector/releases/download/latest/PCSX2PluginInjectorDemo.zip) with all necessary files.

 - Directory tree:

```
│   pcsx2x64.exe
│   PCSX2PluginInjector.asi
│   PCSX2PluginInjector.log
│
└───PLUGINS
    │   PCSX2PluginInvoker.elf (optional)
    │
    ├───GTAVCS
    │       PCSX2PluginDemo.elf
    │       PCSX2PluginDemo.ini
    │
    ├───SCDA
    │       PCSX2PluginDemo2.elf
    │
    └───MKD
            PCSX2PluginDemo3.elf			
```

 - Plugins should be placed inside **PLUGINS** directory, using subfolders is optional.

 - Plugins must have base address that doesn't conflict with other plugins (including **PCSX2PluginInvoker.elf**, which is at **0x2000000**).

 - To find out minimum base address for new plugin, check the log file. Normally anything higher than **0x2001000** should work.

 - Define compatible games for plugin using **CompatibleCRCList** symbol, e.g.:
 ```c
 int CompatibleCRCList[] = { 0xC0498D24, 0xABE2FDE9 };
 ```
 This array is **required** to be present in the plugin, otherwise it will not be loaded.

 - Some games use multiple elf files. To ensure that the plugin will be injected into correct game executable, define **ElfPattern** symbol within plugin, e.g.:

```c
char ElfPattern[] = "10 00 BF FF 00 00 B0 7F 30 00 A4 AF 40 00 A5 AF";
```

 - Set `ElfPattern` to something unique from target elf. 

 - **Ini file** with the same name is written to **PluginData** symbol of the injected plugin. Use this to read ini inside the plugin. Adjust symbol's size accordingly. E.g.:
 ```c
 char PluginData[100] = { 0 };
 ```

 - If **PCSX2Data** symbol is present inside the plugin, e.g. `char PCSX2Data[20] = { 0 };`, you can access these parameters:
 ```c
    int PCSX2Data[10] = { 1 };

    ...

    int DesktopSizeX       = PCSX2Data[PCSX2Data_DesktopSizeX];
    int DesktopSizeY       = PCSX2Data[PCSX2Data_DesktopSizeY];
    int WindowSizeX        = PCSX2Data[PCSX2Data_WindowSizeX];
    int WindowSizeY        = PCSX2Data[PCSX2Data_WindowSizeY];
    int IsFullscreen       = PCSX2Data[PCSX2Data_IsFullscreen];
    int AspectRatioSetting = PCSX2Data[PCSX2Data_AspectRatioSetting];
```
See Demo Plugin 2 for full example.

 - **KeyboardState** and **MouseState** symbols can be used to access mouse and keyboard data (experimental):
 ```c
struct CMouseControllerState
{
    int8_t	lmb;
    int8_t	rmb;
    int8_t	mmb;
    int8_t	wheelUp;
    int8_t	wheelDown;
    int8_t	bmx1;
    int8_t	bmx2;
    float   Z;
    float   X;
    float   Y;
};

enum KeyboardBufState
{
    CurrentState,
    PreviousState,

    StateNum,

    StateSize = 256 //do not modify
};

char KeyboardState[StateNum][StateSize] = { 1 };
struct CMouseControllerState MouseState[StateNum] = { 1 };
```
 See Demo Plugin 1 for full example.

 - **OSDText** symbol can be used to display text on screen (experimental, only in QT version):
```c
enum
{
    OSDStringNum = 10,
    OSDStringSize = 255 //do not modify
};

char OSDText[OSDStringNum][OSDStringSize] = { 1 };
...
npf_snprintf(OSDText[0], 255, "Cam Pos: %s %s %s", pos_x, pos_y, pos_z);
strcpy(OSDText[1], "This is test message");
```
Strings amount can be custom (**10** in the example code), but the length(**255**) is hardcoded, do to change it. 

See Demo Plugin 1 for full example.

- Demo Plugin 1 is compatible with **GTAVCS [SLUS-21590]**. It renders few coronas at the beginning of the game, skips intro, displays some messages using on screen overlay (enable in emulator settings, only available in QT version) and adds possibility to control player with **WASD** keyboard keys, and shoot weapon with left mouse button:

![](https://i.imgur.com/eECJWlQ.png)

- Demo Plugin 2 is for **Splinter Cell Double Agent [SLUS-21356]**, it makes the game's aspect ratio adjust to emulator's resolution via redirecting code to plugin's function:

![](https://i.imgur.com/nYdAUp2.png)

- Demo Plugin 3 is for **Mortal Kombat: Deception [SLES-52705]**, disables intro movies and makes Konquest protagonist always use young model:

![](https://i.imgur.com/VWptXcv.png)

Log example:

```bat
[18:27:50] [thread 8776] [info] Starting PCSX2PluginInjector, game crc: 0xC0498D24
[18:27:50] [thread 8776] [info] EE Memory starts at: 0x7FF6A0000000
[18:27:50] [thread 8776] [info] Game Base Address: 0x200000
[18:27:50] [thread 8776] [info] Game Region End: 0x36D900
[18:27:50] [thread 8776] [info] Loading PCSX2PluginInvoker.elf
[18:27:50] [thread 8776] [info] PCSX2PluginInvoker.elf base address: 0x2000000
[18:27:50] [thread 8776] [info] PCSX2PluginInvoker.elf entry point: 0x2000000
[18:27:50] [thread 8776] [info] Injecting PCSX2PluginInvoker.elf...
[18:27:50] [thread 8776] [info] Finished injecting PCSX2PluginInvoker.elf, 13838 bytes written at 0x2000000
[18:27:50] [thread 8776] [info] Hooking game's entry point function...
[18:27:50] [thread 8776] [info] Finished hooking entry point function at 0x2001F8
[18:27:50] [thread 8776] [info] Looking for plugins in PLUGINS
[18:27:50] [thread 8776] [info] Loading SCDA\PCSX2PluginDemo2.elf
[18:27:50] [thread 8776] [info] PCSX2PluginDemo2.elf base address: 0x2020000
[18:27:50] [thread 8776] [info] PCSX2PluginDemo2.elf entry point: 0x2020038
[18:27:50] [thread 8776] [info] PCSX2PluginDemo2.elf size: 2225 bytes
[18:27:50] [thread 8776] [info] Some plugins has to be loaded in another elf, creating thread to handle it
[18:27:50] [thread 6304] [info] Starting thread ElfSwitchWatcher
[18:27:51] [thread 8776] [warning] Pattern "10 00 BF FF 00 00 B0 7F 30 00 A4 AF 40 00 A5 AF" is not found in this elf, PCSX2PluginDemo2.elf will not be loaded at this time
[18:27:51] [thread 8776] [info] Suggested minimum base address for new plugins: 0x200360E
[18:27:51] [thread 8776] [info] Finished loading plugins

[18:27:56] [thread 6304] [info] Starting PCSX2PluginInjector, game crc: 0xC0498D24
[18:27:56] [thread 6304] [info] EE Memory starts at: 0x7FF6A0000000
[18:27:56] [thread 6304] [info] Game Base Address: 0x200000
[18:27:56] [thread 6304] [info] Game Region End: 0x36D900
[18:27:56] [thread 6304] [info] Loading PCSX2PluginInvoker.elf
[18:27:56] [thread 6304] [info] PCSX2PluginInvoker.elf base address: 0x2000000
[18:27:56] [thread 6304] [info] PCSX2PluginInvoker.elf entry point: 0x2000000
[18:27:56] [thread 6304] [info] Injecting PCSX2PluginInvoker.elf...
[18:27:56] [thread 6304] [info] Finished injecting PCSX2PluginInvoker.elf, 13838 bytes written at 0x2000000
[18:27:56] [thread 6304] [info] Hooking game's entry point function...
[18:27:56] [thread 6304] [info] Finished hooking entry point function at 0x1001F8
[18:27:56] [thread 6304] [info] Looking for plugins in PLUGINS
[18:27:56] [thread 6304] [info] Loading SCDA\PCSX2PluginDemo2.elf
[18:27:56] [thread 6304] [info] PCSX2PluginDemo2.elf base address: 0x2020000
[18:27:56] [thread 6304] [info] PCSX2PluginDemo2.elf entry point: 0x2020038
[18:27:56] [thread 6304] [info] PCSX2PluginDemo2.elf size: 2225 bytes
[18:27:56] [thread 6304] [info] Some plugins has to be loaded in another elf, creating thread to handle it
[18:27:56] [thread 16016] [info] Starting thread ElfSwitchWatcher
[18:27:57] [thread 6304] [info] Injecting PCSX2PluginDemo2.elf...
[18:27:57] [thread 6304] [info] Finished injecting PCSX2PluginDemo2.elf, 2225 bytes written at 0x2020000
[18:27:57] [thread 6304] [info] Loading PCSX2PluginDemo2.ini
[18:27:57] [thread 6304] [info] Writing PCSX2 Data to PCSX2PluginDemo2.elf
[18:27:57] [thread 6304] [info] Suggested minimum base address for new plugins: 0x20208B1
[18:27:57] [thread 6304] [info] Finished loading plugins

[18:27:57] [thread 6304] [info] Ending thread ElfSwitchWatcher
[18:28:01] [thread 16016] [info] Ending thread ElfSwitchWatcher
[18:28:02] [thread 8776] [info] Starting PCSX2PluginInjector, game crc: 0x4F32A11F
[18:28:02] [thread 8776] [info] EE Memory starts at: 0x7FF6A0000000
[18:28:02] [thread 8776] [info] Game Base Address: 0x100000
[18:28:02] [thread 8776] [info] Game Region End: 0x74DF54
[18:28:02] [thread 8776] [info] Loading PCSX2PluginInvoker.elf
[18:28:02] [thread 8776] [info] PCSX2PluginInvoker.elf base address: 0x2000000
[18:28:02] [thread 8776] [info] PCSX2PluginInvoker.elf entry point: 0x2000000
[18:28:02] [thread 8776] [info] Injecting PCSX2PluginInvoker.elf...
[18:28:02] [thread 8776] [info] Finished injecting PCSX2PluginInvoker.elf, 13838 bytes written at 0x2000000
[18:28:02] [thread 8776] [info] Hooking game's entry point function...
[18:28:02] [thread 8776] [info] Finished hooking entry point function at 0x1DD8C0
[18:28:02] [thread 8776] [info] Looking for plugins in PLUGINS
[18:28:02] [thread 8776] [info] Loading PLUGINS\GTAVCS.PCSX2.WidescreenFix.elf
[18:28:02] [thread 8776] [info] GTAVCS.PCSX2.WidescreenFix.elf base address: 0x2002000
[18:28:02] [thread 8776] [info] GTAVCS.PCSX2.WidescreenFix.elf entry point: 0x2002CC8
[18:28:02] [thread 8776] [info] GTAVCS.PCSX2.WidescreenFix.elf size: 3368271 bytes
[18:28:02] [thread 8776] [info] Injecting GTAVCS.PCSX2.WidescreenFix.elf...
[18:28:02] [thread 8776] [info] Finished injecting GTAVCS.PCSX2.WidescreenFix.elf, 3368271 bytes written at 0x2002000
[18:28:02] [thread 8776] [info] Loading GTAVCS.PCSX2.WidescreenFix.ini
[18:28:02] [thread 8776] [info] Injecting GTAVCS.PCSX2.WidescreenFix.ini...
[18:28:02] [thread 8776] [info] GTAVCS.PCSX2.WidescreenFix.ini was successfully injected
[18:28:02] [thread 8776] [info] Writing PCSX2 Data to GTAVCS.PCSX2.WidescreenFix.elf
[18:28:02] [thread 8776] [info] GTAVCS.PCSX2.WidescreenFix.elf requests keyboard state
[18:28:02] [thread 8776] [info] GTAVCS.PCSX2.WidescreenFix.elf requests mouse state
[18:28:02] [thread 8776] [info] Keyboard and mouse data requested by plugins, replacing WndProc for HWND 1059172
[18:28:02] [thread 8776] [info] Suggested minimum base address for new plugins: 0x233854F
[18:28:02] [thread 8776] [info] Finished loading plugins
```
