#include <elfio/elfio.hpp>
#include "stdafx.h"
#include <filesystem>
#include "pcsx2/pcsx2.h"

#include <thread>
#include <iostream>
#include <assert.h>
#include <chrono>
#include <future>

#include <tlhelp32.h>

#define IDR_INVOKER    101

std::promise<void> exitSignal;

enum class PCSX2DataType : uint32_t
{
    PCSX2Data_DesktopSizeX,
    PCSX2Data_DesktopSizeY,
    PCSX2Data_WindowSizeX,
    PCSX2Data_WindowSizeY,
    PCSX2Data_IsFullscreen,
    PCSX2Data_AspectRatioSetting
};

enum class AspectRatioType : uint8_t
{
    Stretch,
    R4_3,
    R16_9,
    MaxCount
};

struct PluginInfo
{
    uint32_t Base;
    uint32_t EntryPoint;
    uint32_t SegmentFileOffset;
    uint32_t Size;
    uint32_t DataAddr;
    uint32_t DataSize;
    uint32_t PCSX2DataAddr;
    uint32_t PCSX2DataSize;
    uint32_t CompatibleCRCListAddr;
    uint32_t CompatibleCRCListSize;
    uint32_t PatternDataAddr;
    uint32_t PatternDataSize;
    uint32_t KeyboardStateAddr;
    uint32_t KeyboardStateSize;
    uint32_t MouseStateAddr;
    uint32_t MouseStateSize;
    uint32_t OSDTextAddr;
    uint32_t OSDTextSize;

    bool isValid() { return (Base != 0 && EntryPoint != 0 && Size != 0); }
};

std::vector<std::string_view>& GetOSDVector()
{
    static std::vector<std::string_view> osd;
    return osd;
}

CEXP size_t GetOSDVectorSize()
{
    return GetOSDVector().size();
}

CEXP const char* GetOSDVectorData(size_t index)
{
    if (index > GetOSDVectorSize())
        return nullptr;
    else
        return GetOSDVector()[index].data();
}

CEXP void LoadPlugins(uint32_t& crc, uintptr_t EEMainMemoryStart, size_t EEMainMemorySize, uintptr_t GameElfTextBase, uintptr_t GameElfTextSize, bool bEnableEE128mbRam, int& WindowSizeX, int& WindowSizeY, bool& IsFullscreen, AspectRatioType& AspectRatioSetting);

void SuspendParentProcess(DWORD targetProcessId, DWORD targetThreadId, bool action)
{
    HANDLE h = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (h != INVALID_HANDLE_VALUE)
    {
        THREADENTRY32 te;
        te.dwSize = sizeof(te);
        if (Thread32First(h, &te))
        {
            do
            {
                if (te.dwSize >= FIELD_OFFSET(THREADENTRY32, th32OwnerProcessID) + sizeof(te.th32OwnerProcessID))
                {
                    if (te.th32ThreadID != targetThreadId && te.th32OwnerProcessID == targetProcessId)
                    {
                        HANDLE thread = ::OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                        if (thread != NULL)
                        {
                            if (action)
                                SuspendThread(thread);
                            else
                                ResumeThread(thread);
                            CloseHandle(thread);
                        }
                    }
                }
                te.dwSize = sizeof(te);
            } while (Thread32Next(h, &te));
        }
        CloseHandle(h);
    }
}

void ElfSwitchWatcher(std::future<void> futureObj, uint32_t* addr, uint32_t data, uint32_t& crc, uintptr_t EEMainMemoryStart, size_t EEMainMemorySize, uintptr_t GameElfTextBase, uintptr_t GameElfTextSize, bool bEnableEE128mbRam, int& WindowSizeX, int& WindowSizeY, bool& IsFullscreen, AspectRatioType& AspectRatioSetting)
{
    spd::log()->info("Starting thread ElfSwitchWatcher");
    volatile auto cur_crc = crc;
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
                    {
                        spd::log()->info("Ending thread ElfSwitchWatcher");
                        return;
                    }
                }
                SuspendParentProcess(GetCurrentProcessId(), GetCurrentThreadId(), true);
                LoadPlugins(crc, EEMainMemoryStart, EEMainMemorySize, GameElfTextBase, GameElfTextSize, bEnableEE128mbRam, WindowSizeX, WindowSizeY, IsFullscreen, AspectRatioSetting);
                SuspendParentProcess(GetCurrentProcessId(), GetCurrentThreadId(), false);
                spd::log()->info("Ending thread ElfSwitchWatcher");
                return;
            }
        }
        else
        {
            break;
        }
        std::this_thread::yield();
    }
    spd::log()->info("Ending thread ElfSwitchWatcher");
}

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

