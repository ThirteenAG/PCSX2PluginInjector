#include <elfio/elfio.hpp>
#include "stdafx.h"
#include <thread>
#include <iostream>
#include <chrono>
#include <future>
#include <filesystem>
#include <tlhelp32.h>
#include <pcsx2/mips.hpp>

#include <pcsx2f_api.h>

#define IDR_INVOKER    101

std::promise<void> exitSignal;
uintptr_t gEEMainMemoryStart;
size_t gEEMainMemorySize;
const void* gWindowHandle;

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

CEXP void LoadPlugins(
    const char* s_disc_serial,
    const char* s_disc_elf,
    const char* s_disc_version,
    const char* s_title,
    const char* s_elf_path,
    const uint32_t& s_disc_crc,
    const uint32_t& s_current_crc,
    const uint32_t& s_elf_entry_point,
    uint8_t* EEMainMemoryStart,
    size_t EEMainMemorySize,
    const void* WindowHandle,
    const uint32_t& WindowSizeX,
    const uint32_t& WindowSizeY,
    const bool IsFullscreen,
    const uint8_t& AspectRatioSetting,
    bool& FrameLimitUnthrottle
);

enum class VMState
{
    Shutdown,
    Initializing,
    Running,
    Paused,
    Resetting,
    Stopping,
};

std::atomic<VMState>* s_state;

CEXP bool VMStateIsRunning()
{
    if (s_state)
        return s_state->load(std::memory_order_acquire) == VMState::Running;
    return true;
}

CEXP void SetVMStatePtr(std::atomic<VMState>* ptr)
{
    s_state = ptr;
}

void UnthrottleWatcher(std::future<void> futureObj, uint8_t* addr, const uint32_t& crc, bool& FrameLimitUnthrottle)
{
    [&]()
    {
        __try
        {
            spd::log()->info("Starting thread UnthrottleWatcher");
            volatile auto cur_crc = crc;
            while (futureObj.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout)
            {
                MEMORY_BASIC_INFORMATION MemoryInf;
                if (cur_crc != crc || (VirtualQuery((LPCVOID)addr, &MemoryInf, sizeof(MemoryInf)) != 0 && MemoryInf.Protect != 0))
                    FrameLimitUnthrottle = *addr != 0;
                else
                    break;
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            FrameLimitUnthrottle = false;
            spd::log()->info("Ending thread UnthrottleWatcher");
        }
        __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }
    }();
}

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

