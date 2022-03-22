#include <elfio/elfio.hpp>
#include "stdafx.h"
#include <filesystem>
#include <assembly64.hpp>
#include "pcsx2/pcsx2.h"

constexpr auto start_pattern = "28 ? 00 70 28 ? 00 70 28 ? 00 70 28 ? 00 70 28";

struct PluginInfo
{
    uint32_t Base;
    uint32_t EntryPoint;
    uint32_t SegmentFileOffset;
    uint32_t Size;
    uint32_t DataAddr;
    uint32_t DataSize;
    uint32_t Malloc;
    uint32_t Free;

    bool isValid() { return (Base != 0 && EntryPoint != 0 && Size != 0); }
};

std::vector<char> LoadFileToBuffer(std::filesystem::path path)
{
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    std::streamsize size = f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<char> buffer(size);
    if (f.read(buffer.data(), size))
        return buffer;
    return std::vector<char>();
}

PluginInfo ParseElf(std::string path)
{
    using namespace ELFIO;

    PluginInfo info = { 0 };

    elfio reader;

    if (reader.load(path) && reader.get_class() == ELFCLASS32 && reader.get_encoding() == ELFDATA2LSB)
    {
        Elf_Half sec_num = reader.sections.size();
        for (int i = 0; i < sec_num; ++i) {
            section* psec = reader.sections[i];
            info.Size += psec->get_size();
        }

        Elf_Half seg_num = reader.segments.size();
        for (int i = 0; i < seg_num; ++i) {
            const segment* pseg = reader.segments[i];

            if (info.SegmentFileOffset == 0)
                info.SegmentFileOffset = pseg->get_offset();
            else
                info.SegmentFileOffset = min(pseg->get_offset(), info.SegmentFileOffset);

            if (info.Base == 0)
                info.Base = pseg->get_virtual_address();
            else
                info.Base = min(pseg->get_virtual_address(), info.Base);

            info.Size += pseg->get_memory_size();
        }

        for (int i = 0; i < sec_num; ++i) {
            section* psec = reader.sections[i];
            if (psec->get_type() == SHT_SYMTAB) {
                const symbol_section_accessor symbols(reader, psec);
                for (unsigned int j = 0; j < symbols.get_symbols_num(); ++j) {
                    std::string   name;
                    Elf64_Addr    value;
                    Elf_Xword     size;
                    unsigned char bind;
                    unsigned char type;
                    Elf_Half      section_index;
                    unsigned char other;

                    symbols.get_symbol(j, name, value, size, bind, type, section_index, other);

                    if (name == "PluginData")
                    {
                        info.DataAddr = value;
                        info.DataSize = size;
                    }
                }
            }
        }
        info.EntryPoint = reader.get_entry();
    }
    return info;
}

