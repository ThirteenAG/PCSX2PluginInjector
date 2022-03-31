workspace "PCSX2PluginInjector"
   configurations { "Release", "Debug" }
   platforms { "Win64" }
   architecture "x64"
   location "build"
   objdir ("build/obj")
   buildlog ("build/log/%{prj.name}.log")
   cppdialect "C++latest"
   include "makefile.lua"
   
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
   files { "Resources/*.rc" }
   files { "external/hooking/Hooking.Patterns.h", "external/hooking/Hooking.Patterns.cpp" }
   files { "includes/stdafx.h", "includes/stdafx.cpp" }
   includedirs { "includes" }
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
   
   function setbuildpaths_ps2(gamepath, exepath, scriptspath, ps2sdkpath, sourcepath, prj_name)
      local pbcmd = {}
      for k,v in pairs(pbcommands) do
        pbcmd[k] = v
      end
      if (gamepath) then
         cmdcopy = { "set \"path=" .. gamepath .. scriptspath .. "\"" }
         pbcmd[2] = "set \"file=../data/" .. scriptspath .. prj_name ..".elf\""
         table.insert(cmdcopy, pbcmd)
         buildcommands   { "call " .. ps2sdkpath .. " -C " .. sourcepath, cmdcopy }
         rebuildcommands { "call " .. ps2sdkpath .. " -C " .. sourcepath .. " clean && " .. ps2sdkpath .. " -C " .. sourcepath, cmdcopy }
         cleancommands   { "call " .. ps2sdkpath .. " -C " .. sourcepath .. " clean" }
         debugdir (gamepath)
         if (exepath) then
            debugcommand (gamepath .. exepath)
            dir, file = exepath:match'(.*/)(.*)'
            debugdir (gamepath .. (dir or ""))
         end
      end
      targetdir ("data/" .. scriptspath)
   end
   
   function add_asmjit()
      files { "external/asmjit/src/**.cpp" }
      includedirs { "external/asmjit/src" }
   end
      
   filter "configurations:Debug*"
      defines "DEBUG"
      symbols "On"

   filter "configurations:Release*"
      defines "NDEBUG"
      optimize "On"


project "PCSX2PluginInjector"
   buildoptions { "/bigobj" }
   includedirs { "external/elfio" }
   add_asmjit()
   setpaths("Z:/GitHub/PCSX2-Fork-With-Plugins/bin/", "pcsx2x64.exe", "scripts/")

project "PCSX2PluginInvoker"
   kind "Makefile"
   includedirs { "external/ps2sdk/ps2sdk/ee" }
   files { "source/%{prj.name}/*.c" }
   targetextension ".elf"
   setbuildpaths_ps2("Z:/GitHub/PCSX2-Fork-With-Plugins/bin/", "pcsx2x64.exe", "scripts/PLUGINS/", "%{wks.location}/../external/ps2sdk/ee/bin/vsmake", "%{wks.location}/../source/%{prj.name}/", "PCSX2PluginInvoker")
   writemakefile("PCSX2PluginInvoker", "scripts/PLUGINS/", "0x02000000")
   writelinkfile("PCSX2PluginInvoker")

project "PCSX2PluginDemo"
   kind "Makefile"
   includedirs { "external/ps2sdk/ps2sdk/ee" }
   files { "source/%{prj.name}/*.c" }
   targetextension ".elf"
   setbuildpaths_ps2("Z:/GitHub/PCSX2-Fork-With-Plugins/bin/", "pcsx2x64.exe", "scripts/PLUGINS/4F32A11F-GTAVCS-[SLUS-21590]/", "%{wks.location}/../external/ps2sdk/ee/bin/vsmake", "%{wks.location}/../source/%{prj.name}/", "PCSX2PluginDemo")
   writemakefile("PCSX2PluginDemo", "scripts/PLUGINS/4F32A11F-GTAVCS-[SLUS-21590]/", "0x02020000")
   writelinkfile("PCSX2PluginDemo")
   
project "PCSX2PluginDemo2"
   kind "Makefile"
   includedirs { "external/ps2sdk/ps2sdk/ee" }
   files { "source/%{prj.name}/*.c" }
   targetextension ".elf"
   setbuildpaths_ps2("Z:/GitHub/PCSX2-Fork-With-Plugins/bin/", "pcsx2x64.exe", "scripts/PLUGINS/C0498D24-SCDA-[SLUS-21356]/", "%{wks.location}/../external/ps2sdk/ee/bin/vsmake", "%{wks.location}/../source/%{prj.name}/", "PCSX2PluginDemo2")
   writemakefile("PCSX2PluginDemo2", "scripts/PLUGINS/C0498D24-SCDA-[SLUS-21356]/", "0x02020000")
   writelinkfile("PCSX2PluginDemo2")

project "PCSX2PluginDemo3"
   kind "Makefile"
   includedirs { "external/ps2sdk/ps2sdk/ee" }
   files { "source/%{prj.name}/*.c" }
   targetextension ".elf"
   setbuildpaths_ps2("Z:/GitHub/PCSX2-Fork-With-Plugins/bin/", "pcsx2x64.exe", "scripts/PLUGINS/43341C03-MKD-[SLES-52705]/", "%{wks.location}/../external/ps2sdk/ee/bin/vsmake", "%{wks.location}/../source/%{prj.name}/", "PCSX2PluginDemo3")
   writemakefile("PCSX2PluginDemo3", "scripts/PLUGINS/43341C03-MKD-[SLES-52705]/", "0x02020000")
   writelinkfile("PCSX2PluginDemo3")
   
project "PCSX2PluginDummy"
   kind "Makefile"
   includedirs { "external/ps2sdk/ps2sdk/ee" }
   files { "source/%{prj.name}/*.c" }
   targetextension ".elf"
   setbuildpaths_ps2("Z:/GitHub/PCSX2-Fork-With-Plugins/bin/", "pcsx2x64.exe", "scripts/PLUGINS/", "%{wks.location}/../external/ps2sdk/ee/bin/vsmake", "%{wks.location}/../source/%{prj.name}/", "PCSX2PluginDummy")
   writemakefile("PCSX2PluginDummy", "scripts/PLUGINS/", "0x02020000")
   writelinkfile("PCSX2PluginDummy")