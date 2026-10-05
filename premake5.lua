workspace "PCSX2PluginInjector"
   configurations { "Release", "Debug" }
   platforms { "Win64" }
   architecture "x64"
   location "build"
   objdir ("build/obj")
   buildlog ("build/log/%{prj.name}.log")
   cppdialect "C++latest"
   buildoptions{"/utf-8"}
   
   kind "SharedLib"
   language "C++"
   targetdir "data/scripts"
   targetextension ".asi"
   characterset ("UNICODE")
   staticruntime "On"
   
   defines { "rsc_CompanyName=\"ThirteenAG\"" }
   defines { "rsc_LegalCopyright=\"MIT License\""} 
   defines { "rsc_FileVersion=\"1.0.0.0\"", "rsc_ProductVersion=\"1.0.0.0\"" }
   defines { "rsc_InternalName=\"%{prj.name}\"", "rsc_ProductName=\"%{prj.name}\"", "rsc_OriginalFilename=\"%{prj.name}.asi\"" }
   defines { "rsc_FileDescription=\"PCSX2 Plugin Injector\"" }
   defines { "rsc_UpdateUrl=\"https://github.com/ThirteenAG/PCSX2PluginInjector\"" }
   
   files { "source/%{prj.name}/*.cpp" }
   files { "source/%{prj.name}/*.h", "source/API/*.h" }
   files { "Resources/*.rc" }
   files { "includes/stdafx.h", "includes/stdafx.cpp" }
   includedirs { "includes" }
   includedirs { "source/api" }
   includedirs { "external/injector/safetyhook/include" }
   includedirs { "external/injector/zydis" }
   includedirs { "external/hooking" }
   includedirs { "external/injector/include" }
   includedirs { "external/inireader" }
   includedirs { "external/spdlog/include" }
   includedirs { "external/filewatch" }
   includedirs { "external/modutils" }
   
   pbcommands = { 
      "setlocal EnableDelayedExpansion",
      --"set \"path=" .. (gamepath) .. "\"",
      "set file=$(TargetPath)",
      "FOR %%i IN (\"%file%\") DO (",
      "set filename=%%~ni",
      "set fileextension=%%~xi",
      "set target=!path!!filename!!fileextension!",
      "if exist \"!target!\" copy /y \"%%~fi\" \"!target!\"",
      ")" }

   function setpaths(gamepath, exepath, scriptspath)
      scriptspath = scriptspath or "scripts/"
      if (gamepath) then
         cmdcopy = { "set \"path=" .. gamepath .. scriptspath .. "\"" }
         table.insert(cmdcopy, pbcommands)
         postbuildcommands (cmdcopy)
         debugdir (gamepath)
         if (exepath) then
            debugcommand (gamepath .. exepath)
            dir, file = exepath:match'(.*/)(.*)'
            debugdir (gamepath .. (dir or ""))
         end
      end
      targetdir ("data/" .. scriptspath)
   end
   
   filter "configurations:Debug*"
      defines "DEBUG"
      symbols "On"

   filter "configurations:Release*"
      defines "NDEBUG"
      optimize "On"


project "PCSX2PluginInjector"
   files { "external/injector/safetyhook/src/allocator.cpp", "external/injector/safetyhook/src/inline_hook.cpp",
      "external/injector/safetyhook/src/os.windows.cpp", "external/injector/safetyhook/src/utility.cpp",
      "external/injector/zydis/Zydis.c" }
   buildoptions { "/bigobj" }
   setpaths("Z:/GitHub/PCSX2-Fork-With-Plugins/bin/", "pcsx2-qtx64.exe", "")

-- The legacy invoker/dummy are retired. Every demo and probe is relocatable.
for _, name in ipairs({ "PCSX2PluginDemo", "PCSX2PluginDemo2", "PCSX2PluginDemo3" }) do
   project(name)
      kind "Makefile"
      files { "source/" .. name .. "/main.c", "source/" .. name .. "/module.json", "source/" .. name .. "/makefile" }
      targetextension ".elf"
      targetdir("data/PLUGINS/" .. ({PCSX2PluginDemo="GTAVCS", PCSX2PluginDemo2="SCDA", PCSX2PluginDemo3="MKD"})[name])
      local command = 'powershell -NoProfile -ExecutionPolicy Bypass -File "%{wks.location}/../tools/build-module.ps1" -Project "%{wks.location}/../source/' .. name .. '/module.json"'
      buildcommands { command }
      rebuildcommands { command .. ' -Clean', command }
      cleancommands { command .. ' -Clean' }
end

for id = 1, 2 do
   project ("ModuleProbe" .. id)
      kind "Makefile"
      files { "source/GuestModuleProbe/main.c", "source/API/guest_module.h", "tools/build-module.ps1" }
      includedirs { "source/API" }
      targetname ("ModuleProbe" .. id)
      targetextension ".elf"
      targetdir "data/PLUGINS"
      local command = 'powershell -NoProfile -ExecutionPolicy Bypass -File "%{wks.location}/../tools/build-module.ps1" -Sources "%{wks.location}/../source/GuestModuleProbe/main.c" -Output "%{wks.location}/../data/PLUGINS/ModuleProbe' .. id .. '.elf" -Defines PROBE_ID=' .. id .. ' -NoRuntime'
      buildcommands { command }
      rebuildcommands { command .. ' -Clean', command }
      cleancommands { command .. ' -Clean' }
end

project "ModuleProbeCpp"
   kind "Makefile"
   files { "source/GuestModuleProbe/cpp.cpp", "source/API/guest_module.h", "tools/build-module.ps1" }
   targetextension ".elf"
   targetdir "data/PLUGINS"
   local command = 'powershell -NoProfile -ExecutionPolicy Bypass -File "%{wks.location}/../tools/build-module.ps1" -Sources "%{wks.location}/../source/GuestModuleProbe/cpp.cpp" -Output "%{wks.location}/../data/PLUGINS/ModuleProbeCpp.elf"'
   buildcommands { command }
   rebuildcommands { command .. ' -Clean', command }
   cleancommands { command .. ' -Clean' }
