REM Pack binaries (Main)
7z a "PCSX2PluginInjector.zip" ".\data\*" ^
-x!PLUGINS\GTAVCS ^
-x!PLUGINS\MKD ^
-x!PLUGINS\SCDA ^
-x!PLUGINS\PCSX2PluginInvoker.elf ^
-x!PLUGINS\PCSX2PluginDummy.elf ^
-xr!.gitkeep ^
-xr!*.pdb ^
-xr!*.lib ^
-xr!*.exp ^
-xr!*.map

REM Pack binaries (With Demo Plugins)
7z a "PCSX2PluginInjectorDemo.zip" ".\data\*" ^
-xr!.gitkeep ^
-xr!*.pdb ^
-xr!*.lib ^
-xr!*.exp ^
-xr!*.map
