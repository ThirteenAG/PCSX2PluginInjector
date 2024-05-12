# PCSX2 Fork Plugin Injector

## Using

 - Download [PCSX2 Fork With Plugins](https://github.com/ASI-Factory/PCSX2-Fork-With-Plugins/releases/tag/latest), this project is already included in it. (**Windows only**).

 - Copy **.elf** plugins to **PLUGINS** directory, e.g. **[GTAVCS.PCSX2.WidescreenFix.elf](https://thirteenag.github.io/wfp#gtavcs)**.

## Compatibility with regular PCSX2 builds

It is not recommended to use Plugin Injector with regular PCSX2 builds, however there's a limited compatibility mode.

 - Download [PCSX2 Latest Pre-Release v1.7.x](https://github.com/PCSX2/PCSX2/releases/).
 - Download [PCSX2PluginInjector.zip](https://github.com/ThirteenAG/PCSX2PluginInjector/releases/tag/latest) (**Windows only**).
 - Unpack [PCSX2PluginInjector.zip](https://github.com/ThirteenAG/PCSX2PluginInjector/releases/tag/latest) to PCSX2 root directory, where the exe is located.
 - Under **Settings** -> **Graphics**, select **Fit to Window / Fullscreen**.
 - Under **Tools**, toggle **Show Advanced Settings**.
 - Under **Settings** -> **Advanced**, toggle **Enable 128 MB RAM**.
 - Scroll down below and under **Settings** -> **Advanced** -> **PINE Settings**, toggle **Enable**, and make sure to have **slot** with value **28011**.
 - Copy **.elf** plugins to **PLUGINS** directory, e.g. **[GTAVCS.PCSX2.WidescreenFix.elf](https://thirteenag.github.io/wfp#gtavcs)**.
 - Not all plugins may be compatible, and not all features are available(e.g. unthrottle). Use Fork version for full compatibility.

## Limitations

 - Only Windows version is supported.

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
Strings amount can be custom (**10** in the example code), but the length(**255**) is hardcoded, do to change it. 

See Demo Plugin 1 for full example.

- Demo Plugin 1 is compatible with **GTAVCS [SLUS-21590]**. It renders few coronas at the beginning of the game, skips intro, displays some messages using on screen overlay and adds possibility to control player with **WASD** keyboard keys, and shoot weapon with left mouse button:

![](https://i.imgur.com/eECJWlQ.png)

- Demo Plugin 2 is for **Splinter Cell Double Agent [SLUS-21356]**, it makes the game's aspect ratio adjust to emulator's resolution via redirecting code to plugin's function:

![](https://i.imgur.com/nYdAUp2.png)

- Demo Plugin 3 is for **Mortal Kombat: Deception [SLES-52705]**, disables intro movies and makes Konquest protagonist always use young model:

![](https://i.imgur.com/VWptXcv.png)