void RegisterInputDevices(HWND hWnd)
{
    constexpr auto HID_USAGE_PAGE_GENERIC = 0x01;
    constexpr auto HID_USAGE_GENERIC_MOUSE = 0x02;
    constexpr auto HID_USAGE_GENERIC_KEYBOARD = 0x06;
    RAWINPUTDEVICE Rid[2] = {};
    Rid[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    Rid[0].usUsage = HID_USAGE_GENERIC_KEYBOARD;
    Rid[0].dwFlags = 0;
    Rid[0].hwndTarget = hWnd;
    Rid[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    Rid[1].usUsage = HID_USAGE_GENERIC_MOUSE;
    Rid[1].dwFlags = RIDEV_INPUTSINK;
    Rid[1].hwndTarget = hWnd;
    RegisterRawInputDevices(&Rid[0], 1, sizeof(Rid[0]));
    RegisterRawInputDevices(&Rid[1], 1, sizeof(Rid[1]));
}

enum
{
    OSDStringSize = 255
};

enum KeyboardBufState
{
    CurrentState,
    PreviousState,

    StateNum,

    StateSize = 256
};

enum PtrType
{
    KeyboardData,
    MouseData
};

std::vector<std::pair<uintptr_t, std::pair<size_t, uint8_t>>> kbd_ptrs;
LRESULT(WINAPI* WndProc)(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI CustomWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_ACTIVATE && wParam == WA_ACTIVE)
    {
        RegisterInputDevices(hWnd);
    }
    else if (msg == WM_INPUT && hWnd == GetActiveWindow())
    {
        for (auto& it : kbd_ptrs)
        {
            MEMORY_BASIC_INFORMATION MemoryInf;
            if ((VirtualQuery((LPCVOID)it.first, &MemoryInf, sizeof(MemoryInf)) != 0 && MemoryInf.Protect != 0))
            {
                auto VKeyStates = reinterpret_cast<char*>(it.first);
                auto VKeyStatesPrev = reinterpret_cast<char*>(it.first + KeyboardBufState::StateSize);
                UINT dwSize = 0;

                GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
                static std::vector<uint8_t> lbp(dwSize);
                auto raw = reinterpret_cast<RAWINPUT*>(lbp.data());

                if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, raw, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize)
                {
                    OutputDebugString(TEXT("GetRawInputData does not return correct size !\n"));
                }

                if (raw->header.dwType == RIM_TYPEKEYBOARD)
                {
                    if (it.second.second == KeyboardData && raw->header.hDevice)
                    {
                        switch (raw->data.keyboard.VKey)
                        {
                        case VK_CONTROL:
                            if (raw->data.keyboard.Flags & RI_KEY_E0)
                            {
                                VKeyStatesPrev[VK_RCONTROL] = VKeyStates[VK_RCONTROL];
                                VKeyStates[VK_RCONTROL] = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
                            }
                            else
                            {
                                VKeyStatesPrev[VK_LCONTROL] = VKeyStates[VK_LCONTROL];
                                VKeyStates[VK_LCONTROL] = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
                            }
                            break;
                        case VK_MENU:
                            if (raw->data.keyboard.Flags & RI_KEY_E0)
                            {
                                VKeyStatesPrev[VK_RMENU] = VKeyStates[VK_RMENU];
                                VKeyStates[VK_RMENU] = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
                            }
                            else
                            {
                                VKeyStatesPrev[VK_LMENU] = VKeyStates[VK_LMENU];
                                VKeyStates[VK_LMENU] = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
                            }
                            break;
                        case VK_SHIFT:
                            if (raw->data.keyboard.MakeCode == 0x36)
                            {
                                VKeyStatesPrev[VK_RSHIFT] = VKeyStates[VK_RSHIFT];
                                VKeyStates[VK_RSHIFT] = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
                            }
                            else
                            {
                                VKeyStatesPrev[VK_LSHIFT] = VKeyStates[VK_LSHIFT];
                                VKeyStates[VK_LSHIFT] = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
                            }
                            break;
                        default:
                            VKeyStatesPrev[raw->data.keyboard.VKey] = VKeyStates[raw->data.keyboard.VKey];
                            VKeyStates[raw->data.keyboard.VKey] = !(raw->data.keyboard.Flags & RI_KEY_BREAK);
                            break;
                        }
                    }
                }
                else if (raw->header.dwType == RIM_TYPEMOUSE)
                {
                    if (it.second.second == MouseData && raw->header.hDevice)
                    {
                        CMouseControllerState& StateBuf = *reinterpret_cast<CMouseControllerState*>(it.first);
                        CMouseControllerState& StateBufPrev = *reinterpret_cast<CMouseControllerState*>(it.first + sizeof(CMouseControllerState));

                        StateBufPrev = StateBuf;

                        // Movement
                        StateBuf.X += static_cast<float>(raw->data.mouse.lLastX);
                        StateBuf.Y += static_cast<float>(raw->data.mouse.lLastY);

                        // LMB
                        if (!StateBuf.lmb)
                            StateBuf.lmb = (raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != false;
                        else
                            StateBuf.lmb = (raw->data.mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) == false;

                        // RMB
                        if (!StateBuf.rmb)
                            StateBuf.rmb = (raw->data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) != false;
                        else
                            StateBuf.rmb = (raw->data.mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) == false;

                        // MMB
                        if (!StateBuf.mmb)
                            StateBuf.mmb = (raw->data.mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != false;
                        else
                            StateBuf.mmb = (raw->data.mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) == false;

                        // 4th button
                        if (!StateBuf.bmx1)
                            StateBuf.bmx1 = (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) != false;
                        else
                            StateBuf.bmx1 = (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP) == false;

                        // 5th button
                        if (!StateBuf.bmx2)
                            StateBuf.bmx2 = (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) != false;
                        else
                            StateBuf.bmx2 = (raw->data.mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP) == false;

                        // Scroll
                        if (raw->data.mouse.usButtonFlags & RI_MOUSE_WHEEL)
                        {
                            StateBuf.Z += static_cast<signed short>(raw->data.mouse.usButtonData);
                            if (StateBuf.Z < 0.0f)
                                StateBuf.wheelDown = true;
                            else if (StateBuf.Z > 0.0f)
                                StateBuf.wheelUp = true;
                        }
                    }
                }
            }
        }
    }
    return WndProc(hWnd, msg, wParam, lParam);
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

PluginInfo ParseElf(auto path)
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
                    else if (name == "PCSX2Data")
                    {
                        info.PCSX2DataAddr = value;
                        info.PCSX2DataSize = size;
                    }
                    else if (name == "CompatibleCRCList")
                    {
                        info.CompatibleCRCListAddr = value;
                        info.CompatibleCRCListSize = size;
                    }
                    else if (name == "ElfPattern")
                    {
                        info.PatternDataAddr = value;
                        info.PatternDataSize = size;
                    }
                    else if (name == "KeyboardState")
                    {
                        info.KeyboardStateAddr = value;
                        info.KeyboardStateSize = size;
                    }
                    else if (name == "MouseState")
                    {
                        info.MouseStateAddr = value;
                        info.MouseStateSize = size;
                    }
                    else if (name == "OSDText")
                    {
                        info.OSDTextAddr = value;
                        info.OSDTextSize = size;
                    }
                }
            }
        }
        info.EntryPoint = reader.get_entry();
    }
    return info;
}