HWND* gHWND;
struct InputDataT
{
    uint8_t Type;
    uintptr_t Addr;
    size_t Size;
}; std::vector<InputDataT> InputData;
LRESULT(WINAPI* WndProc)(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);
LRESULT WINAPI CustomWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    std::vector<uint8_t> lbp(255);
    [&]()
    {
        __try
        {
            if (msg == WM_ACTIVATE)
            {
                switch (wParam)
                {
                case WA_ACTIVE:
                case WA_CLICKACTIVE:
                    RegisterInputDevices(hWnd);
                    break;
                case WA_INACTIVE:
                    for (auto& it : InputData)
                    {
                        if (it.Type != PtrType::CheatStringData)
                        {
                            MEMORY_BASIC_INFORMATION MemoryInf;
                            if ((VirtualQuery((LPCVOID)it.Addr, &MemoryInf, sizeof(MemoryInf)) != 0 && MemoryInf.Protect != 0))
                            {
                                injector::MemoryFill(it.Addr, 0x00, it.Size, true);
                            }
                        }
                    }
                    break;
                }
            }
            else if (msg == WM_INPUT)
            {
                auto awnd = GetActiveWindow();
                if (hWnd == awnd || *gHWND == awnd)
                {
                    for (auto& it : InputData)
                    {
                        MEMORY_BASIC_INFORMATION MemoryInf;
                        if ((VirtualQuery((LPCVOID)it.Addr, &MemoryInf, sizeof(MemoryInf)) != 0 && MemoryInf.Protect != 0))
                        {
                            auto VKeyStates = reinterpret_cast<char*>(it.Addr);
                            auto VKeyStatesPrev = reinterpret_cast<char*>(it.Addr + KeyboardBufState::StateSize);
                            UINT dwSize = 0;

                            GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, NULL, &dwSize, sizeof(RAWINPUTHEADER));
                            lbp.resize(dwSize);
                            auto raw = reinterpret_cast<RAWINPUT*>(lbp.data());

                            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, raw, &dwSize, sizeof(RAWINPUTHEADER)) != dwSize)
                            {
                                OutputDebugString(TEXT("GetRawInputData does not return correct size !\n"));
                            }

                            if (raw->header.dwType == RIM_TYPEKEYBOARD)
                            {
                                if (raw->header.hDevice)
                                {
                                    if (it.Type == PtrType::KeyboardData)
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
                                    else if (it.Type == PtrType::CheatStringData)
                                    {
                                        if (raw->data.keyboard.Flags & RI_KEY_BREAK)
                                        {
                                            auto keycode = raw->data.keyboard.VKey;
                                            if ((keycode > 47 && keycode < 58) || (keycode > 64 && keycode < 91)) // number or letter keys
                                            {
                                                std::memcpy(&VKeyStates[1], &VKeyStates[0], it.Size - 2);
                                                VKeyStates[0] = raw->data.keyboard.VKey;
                                                VKeyStates[it.Size - 1] = 0;
                                            }
                                            else
                                            {
                                                VKeyStates[0] = 0;
                                            }
                                        }
                                    }
                                }
                            }
                            else if (raw->header.dwType == RIM_TYPEMOUSE)
                            {
                                if (it.Type == PtrType::MouseData && raw->header.hDevice)
                                {
                                    CMouseControllerState& StateBuf = *reinterpret_cast<CMouseControllerState*>(it.Addr);
                                    CMouseControllerState& StateBufPrev = *reinterpret_cast<CMouseControllerState*>(it.Addr + sizeof(CMouseControllerState));

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
            }
        }
        __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }
    }();

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

    PluginInfo info = {};

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
                        info.PluginDataAddr = value;
                        info.PluginDataSize = size;
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
                    else if (name == "CompatibleElfCRCList")
                    {
                        info.CompatibleElfCRCListAddr = value;
                        info.CompatibleElfCRCListSize = size;
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
                    else if (name == "CheatString")
                    {
                        info.CheatStringAddr = value;
                        info.CheatStringSize = size;
                    }
                    else if (name == "OSDText")
                    {
                        info.OSDTextAddr = value;
                        info.OSDTextSize = size;
                    }
                    else if (name == "FrameLimitUnthrottle")
                    {
                        info.FrameLimitUnthrottleAddr = value;
                        info.FrameLimitUnthrottleSize = size;
                    }
                    else if (name == "CLEOScripts")
                    {
                        info.CLEOScriptsAddr = value;
                        info.CLEOScriptsSize = size;
                    }
                }
            }
        }
        info.EntryPoint = reader.get_entry();
    }
    return info;
}

