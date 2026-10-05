REM Pack binaries (Main)
if exist "PCSX2PluginInjector.zip" del /q "PCSX2PluginInjector.zip"
7z a "PCSX2PluginInjector.zip" ".\data\*" ^
-x!PLUGINS\GTAVCS ^
-x!PLUGINS\MKD ^
-x!PLUGINS\SCDA ^
-x!PLUGINS\PCSX2PluginInvoker.elf ^
-x!PLUGINS\PCSX2PluginDummy.elf ^
-x!PLUGINS\*.elf ^
-xr!*.objects ^
-xr!*.tmp ^
-xr!.gitkeep ^
-xr!*.pdb ^
-xr!*.lib ^
-xr!*.exp ^
-xr!*.map

REM Pack binaries (With Demo Plugins)
if exist "PCSX2PluginInjectorDemo.zip" del /q "PCSX2PluginInjectorDemo.zip"
7z a "PCSX2PluginInjectorDemo.zip" ".\data\*" ^
-x!PLUGINS\PCSX2PluginInvoker.elf ^
-x!PLUGINS\PCSX2PluginDummy.elf ^
-xr!*.objects ^
-xr!*.tmp ^
-xr!.gitkeep ^
-xr!*.pdb ^
-xr!*.lib ^
-xr!*.exp ^
-xr!*.map
