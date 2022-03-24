#include <elfio/elfio.hpp>
#include "stdafx.h"
#include <filesystem>
#include <assembly64.hpp>
#include "pcsx2/pcsx2.h"

#include <thread>
#include <iostream>
#include <assert.h>
#include <chrono>
#include <future>

std::promise<void> exitSignal;

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

CEXP void LoadPlugins(uint32_t& crc, uintptr_t EEMainMemoryStart, size_t EEMainMemorySize, uintptr_t GameElfTextBase, uintptr_t GameElfTextSize);

void FindMemoryBuffer(uintptr_t EEMainMemoryStart, size_t EEMainMemorySize, uintptr_t GameElfTextBase, uintptr_t GameElfTextSize)
{
    auto test = [](uint8_t* begin, std::size_t bytes) -> bool
    {
        return std::all_of(begin, begin + bytes, [](uint8_t const byte)
            {
                return byte == 0;
            });
    };

    size_t NeededRegionSize = 200000;
    auto start = EEMainMemoryStart + GameElfTextBase + GameElfTextSize;
    auto end = EEMainMemoryStart + EEMainMemorySize - NeededRegionSize;

    using namespace std::chrono_literals;
    std::this_thread::sleep_for(25500ms);

    AllocConsole();
    freopen("CONOUT$", "w", stdout);

    for (auto i = start; i < end; i += 1)
    {
        while (test((uint8_t*)i, NeededRegionSize))
        {
            std::cout << "0x" << std::hex << i - EEMainMemoryStart << std::endl;
        }
    }
    std::cout << "Nothing found" << std::endl;
}

void ElfSwitchWatcher(std::future<void> futureObj, uint32_t* addr, uint32_t data, uint32_t& crc, uintptr_t EEMainMemoryStart, size_t EEMainMemorySize, uintptr_t GameElfTextBase, uintptr_t GameElfTextSize)
{
    auto cur_crc = crc;
    while (futureObj.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout)
    {
        MEMORY_BASIC_INFORMATION MemoryInf;
        if (cur_crc != crc || (VirtualQuery((LPCVOID)addr, &MemoryInf, sizeof(MemoryInf)) != 0 && MemoryInf.Protect != 0))
        {
            if (cur_crc != crc || *addr != data)
            {
                while (*addr == 0)
                {
                    if (cur_crc != crc)
                        return;
                    std::this_thread::yield();
                }
                LoadPlugins(crc, EEMainMemoryStart, EEMainMemorySize, GameElfTextBase, GameElfTextSize);
                break;
            }
        }
        else
        {
            break;
        }
        std::this_thread::yield();
    }
}

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

void LoadPlugins(uint32_t& crc, uintptr_t EEMainMemoryStart, size_t EEMainMemorySize, uintptr_t GameElfTextBase, uintptr_t GameElfTextSize)
{
    spd::log()->info("Starting PCSX2PluginInjector, game crc: 0x{:X}", crc);
    spd::log()->info("EE Memory starts at: 0x{:X}", EEMainMemoryStart);
    spd::log()->info("Game Base Address: 0x{:X}", GameElfTextBase);
    spd::log()->info("Game Region End: 0x{:X}", GameElfTextBase + GameElfTextSize);
    //spd::log()->info("Suggested minimum base address for plugins: 0x{:X}", GameElfTextBase + GameElfTextSize + 0xAC);

    if (0)
        std::thread(&FindMemoryBuffer, EEMainMemoryStart, EEMainMemorySize, GameElfTextBase, GameElfTextSize).detach();

    exitSignal.set_value();
    std::promise<void>().swap(exitSignal);
    uint32_t* ei_hook = nullptr;
    uint32_t ei_data = 0;

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
                        auto iniPath = std::filesystem::path(file.path()).replace_extension(L".ini");
                        CIniReader pluginini(iniPath.string());

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

                            if (invoker.Malloc == 0)
                                spd::log()->warn("{} does not contain 'malloc' address in game elf, make sure plugin will not be overwritten in memory", iniPath.filename().string());
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
                                pattern.for_each_result([&ei_hook, &ei_data, &invoker, &patched](hook::pattern_match match)
                                    {
                                        auto ei_lookup = hook::pattern((uintptr_t)match.get<void>(0), (uintptr_t)match.get<void>(2000), "38 00 00 42");
                                        if (!ei_lookup.empty())
                                        {
                                            ei_hook = ei_lookup.get_first<uint32_t>();
                                            ei_data = mips::jal(invoker.EntryPoint);
                                            injector::WriteMemory(ei_hook, ei_data, true);
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

                                if (pluginini.ReadInteger("MAIN", "MultipleElfs", 0))
                                {
                                    spd::log()->info("MultipleElfs parameter is set, creating thread to handle it");
                                    std::future<void> futureObj = exitSignal.get_future();
                                    std::thread th(&ElfSwitchWatcher, std::move(futureObj), ei_hook, ei_data, std::ref(crc), EEMainMemoryStart, EEMainMemorySize, GameElfTextBase, GameElfTextSize);
                                    th.detach();
                                }
                                else
                                {
                                    spd::log()->info("MultipleElfs ini parameter is not set, plugins will not be injected if the game loads another elf");
                                }
                            }
                        }

                        spd::log()->info("Loading {}", file.path().filename().string());
                        PluginInfo mod = ParseElf(file.path().string());
                        if (!mod.isValid())
                        {
                            spd::log()->warn("Can't load {}", file.path().filename().string());
                            continue;
                        }

                        auto pattern_str = pluginini.ReadString("MAIN", "ElfPattern", "");
                        if (!pattern_str.empty())
                        {
                            auto pattern = hook::pattern(EEMainMemoryStart, EEMainMemoryStart + EEMainMemorySize, pattern_str);
                            if (pattern.empty())
                            {
                                spd::log()->warn("Pattern \"{}\" is not found in this elf, {} will not be loaded", pattern_str, file.path().filename().string());
                                continue;
                            }
                        }

                        //add check for conflicting base addresses

                        spd::log()->info("{} base address: 0x{:X}", file.path().filename().string(), mod.Base);
                        spd::log()->info("{} entry point: 0x{:X}", file.path().filename().string(), mod.EntryPoint);
                        spd::log()->info("{} size: {} bytes", file.path().filename().string(), mod.Size);

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