void LoadPlugins(
    const char* s_disc_serial,
    const char* s_disc_elf,
    const char* s_disc_version,
    const char* s_title,
    const char* s_elf_path,
    const uint32_t& s_disc_crc,
    const uint32_t& s_current_crc,
    const uint32_t& s_elf_entry_point,
    uint8_t* EEMainMemoryStart,
    size_t EEMainMemorySize,
    const void* WindowHandle,
    const uint32_t& WindowSizeX,
    const uint32_t& WindowSizeY,
    const bool IsFullscreen,
    const uint8_t& AspectRatioSetting,
    bool& FrameLimitUnthrottle
)
{
    spd::log()->info("Starting PCSX2PluginInjector");
    spd::log()->info("Game: {}", s_title);
    spd::log()->info("Disc Serial: {}", s_disc_serial);
    spd::log()->info("Disc ELF: {}", s_disc_elf);
    spd::log()->info("Disc Version: {}", s_disc_version);
    spd::log()->info("ELF Path: {}", s_elf_path);
    spd::log()->info("Disc CRC: 0x{:X}", s_disc_crc);
    spd::log()->info("Current ELF CRC: 0x{:X}", s_current_crc);
    spd::log()->info("ELF Entry Point Address: 0x{:X}", s_elf_entry_point);
    spd::log()->info("EE Memory starts at: 0x{:X}", (uintptr_t)EEMainMemoryStart);

    gEEMainMemoryStart = (uintptr_t)EEMainMemoryStart;
    gEEMainMemorySize = (size_t)EEMainMemorySize;
    gWindowHandle = WindowHandle;
    exitSignal.set_value();
    std::promise<void>().swap(exitSignal);
    GetOSDVector().clear();
    InputData.clear();
    uint32_t* ei_hook = nullptr;
    uint32_t ei_data = 0;
    std::vector<std::pair<uintptr_t, uintptr_t>> PluginRegions = { { 0, EEMainMemorySize } };
    std::error_code ec;

    auto modulePath = std::filesystem::path(GetThisModulePath<std::wstring>());
    auto pluginsPath = modulePath.remove_filename() / L"PLUGINS/";
    auto invokerPath = pluginsPath / L"PCSX2PluginInvoker.elf";

    if (std::filesystem::exists(pluginsPath, ec))
    {
        spd::log()->info("Loading {}", invokerPath.filename().string());
        PluginInfo invoker = { 0 };
        std::vector<char> buffer;
        if (std::filesystem::exists(invokerPath, ec))
        {
            invoker = ParseElf(invokerPath.string());
            buffer = LoadFileToBuffer(invokerPath);
        }
        else
        {
            HMODULE hm = NULL;
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCWSTR)&LoadPlugins, &hm);
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

        if (invoker.Base < 1024 * 1024 * 32)
        {
            spd::log()->warn("{} base address is within main memory, it may be overwritten by the game", invokerPath.filename().string());
        }

        auto count = 0;

        spd::log()->info("Injecting {}...", invokerPath.filename().string());
        injector::WriteMemoryRaw(EEMainMemoryStart + invoker.Base, buffer.data() + invoker.SegmentFileOffset, buffer.size() - invoker.SegmentFileOffset, true);
        injector::MemoryFill(EEMainMemoryStart + invoker.PluginDataAddr, 0x00, invoker.PluginDataSize, true);
        injector::WriteMemoryRaw(EEMainMemoryStart + invoker.PluginDataAddr + (sizeof(PluginInfoInvoker) * count), &invoker, sizeof(PluginInfoInvoker), true);
        spd::log()->info("Finished injecting {}, {} bytes written at 0x{:X}", invokerPath.filename().string(), invoker.Size, invoker.Base);
        PluginRegions.emplace_back(invoker.Base, invoker.Base + invoker.Size);

        spd::log()->info("Hooking game's entry point function...", invokerPath.filename().string());
        auto patched = false;
        
        auto ei_lookup = hook::pattern((uintptr_t)(EEMainMemoryStart + s_elf_entry_point), (uintptr_t)(EEMainMemoryStart + s_elf_entry_point + 2000), "38 00 00 42");
        if (!ei_lookup.empty())
        {
            ei_hook = ei_lookup.get_first<uint32_t>();
            ei_data = mips::jal(invoker.EntryPoint);
            injector::WriteMemory<uint32_t>(ei_hook, ei_data, true);
            patched = true;
        }
       
        if (!patched)
        {
            spd::log()->error("{} can't hook the game with Disc CRC 0x{:X}, ELF CRC 0x{:X}, exiting...", modulePath.filename().string(), s_disc_crc, s_current_crc);
            return;
        }

        spd::log()->info("Finished hooking entry point function at 0x{:X}", (uintptr_t)ei_hook - (uintptr_t)EEMainMemoryStart);
        spd::log()->info("Looking for plugins in {}", pluginsPath.parent_path().filename().string());

        bool fl_thread_created = false;

        for (const auto& file : std::filesystem::recursive_directory_iterator(pluginsPath, std::filesystem::directory_options::skip_permission_denied, ec))
        {
            if (!std::filesystem::is_directory(file, ec) && file.is_regular_file(ec) && file.path() != invokerPath && iequals(file.path().extension().wstring(), L".elf"))
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
                        if (crc_array[i] == s_disc_crc)
                        {
                            crc_compatible = true;
                            break;
                        }
                    }

                    if (crc_compatible && mod.CompatibleElfCRCListAddr)
                    {
                        crc_compatible = false;
                        uint32_t* elf_crc_array = (uint32_t*)(buffer.data() + mod.SegmentFileOffset + mod.CompatibleElfCRCListAddr - mod.Base);
                        for (uint32_t i = 0; i < mod.CompatibleElfCRCListSize / sizeof(uint32_t); i++)
                        {
                            if (elf_crc_array[i] == s_current_crc)
                            {
                                crc_compatible = true;
                                break;
                            }
                        }
                    }

                    if (!crc_compatible)
                        continue;

                    auto BaseCheck = std::find_if(PluginRegions.begin(), PluginRegions.end(), [&mod](auto x) {
                        return x.first >= mod.Base && x.second <= mod.Base + mod.Size; 
                    }) != PluginRegions.end();

                    spd::log()->info("Loading {}", plugin_path.string());
                    if (!mod.isValid() || BaseCheck)
                    {
                        spd::log()->warn("{} could not be loaded", plugin_path.string());
                        if (BaseCheck)
                            spd::log()->error("{} base address can't be 0x{:X}, you have conflicting or invalid plugins", file.path().filename().string(), mod.Base);
                        continue;
                    }

                    spd::log()->info("{} base address: 0x{:X}", file.path().filename().string(), mod.Base);
                    spd::log()->info("{} entry point: 0x{:X}", file.path().filename().string(), mod.EntryPoint);
                    spd::log()->info("{} size: {} bytes", file.path().filename().string(), mod.Size);

                    count++;
                    spd::log()->info("Injecting {}...", plugin_path.filename().string());
                    injector::WriteMemoryRaw(EEMainMemoryStart + mod.Base, buffer.data() + mod.SegmentFileOffset, buffer.size() - mod.SegmentFileOffset, true);
                    injector::WriteMemoryRaw(EEMainMemoryStart + invoker.PluginDataAddr + (sizeof(PluginInfoInvoker) * count), &mod, sizeof(PluginInfoInvoker), true);
                    spd::log()->info("Finished injecting {}, {} bytes written at 0x{:X}", plugin_path.filename().string(), mod.Size, mod.Base);
                    PluginRegions.emplace_back(mod.Base, mod.Base + mod.Size);

                    auto iniPath = std::filesystem::path(file.path()).replace_extension(L".ini");
                    if (std::filesystem::exists(iniPath, ec))
                    {
                        spd::log()->info("Loading {}", iniPath.filename().string());
                        if (mod.PluginDataAddr)
                        {
                            auto ini = LoadFileToBuffer(iniPath);
                            spd::log()->info("Injecting {}...", iniPath.filename().string());
                            ini.resize(mod.PluginDataSize - sizeof(uint32_t));
                            injector::WriteMemory<uint32_t>(EEMainMemoryStart + mod.PluginDataAddr, ini.size(), true);
                            injector::WriteMemoryRaw(EEMainMemoryStart + mod.PluginDataAddr + sizeof(uint32_t), ini.data(), ini.size(), true);
                            spd::log()->info("{} was successfully injected", iniPath.filename().string());
                        }
                    }

                    if (mod.PCSX2DataAddr)
                    {
                        spd::log()->info("Writing PCSX2 Data to {}", plugin_path.filename().string());
                        auto [DesktopSizeX, DesktopSizeY] = GetDesktopRes();
                        injector::WriteMemory<uint32_t>(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_DesktopSizeX), (uint32_t)DesktopSizeX, true);
                        injector::WriteMemory<uint32_t>(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_DesktopSizeY), (uint32_t)DesktopSizeY, true);
                        injector::WriteMemory<uint32_t>(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_WindowSizeX), (uint32_t)WindowSizeX, true);
                        injector::WriteMemory<uint32_t>(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_WindowSizeY), (uint32_t)WindowSizeY, true);
                        injector::WriteMemory<uint32_t>(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_IsFullscreen), (uint32_t)IsFullscreen, true);
                        injector::WriteMemory<uint32_t>(EEMainMemoryStart + mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_AspectRatioSetting), (uint32_t)AspectRatioSetting, true);
                    }

                    if (mod.KeyboardStateAddr)
                    {
                        spd::log()->info("{} requests keyboard state", plugin_path.filename().string());
                        InputData.emplace_back(PtrType::KeyboardData, (uintptr_t)(EEMainMemoryStart + mod.KeyboardStateAddr), mod.KeyboardStateSize);
                        injector::MemoryFill(EEMainMemoryStart + mod.KeyboardStateAddr, 0, mod.KeyboardStateSize, true);
                    }

                    if (mod.MouseStateAddr)
                    {
                        spd::log()->info("{} requests mouse state", plugin_path.filename().string());
                        InputData.emplace_back(PtrType::MouseData, (uintptr_t)(EEMainMemoryStart + mod.MouseStateAddr), mod.MouseStateSize);
                        injector::MemoryFill(EEMainMemoryStart + mod.MouseStateAddr, 0, mod.MouseStateSize, true);
                    }

                    if (mod.CheatStringAddr)
                    {
                        spd::log()->info("{} requests cheat string access", plugin_path.filename().string());
                        InputData.emplace_back(PtrType::CheatStringData, (uintptr_t)(EEMainMemoryStart + mod.CheatStringAddr), mod.CheatStringSize);
                        injector::MemoryFill(EEMainMemoryStart + mod.CheatStringAddr, 0, mod.CheatStringSize, true);
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

                    if (mod.FrameLimitUnthrottleAddr)
                    {
                        if (!fl_thread_created)
                        {
                            spd::log()->info("Some plugins can manage emulator's speed, creating thread to handle it");
                            injector::MemoryFill(EEMainMemoryStart + mod.FrameLimitUnthrottleAddr, 0x00, mod.FrameLimitUnthrottleSize, true);
                            std::future<void> futureObj = exitSignal.get_future();
                            std::thread th(&UnthrottleWatcher, std::move(futureObj), (uint8_t*)(EEMainMemoryStart + mod.FrameLimitUnthrottleAddr), std::ref(s_current_crc), std::ref(FrameLimitUnthrottle));
                            th.detach();
                            fl_thread_created = true;
                        }
                    }

                    if (mod.CLEOScriptsAddr)
                    {
                        spd::log()->info("CLEO Plugin detected, injecting CLEO Scripts");
                        injector::MemoryFill(EEMainMemoryStart + mod.CLEOScriptsAddr, 0x00, mod.CLEOScriptsSize, true);
                        auto script_offset = mod.CLEOScriptsAddr;
                        auto cleo_path = pluginsPath / L"CLEO";
                        if (std::filesystem::exists(cleo_path, ec))
                        {
                            for (const auto& entry : std::filesystem::directory_iterator(cleo_path, std::filesystem::directory_options::skip_permission_denied, ec))
                            {
                                auto ext = entry.path().extension().wstring();
                                if (iequals(ext, L".cs") || iequals(ext, L".csa") || iequals(ext, L".csi"))
                                {
                                    auto script = LoadFileToBuffer(entry.path());
                                    if (script_offset + sizeof(uint32_t) + script.size() <= mod.CLEOScriptsAddr + mod.CLEOScriptsSize)
                                    {
                                        spd::log()->info("Injecting {}", entry.path().filename().string());
                                        injector::WriteMemory<uint32_t>(EEMainMemoryStart + script_offset, script.size(), true);
                                        script_offset += sizeof(uint32_t);
                                        injector::WriteMemoryRaw(EEMainMemoryStart + script_offset, script.data(), script.size(), true);
                                        script_offset += script.size();
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        if (!InputData.empty())
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

            gHWND = (HWND*)WindowHandle;
            auto hwnd = *(HWND*)WindowHandle ? GetAncestor(reinterpret_cast<HWND>(*(HWND*)WindowHandle), GA_ROOT) : GetHwnd(GetCurrentProcessId());
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
        PluginRegions.erase(std::remove_if(PluginRegions.begin(), PluginRegions.end(), [](auto x) { return x.first == 0; }), PluginRegions.end());
        auto NewBase = std::max_element(PluginRegions.begin(), PluginRegions.end(), [](auto a, auto b) { return a.second < b.second; })->second + 1000;
        spd::log()->info("Suggested minimum base address for new plugins: 0x{:08X}", NewBase);
    }
    else
    {
        spd::log()->error("{} directory does not exist", pluginsPath.filename().string());
    }

    spd::log()->info("Finished loading plugins\n");

    auto XboxRainDroplets = L"PCSX2F.XboxRainDroplets64.asi";
    if (GetModuleHandle(XboxRainDroplets) == NULL)
    {
        auto h = LoadLibraryW(XboxRainDroplets);
        if (h != NULL)
        {
            auto procedure = (void(*)())GetProcAddress(h, "InitializeASI");
            if (procedure != NULL) {
                procedure();
            }
        }
    }
}

CEXP uintptr_t GetEEMainMemoryStart()
{
    return gEEMainMemoryStart;
}

CEXP size_t GetEEMainMemorySize()
{
    return gEEMainMemorySize;
}

CEXP const void* GetWindowHandle()
{
    return gWindowHandle;
}

CEXP uintptr_t GetPluginSymbolAddr(const char* path, const char* sym_name)
{
    using namespace ELFIO;

    elfio reader;
    uint32_t Size = 0;

    if (reader.load(path) && reader.get_class() == ELFCLASS32 && reader.get_encoding() == ELFDATA2LSB)
    {
        Elf_Half sec_num = reader.sections.size();
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

                    if (name == sym_name)
                    {
                        return value;
                    }
                }
            }
        }
    }
    return 0;
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

    if (reason == DLL_PROCESS_DETACH)
    {

    }
    return TRUE;
}
