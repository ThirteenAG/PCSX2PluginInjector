# PCSX2 Fork Plugin Injector

<!-- plugin-upstream:begin -->
## Original PCSX2 download for Plugin Injector

The current Windows x64 adapter targets **PCSX2 v2.9.94**, the upstream development
sources used by this fork (`81526d4dc7cc70e4ae75abb35a789417456c6d43`).

- [Download original PCSX2 v2.9.94 for Windows x64](https://github.com/PCSX2/pcsx2/releases/download/v2.9.94/pcsx2-v2.9.94-windows-x64-Qt.7z)
- [Release details and optional debugging symbols](https://github.com/PCSX2/pcsx2/releases/tag/v2.9.94)

Use this exact build with the corresponding Plugin Injector package. Other
versions are rejected before installing hooks. This is a pinned development build;
the discovery CI never enables newer builds automatically. Enable **128 MB RAM** in
PCSX2's Advanced settings before using guest plugins.
<!-- plugin-upstream:end -->

## Using

 - Download [PCSX2 Fork With Plugins](https://github.com/ASI-Factory/PCSX2-Fork-With-Plugins/releases/tag/latest), this project is already included in it. (**Windows only**).

 - Copy **.elf** plugins to **PLUGINS** directory, e.g. **[GTAVCS.PCSX2.WidescreenFix.elf](https://thirteenag.github.io/wfp#gtavcs)**.

## Compatibility with regular PCSX2 builds

Use the exact upstream development build linked above. Compatibility is tied to
the fork's upstream source revision and verified executable/PDB identity.

 - Download [PCSX2PluginInjector.zip](https://github.com/ThirteenAG/PCSX2PluginInjector/releases/tag/latest) (**Windows only**).
 - Unpack [PCSX2PluginInjector.zip](https://github.com/ThirteenAG/PCSX2PluginInjector/releases/tag/latest) to PCSX2 root directory, where the exe is located.
 - Under **Tools**, toggle **Show Advanced Settings**.
 - Under **Settings** -> **Advanced**, toggle **Enable 128 MB RAM**.
 - Copy **.elf** plugins to **PLUGINS** directory, e.g. **[GTAVCS.PCSX2.WidescreenFix.elf](https://thirteenag.github.io/wfp#gtavcs)**.

## Limitations

 - Only Windows version is supported.

 - Save/load states are blocked while guest modules are active until module
   persistence is implemented. Reset and normal shutdown clear module state.

 - Do not open issues in PCSX2 repository when using the fork. Reproduce them in regular PCSX2 build first.

## Plugin development

Initialize submodules, run `premake5.bat`, and build the solution. Guest projects
invoke the Windows PS2SDK module builder; no unique base address or standalone
PS2SDK startup executable is required. The three game demos and C/C++ probes all
use the relocatable ABI. The old invoker and dummy are retired.

The emulator directory contains `pcsx2-qtx64.exe` (fork) or `pcsx2-qt.exe`
(original), Ultimate ASI Loader's `version.dll`, `PCSX2PluginInjector.asi`, and a
`PLUGINS` directory. Original PCSX2 also requires the packaged
`PCSX2PluginInjector.stock.ini` beside the injector. Plugin subfolders are optional.

Implement the existing `init()` in C or `extern "C" void init()` in C++ and
list your source files in `module.json`. The SDK supplies the descriptor, startup,
constructor initialization, private stack, and aligned heap. Existing data exports
below retain their names and layout. Old fixed-address binaries require rebuilding.
See [guest module development](docs/guest-modules.md) for the build/runtime contract.

 - Define compatible games for plugin using **CompatibleCRCList** symbol, e.g.:
 ```c
 int CompatibleCRCList[] = { 0xC0498D24, 0xABE2FDE9 };
 ```
 This array is **required** to be present in the plugin, otherwise it will not be loaded.

 - Some games use multiple elf files. To ensure that the plugin will be injected into correct game executable, define **CompatibleElfCRCList** symbol within plugin, e.g.:

```c
int CompatibleElfCRCList[] = { 0x198F1AD, 0x6BD0E9C2 };
```

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

 - **OSDText** symbol can be used to display text on screen:
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
The number of strings is configurable; keep each row exactly **255 bytes**.

See Demo Plugin 1 for full example.

- Demo Plugin 1 is compatible with **GTAVCS [SLUS-21590]**. It renders few coronas at the beginning of the game, skips intro, displays some messages using on screen overlay and adds possibility to control player with **WASD** keyboard keys, and shoot weapon with left mouse button:

![](https://i.imgur.com/eECJWlQ.png)

- Demo Plugin 2 is for **Splinter Cell Double Agent [SLUS-21356]**, it makes the game's aspect ratio adjust to emulator's resolution via redirecting code to plugin's function:

![](https://i.imgur.com/nYdAUp2.png)

- Demo Plugin 3 is for **Mortal Kombat: Deception [SLES-52705]**, disables intro movies and makes Konquest protagonist always use young model:

![](https://i.imgur.com/VWptXcv.png)