void LoadPlugins(uint32_t& crc, uintptr_t EEMainMemoryStart, size_t EEMainMemorySize, uintptr_t GameElfTextBase, uintptr_t GameElfTextSize, bool bEnableEE128mbRam, int& WindowSizeX, int& WindowSizeY, bool& IsFullscreen, AspectRatioType& AspectRatioSetting)
{
    spd::log()->info("Starting PCSX2PluginInjector, game crc: 0x{:X}", crc);
    spd::log()->info("EE Memory starts at: 0x{:X}", EEMainMemoryStart);
    spd::log()->info("Game Base Address: 0x{:X}", GameElfTextBase);
    spd::log()->info("Game Region End: 0x{:X}", GameElfTextBase + GameElfTextSize);

    exitSignal.set_value();
    std::promise<void>().swap(exitSignal);
    GetOSDVector().clear();
    kbd_ptrs.clear();
    uint32_t* ei_hook = nullptr;
    uint32_t ei_data = 0;
    uint32_t NewMinBase = 0;

    wchar_t buf[MAX_PATH];
    HMODULE hm = NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&LoadPlugins, &hm);
    GetModuleFileNameW(hm, buf, sizeof(buf));
    auto modulePath = std::filesystem::path(buf);
    auto pluginsPath = modulePath.remove_filename() / L"PLUGINS/";
    auto invokerPath = pluginsPath / L"PCSX2PluginInvoker.elf";
    auto _32mb = 0x02000000;

    if (std::filesystem::exists(pluginsPath))
    {
        spd::log()->info("Loading {}", invokerPath.filename().string());
        PluginInfo invoker = { 0 };
        std::vector<char> buffer;
        if (std::filesystem::exists(invokerPath))
        {
            invoker = ParseElf(invokerPath.string());
            buffer = LoadFileToBuffer(invokerPath);
        }
        else
        {
            HRSRC hResource = FindResource(hm, MAKEINTRESOURCE(IDR_INVOKER), RT_RCDATA);
            if (hResource)
            {
                HGLOBAL hLoadedResource = LoadResource(hm, hResource);
                if (hLoadedResource)
                {
                    LPVOID pLockedResource = LockResource(hLoadedResource);
                    if (pLockedResource)
                    {
                        size_t dwResourceSize = SizeofResource(hm, hResource);
                        if (dwResourceSize)
                        {
                            std::string rsrc(static_cast<char*>(pLockedResource), dwResourceSize);
                            std::istringstream iss(rsrc);
                            invoker = ParseElf(std::ref(iss));
                            buffer = std::vector<char>(rsrc.begin(), rsrc.end());
                        }
                    }
                }
            }
        }

        if (!invoker.isValid() || buffer.empty())
        {
            spd::log()->error("{} could not be loaded, exiting...", invokerPath.filename().string());
            return;
        }

        spd::log()->info("{} base address: 0x{:X}", invokerPath.filename().string(), invoker.Base);
        spd::log()->info("{} entry point: 0x{:X}", invokerPath.filename().string(), invoker.EntryPoint);

        if (invoker.Base < _32mb)
        {
            spd::log()->warn("{} base address is within main memory, it may be overwritten by the game", invokerPath.filename().string());
        }
        else if (bEnableEE128mbRam != true)
        {
            spd::log()->error("\"Enable 128 MB of RAM\" option is not set in PCSX2 settings (EE/IOP), it is required to be on");
            return;
        }

        auto count = 0;

        spd::log()->info("Injecting {}...", invokerPath.filename().string());
        injector::WriteMemoryRaw(EEMainMemoryStart + invoker.Base, buffer.data() + invoker.SegmentFileOffset, buffer.size() - invoker.SegmentFileOffset, true);
        injector::WriteMemoryRaw(EEMainMemoryStart + invoker.DataAddr + (sizeof(invoker) * count), &invoker, sizeof(invoker), true);
        spd::log()->info("Finished injecting {}, {} bytes written at 0x{:X}", invokerPath.filename().string(), invoker.Size, invoker.Base);
        if (NewMinBase < invoker.Base + invoker.Size) NewMinBase = invoker.Base + invoker.Size;

        spd::log()->info("Hooking game's entry point function...", invokerPath.filename().string());
        auto patched = false;
        constexpr auto start_pattern = "28 ? 00 70 28 ? 00 70 28 ? 00 70 28 ? 00 70 28";
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
            spd::log()->error("{} can't hook the game with CRC {}, exiting...", modulePath.filename().string(), crc);
            return;
        }

        spd::log()->info("Finished hooking entry point function at 0x{:X}", (uintptr_t)ei_hook - EEMainMemoryStart);
        spd::log()->info("Looking for plugins in {}", pluginsPath.parent_path().filename().string());

        bool elf_thread_created = false;

        for (const auto& file : std::filesystem::recursive_directory_iterator(pluginsPath, std::filesystem::directory_options::skip_permission_denied))
        {
            if (!std::filesystem::is_directory(file) && file.is_regular_file() && file.path() != invokerPath && iequals(file.path().extension().wstring(), L".elf"))
            {
                auto plugin_path = file.path().parent_path().filename() / file.path().filename().string();
                PluginInfo mod = ParseElf(file.path().string());

                if (mod.CompatibleCRCListAddr && mod.CompatibleCRCListSize)
                {
                    auto buffer = LoadFileToBuffer(file.path().string());
                    if (buffer.empty())
                        continue;

                    bool crc_compatible = false;
                    uint32_t* crc_array = (uint32_t*)(buffer.data() + mod.SegmentFileOffset + mod.CompatibleCRCListAddr - mod.Base);
                    for (uint32_t i = 0; i < mod.CompatibleCRCListSize / sizeof(uint32_t); i++)
                    {
                        if (crc_array[i] == crc)
                        {
                            crc_compatible = true;
                            break;
                        }
                    }

                    if (!crc_compatible)
                        continue;

                    spd::log()->info("Loading {}", plugin_path.string());
                    if (!mod.isValid() || mod.Base < _32mb)
                    {
                        spd::log()->warn("{} could not be loaded", plugin_path.string());
                        if (mod.Base < _32mb)
                            spd::log()->error("{} base address can't be less than 32mb", file.path().filename().string());
                        continue;
                    }

                    spd::log()->info("{} base address: 0x{:X}", file.path().filename().string(), mod.Base);
                    spd::log()->info("{} entry point: 0x{:X}", file.path().filename().string(), mod.EntryPoint);
                    spd::log()->info("{} size: {} bytes", file.path().filename().string(), mod.Size);

                    if (mod.PatternDataAddr)
                    {
                        if (!elf_thread_created)
                        {
                            spd::log()->info("Some plugins has to be loaded in another elf, creating thread to handle it");
                            std::future<void> futureObj = exitSignal.get_future();
                            std::thread th(&ElfSwitchWatcher, std::move(futureObj), ei_hook, ei_data, std::ref(crc), EEMainMemoryStart, EEMainMemorySize, GameElfTextBase, GameElfTextSize, bEnableEE128mbRam, std::ref(WindowSizeX), std::ref(WindowSizeY), std::ref(IsFullscreen), std::ref(AspectRatioSetting));
                            th.detach();
                            elf_thread_created = true;
                        }

                        std::string_view pattern_str((char*)(buffer.data() + mod.SegmentFileOffset + mod.PatternDataAddr - mod.Base));
                        if (!pattern_str.empty())
                        {
                            auto pattern = hook::pattern(EEMainMemoryStart, EEMainMemoryStart + EEMainMemorySize, pattern_str);
                            if (pattern.empty())
                            {
                                spd::log()->warn("Pattern \"{}\" is not found in this elf, {} will not be loaded at this time", pattern_str, file.path().filename().string());
                                continue;
                            }
                        }
                    }

                    count++;
                    spd::log()->info("Injecting {}...", plugin_path.filename().string());
                    injector::WriteMemoryRaw(EEMainMemoryStart + mod.Base, buffer.data() + mod.SegmentFileOffset, buffer.size() - mod.SegmentFileOffset, true);
                    injector::WriteMemoryRaw(EEMainMemoryStart + invoker.DataAddr + (sizeof(invoker) * count), &mod, sizeof(mod), true);
                    spd::log()->info("Finished injecting {}, {} bytes written at 0x{:X}", plugin_path.filename().string(), mod.Size, mod.Base);
                    if (NewMinBase < mod.Base + mod.Size) NewMinBase = mod.Base + mod.Size;

                    auto iniPath = std::filesystem::path(file.path()).replace_extension(L".ini");
                    if (std::filesystem::exists(iniPath))
                    {
                        spd::log()->info("Loading {}", iniPath.filename().string());
                        if (mod.DataAddr)
                        {
                            auto ini = LoadFileToBuffer(iniPath);
                            spd::log()->info("Injecting {}...", iniPath.filename().string());
                            ini.resize(mod.DataSize - sizeof(uint32_t));
                            injector::WriteMemory(EEMainMemoryStart + mod.DataAddr, ini.size(), true);
                            injector::WriteMemoryRaw(EEMainMemoryStart + mod.DataAddr + sizeof(uint32_t), ini.data(), ini.size(), true);
                            spd::log()->info("{} was successfully injected", iniPath.filename().string());
                        }
                    }

                    if (mod.PCSX2DataAddr)
                    {
                        spd::log()->info("Writing PCSX2 Data to {}", plugin_path.filename().string());
                        auto [DesktopSizeX, DesktopSizeY] = GetDesktopRes();
                        injector::WriteMemory(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_DesktopSizeX), (uint32_t)DesktopSizeX, true);
                        injector::WriteMemory(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_DesktopSizeY), (uint32_t)DesktopSizeY, true);
                        injector::WriteMemory(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_WindowSizeX), (uint32_t)WindowSizeX, true);
                        injector::WriteMemory(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_WindowSizeY), (uint32_t)WindowSizeY, true);
                        injector::WriteMemory(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_IsFullscreen), (uint32_t)IsFullscreen, true);
                        injector::WriteMemory(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_AspectRatioSetting), (uint32_t)AspectRatioSetting, true);
                    }

                    if (mod.KeyboardStateAddr)
                    {
                        spd::log()->info("{} requests keyboard state", plugin_path.filename().string());
                        kbd_ptrs.emplace_back(EEMainMemoryStart + mod.KeyboardStateAddr, std::make_pair(mod.KeyboardStateSize, PtrType::KeyboardData));
                        injector::MemoryFill(EEMainMemoryStart + mod.KeyboardStateAddr, 0, mod.KeyboardStateSize, true);
                    }

                    if (mod.MouseStateAddr)
                    {
                        spd::log()->info("{} requests mouse state", plugin_path.filename().string());
                        kbd_ptrs.emplace_back(EEMainMemoryStart + mod.MouseStateAddr, std::make_pair(mod.MouseStateSize, PtrType::MouseData));
                        injector::MemoryFill(EEMainMemoryStart + mod.MouseStateAddr, 0, mod.MouseStateSize, true);
                    }

                    if (mod.OSDTextAddr)
                    {
                        spd::log()->info("{} requests OSD drawings", plugin_path.filename().string());
                        for (size_t i = 0; i < mod.OSDTextSize / OSDStringSize; i++)
                        {
                            auto block = (char*)(EEMainMemoryStart + mod.OSDTextAddr + (OSDStringSize * i));
                            injector::MemoryFill(block, 0, OSDStringSize, true);
                            GetOSDVector().emplace_back(std::string_view(block, OSDStringSize));
                        }
                    }
                }
            }
        }

        if (!kbd_ptrs.empty())
        {
            auto GetHwnd = [](DWORD dwProcessID) -> HWND
            {
                HWND hCurWnd = nullptr;
                do
                {
                    hCurWnd = FindWindowEx(nullptr, hCurWnd, nullptr, nullptr);
                    DWORD checkProcessID = 0;
                    GetWindowThreadProcessId(hCurWnd, &checkProcessID);
                    if (checkProcessID == dwProcessID)
                    {
                        std::wstring title(GetWindowTextLength(hCurWnd) + 1, L'\0');
                        GetWindowTextW(hCurWnd, &title[0], title.size());
                        if (title.starts_with(L"Slot:") || title.starts_with(L"Booting PS2 BIOS..."))
                            return hCurWnd;
                    }
                } while (hCurWnd != nullptr);
                return NULL;
            };

            auto hwnd = GetHwnd(GetCurrentProcessId());
            if (hwnd)
            {
                RegisterInputDevices(hwnd);
                auto wp = (LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM))GetWindowLongPtr(hwnd, GWLP_WNDPROC);
                if (wp != CustomWndProc)
                {
                    spd::log()->info("Keyboard and mouse data requested by plugins, replacing WndProc for HWND {}", (uint32_t)hwnd);
                    WndProc = wp;
                    SetWindowLongPtr(hwnd, GWLP_WNDPROC, (LONG_PTR)&CustomWndProc);
                }
            }
        }

        spd::log()->info("Suggested minimum base address for new plugins: 0x{:X}", NewMinBase);
    }
    else
    {
        spd::log()->error("{} directory does not exist", pluginsPath.filename().string());
    }
    spd::log()->info("Finished loading plugins\n");
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