CEXP void LoadPlugins(uint32_t crc, uintptr_t EEMainMemoryStart, size_t EEMainMemorySize)
{
    spd::log()->info("Starting PCSX2PluginInjector, game crc: 0x{:X}", crc);
    spd::log()->info("EE Memory starts at: 0x{:X}", EEMainMemoryStart);

    wchar_t buffer[MAX_PATH];
    HMODULE hm = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&LoadPlugins, &hm);
    GetModuleFileNameW(hm, buffer, sizeof(buffer));
    auto modulePath = std::filesystem::path(buffer).remove_filename();
    auto pluginsPath = modulePath / L"PLUGINS";
    auto invokerPath = pluginsPath / L"PCSX2PluginInvoker.elf";

    spd::log()->info("Looking for plugins in {}", pluginsPath.string());

    if (std::filesystem::exists(pluginsPath) && std::filesystem::exists(invokerPath))
    {
        PluginInfo invoker = ParseElf(invokerPath.string());
        auto bInvokerLoaded = false;

        auto count = 0;
        for (const auto& folder : std::filesystem::recursive_directory_iterator(pluginsPath, std::filesystem::directory_options::skip_permission_denied))
        {
            if (folder.path().stem().wstring().starts_with(int_to_hex(crc)))
            {
                for (const auto& file : std::filesystem::directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied))
                {
                    if (file.is_regular_file() && iequals(file.path().extension().wstring(), L".elf"))
                    {
                        CIniReader pluginini(std::filesystem::path(file.path()).replace_extension(L".ini").string());

                        invoker.Malloc = pluginini.ReadInteger("MAIN", "Malloc", 0);
                        if (invoker.Malloc == 0)
                        {
                            auto malloc_ini = pluginini.ReadString("MAIN", "MallocPatternString", "");
                            if (!malloc_ini.empty())
                            {
                                auto pattern = hook::pattern(EEMainMemoryStart, EEMainMemoryStart + EEMainMemorySize, malloc_ini);
                                if (!pattern.empty())
                                    invoker.Malloc = (uint32_t)pattern.get(pluginini.ReadInteger("MAIN", "MallocPatternIndex", 0)).get<void>(pluginini.ReadInteger("MAIN", "MallocPatternOffset", 0)) - EEMainMemoryStart;
                            }
                        }

                        invoker.Free = pluginini.ReadInteger("MAIN", "Free", 0);
                        if (invoker.Free == 0)
                        {
                            auto free_ini = pluginini.ReadString("MAIN", "FreePatternString", "");
                            if (!free_ini.empty())
                            {
                                auto pattern = hook::pattern(EEMainMemoryStart, EEMainMemoryStart + EEMainMemorySize, free_ini);
                                if (!pattern.empty())
                                    invoker.Free = (uint32_t)pattern.get(pluginini.ReadInteger("MAIN", "FreePatternIndex", 0)).get<void>(pluginini.ReadInteger("MAIN", "FreePatternOffset", 0)) - EEMainMemoryStart;
                            }
                        }

                        if (invoker.Malloc == 0)
                        {
                            spd::log()->error("{} does not contain 'malloc' address in game elf, plugin injection can not continue", pluginini.GetIniPath());
                            return;
                        }

                        if (!bInvokerLoaded)
                        {
                            spd::log()->info("Loading {}", invokerPath.filename().string());
                            spd::log()->info("{} base address: 0x{:X}", invokerPath.filename().string(), invoker.Base);
                            spd::log()->info("{} entry point: 0x{:X}", invokerPath.filename().string(), invoker.EntryPoint);
                            if (!invoker.isValid())
                            {
                                spd::log()->error("{} could not be loaded, exiting...", invokerPath.filename().string());
                                return;
                            }

                            auto buffer = LoadFileToBuffer(invokerPath);
                            if (!buffer.empty())
                            {
                                injector::WriteMemoryRaw(EEMainMemoryStart + invoker.Base, buffer.data() + invoker.SegmentFileOffset, buffer.size() - invoker.SegmentFileOffset, true);
                                injector::WriteMemoryRaw(EEMainMemoryStart + invoker.DataAddr + (sizeof(invoker) * count), &invoker, sizeof(invoker), true);

                                auto patched = false;
                                auto pattern = hook::pattern(EEMainMemoryStart, EEMainMemoryStart + EEMainMemorySize, start_pattern);
                                pattern.for_each_result([&](hook::pattern_match match)
                                    {
                                        auto ei_lookup = hook::pattern((uintptr_t)match.get<void>(0), (uintptr_t)match.get<void>(2000), "38 00 00 42");
                                        if (!ei_lookup.empty())
                                        {
                                            //injector::WriteMemory(ei_lookup.get_first(), ((0x0C000000 | ((invoker.EntryPoint & 0x0fffffff) >> 2))), true);
                                            injector::WriteMemory(ei_lookup.get_first(), mips::jal(invoker.EntryPoint), true);
                                            patched = true;
                                            return;
                                        }
                                    });

                                if (!patched)
                                {
                                    spd::log()->error("{} can't hook the game with CRC {}, exiting...", invokerPath.filename().string(), crc);
                                    return;
                                }

                                spd::log()->info("{} was successfully injected", invokerPath.filename().string());
                                bInvokerLoaded = true;
                                count++;
                            }
                        }

                        spd::log()->info("Loading {}", file.path().filename().string());
                        PluginInfo mod = ParseElf(file.path().string());
                        if (!mod.isValid())
                        {
                            spd::log()->warn("Can't load {}", file.path().filename().string());
                            continue;
                        }

                        //add check for conflicting base addresses

                        spd::log()->info("{} base address: 0x{:X}", file.path().filename().string(), mod.Base);
                        spd::log()->info("{} entry point: 0x{:X}", file.path().filename().string(), mod.EntryPoint);

                        mod.Malloc = invoker.Malloc;
                        mod.Free = invoker.Free;

                        auto buffer = LoadFileToBuffer(file.path().string());
                        if (!buffer.empty())
                        {
                            injector::WriteMemoryRaw(EEMainMemoryStart + mod.Base, buffer.data() + mod.SegmentFileOffset, buffer.size() - mod.SegmentFileOffset, true);
                            injector::WriteMemoryRaw(EEMainMemoryStart + invoker.DataAddr + (sizeof(invoker) * count), &mod, sizeof(mod), true);
                            auto ini = LoadFileToBuffer(pluginini.GetIniPath());
                            injector::WriteMemory(EEMainMemoryStart + mod.DataAddr, ini.size(), true);
                            injector::WriteMemoryRaw(EEMainMemoryStart + mod.DataAddr + sizeof(uint32_t), ini.data(), ini.size(), true);
                            count++;
                            spd::log()->info("{} was successfully injected", file.path().filename().string());
                        }
                        else
                            spd::log()->warn("Can't load {}", file.path().filename().string());
                    }
                }
            }
        }
    }
    spd::log()->info("Finished injecting plugins, exiting...");
}


void Init()
{

}

CEXP void InitializeASI()
{
    //std::call_once(CallbackHandler::flag, []()
    //{
    //    CallbackHandler::RegisterCallback(Init, hook::pattern(""));
    //});
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        //if (!IsUALPresent()) { InitializeASI(); }
    }
    return TRUE;
}
