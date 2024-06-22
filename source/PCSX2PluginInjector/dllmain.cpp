#include <elfio/elfio.hpp>
#include "stdafx.h"
#include <thread>
#include <iostream>
#include <chrono>
#include <future>
#include <filesystem>
#include <tlhelp32.h>
#include "safetyhook.hpp"
#include <pcsx2/mips.hpp>

#include <pcsx2f_api.h>

#define C_FFI
#include "pine.h"
PINE::PCSX2* ipc;

uint32_t FallbackEntryPointChecker;
HWND FallbackWindowHandle;
static BOOL CALLBACK EnumWindowsProc(HWND hwnd, LPARAM lParam)
{
    DWORD lpdwProcessId;
    GetWindowThreadProcessId(hwnd, &lpdwProcessId);
    auto str = reinterpret_cast<const char*>(lParam);

    if (lpdwProcessId == GetCurrentProcessId())
    {
        if (IsWindowVisible(hwnd))
        {
            std::string title(GetWindowTextLengthA(hwnd) + 1, '\0');
            GetWindowTextA(hwnd, title.data(), static_cast<int>(title.size()));
            if (title.contains(str) || title.starts_with("Slot:") || title.starts_with("Booting PS2 BIOS..."))
                FallbackWindowHandle = hwnd;
        }
    }
    return TRUE;
}

enum class VMState
{
    Shutdown,
    Initializing,
    Running,
    Paused,
    Resetting,
    Stopping,
};

using InitCB = void (*)(
    const char* s_disc_serial,
    const char* s_disc_elf,
    const char* s_disc_version,
    const char* s_title,
    const char* s_elf_path,
    const uint32_t s_disc_crc,
    const uint32_t s_current_crc,
    const uint32_t s_elf_entry_point,
    uint8_t* EEMainMemoryStart,
    size_t EEMainMemorySize,
    const void* pWindowHandle,
    const uint32_t WindowSizeX,
    const uint32_t WindowSizeY,
    const bool IsFullscreen,
    const uint8_t AspectRatioSetting);
using ShutdownCB = void (*)();

using tWriteBytes = void(*)(uint32_t mem, const void* src, uint32_t size);
using tGetIsThrottlerTempDisabled = bool(*)();
using tSetIsThrottlerTempDisabled = void(*)(bool);
using tGetVMState = VMState(*)();
using tAddOnGameElfInitCallback = void(*)(InitCB callback);
using tAddOnGameShutdownCallback = void(*)(ShutdownCB callback);

tWriteBytes WriteBytes = nullptr;
tGetIsThrottlerTempDisabled GetIsThrottlerTempDisabled = nullptr;
tSetIsThrottlerTempDisabled SetIsThrottlerTempDisabled = nullptr;
tGetVMState GetVMState = nullptr;
tAddOnGameElfInitCallback AddOnGameElfInitCallback = nullptr;
tAddOnGameShutdownCallback AddOnGameShutdownCallback = nullptr;

uintptr_t gEEMainMemoryStart;
size_t gEEMainMemorySize;

void MemoryFill(uint32_t addr, uint8_t value, uint32_t size)
{
    std::vector<uint8_t> temp(size, value);

    if (ipc)
    {
        //topkek
        //ipc->InitializeBatch();
        //for (auto i = 0; i < size; i++)
        //{
        //    ipc->Write<uint8_t, true>(addr + i, temp[i]);
        //}
        //ipc->SendCommand(ipc->FinalizeBatch());
        return injector::MemoryFill(addr + gEEMainMemoryStart, value, size, true);
    }

    WriteBytes(addr, temp.data(), static_cast<uint32_t>(temp.size()));
}

void WriteMemory32(uint32_t addr, uint32_t value)
{
    if (ipc)
    {
        //ipc->InitializeBatch();
        //ipc->Write<uint32_t, true>(addr, value);
        //ipc->SendCommand(ipc->FinalizeBatch());
        return injector::WriteMemory<uint32_t>(addr + gEEMainMemoryStart, value, true);
    }

    WriteBytes(addr, &value, sizeof(value));
}

void WriteMemoryRaw(uint32_t addr, void* value, uint32_t size)
{
    if (ipc)
    {
        auto temp = reinterpret_cast<uint8_t*>(value);
        //ipc->InitializeBatch();
        //for (auto i = 0; i < size; i++)
        //{
        //    ipc->Write<uint8_t, true>(addr + i, temp[i]);
        //}
        //ipc->SendCommand(ipc->FinalizeBatch());
        return injector::WriteMemoryRaw(addr + gEEMainMemoryStart, temp, size, true);
    }

    WriteBytes(addr, value, size);
}
#define IDR_INVOKER    101

std::promise<void> exitSignal;
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
    const uint32_t s_disc_crc,
    const uint32_t s_current_crc,
    const uint32_t s_elf_entry_point,
    uint8_t* EEMainMemoryStart,
    size_t EEMainMemorySize,
    const void* pWindowHandle,
    const uint32_t WindowSizeX,
    const uint32_t WindowSizeY,
    const bool IsFullscreen,
    const uint8_t AspectRatioSetting
);

CEXP bool VMStateIsRunning()
{
    if (ipc)
    {
        return ipc->Status() == PINE::Shared::EmuStatus::Running;
    }

    if (GetVMState)
        return GetVMState() == VMState::Running;
    return false;
}

void UnthrottleWatcher(std::future<void> futureObj, uint8_t* addr, const uint32_t& crc)
{
    [&]()
    {
        __try
        {
            spd::log()->info("Starting thread UnthrottleWatcher");
            while (futureObj.wait_for(std::chrono::milliseconds(1)) == std::future_status::timeout)
            {
                SetIsThrottlerTempDisabled(*addr != 0);
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
            }
            SetIsThrottlerTempDisabled(false);
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
                                MemoryFill(static_cast<uint32_t>(it.Addr), 0x00, static_cast<uint32_t>(it.Size));
                            }
                        }
                    }
                    break;
                }
            }
            else if (msg == WM_INPUT)
            {
                auto awnd = GetActiveWindow();
                if (hWnd == awnd || *(HWND*)gWindowHandle == awnd)
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
            info.Size += static_cast<uint32_t>(psec->get_size());
        }

        Elf_Half seg_num = reader.segments.size();
        for (int i = 0; i < seg_num; ++i) {
            const segment* pseg = reader.segments[i];

            if (info.SegmentFileOffset == 0)
                info.SegmentFileOffset = static_cast<uint32_t>(pseg->get_offset());
            else
                info.SegmentFileOffset = min(static_cast<uint32_t>(pseg->get_offset()), info.SegmentFileOffset);

            if (info.Base == 0)
                info.Base = static_cast<uint32_t>(pseg->get_virtual_address());
            else
                info.Base = min(static_cast<uint32_t>(pseg->get_virtual_address()), info.Base);

            info.Size += static_cast<uint32_t>(pseg->get_memory_size());
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
                        info.PluginDataAddr = static_cast<uint32_t>(value);
                        info.PluginDataSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "PCSX2Data")
                    {
                        info.PCSX2DataAddr = static_cast<uint32_t>(value);
                        info.PCSX2DataSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "CompatibleCRCList")
                    {
                        info.CompatibleCRCListAddr = static_cast<uint32_t>(value);
                        info.CompatibleCRCListSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "CompatibleElfCRCList")
                    {
                        info.CompatibleElfCRCListAddr = static_cast<uint32_t>(value);
                        info.CompatibleElfCRCListSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "KeyboardState")
                    {
                        info.KeyboardStateAddr = static_cast<uint32_t>(value);
                        info.KeyboardStateSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "MouseState")
                    {
                        info.MouseStateAddr = static_cast<uint32_t>(value);
                        info.MouseStateSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "CheatString")
                    {
                        info.CheatStringAddr = static_cast<uint32_t>(value);
                        info.CheatStringSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "OSDText")
                    {
                        info.OSDTextAddr = static_cast<uint32_t>(value);
                        info.OSDTextSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "FrameLimitUnthrottle")
                    {
                        info.FrameLimitUnthrottleAddr = static_cast<uint32_t>(value);
                        info.FrameLimitUnthrottleSize = static_cast<uint32_t>(size);
                    }
                    else if (name == "CLEOScripts")
                    {
                        info.CLEOScriptsAddr = static_cast<uint32_t>(value);
                        info.CLEOScriptsSize = static_cast<uint32_t>(size);
                    }
                }
            }
        }
        info.EntryPoint = static_cast<uint32_t>(reader.get_entry());
    }
    return info;
}

void LoadPlugins(
    const char* s_disc_serial,
    const char* s_disc_elf,
    const char* s_disc_version,
    const char* s_title,
    const char* s_elf_path,
    const uint32_t s_disc_crc,
    const uint32_t s_current_crc,
    const uint32_t s_elf_entry_point,
    uint8_t* EEMainMemoryStart,
    size_t EEMainMemorySize,
    const void* pWindowHandle,
    const uint32_t WindowSizeX,
    const uint32_t WindowSizeY,
    const bool IsFullscreen,
    const uint8_t AspectRatioSetting
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
    spd::log()->info("EE Memory Size is: {}", (uintptr_t)EEMainMemorySize);

    if (EEMainMemorySize < 0x0000000008000000)
    {
        constexpr auto ramerr = "Enable 128 MB RAM option in Settings -> Advanced and restart the emulator. Plugins will not be loaded at this time.";
        spd::log()->error(ramerr);
        MessageBoxA(NULL, ramerr, "PCSX2PluginInjector", MB_ICONERROR);
        return;
    }

    gEEMainMemoryStart = (uintptr_t)EEMainMemoryStart;
    gEEMainMemorySize = (size_t)EEMainMemorySize;
    gWindowHandle = pWindowHandle;
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
        WriteMemoryRaw(invoker.Base, buffer.data() + invoker.SegmentFileOffset, static_cast<uint32_t>(buffer.size()) - invoker.SegmentFileOffset);
        MemoryFill(invoker.PluginDataAddr, 0x00, invoker.PluginDataSize);
        WriteMemoryRaw(invoker.PluginDataAddr + (sizeof(PluginInfoInvoker) * count), &invoker, sizeof(PluginInfoInvoker));
        spd::log()->info("Finished injecting {}, {} bytes written at 0x{:X}", invokerPath.filename().string(), invoker.Size, invoker.Base);
        PluginRegions.emplace_back(invoker.Base, invoker.Base + invoker.Size);

        spd::log()->info("Hooking game's entry point function...", invokerPath.filename().string());
        auto patched = false;

        while (!s_elf_entry_point)
        {
            constexpr auto base = 0x100000;
            auto pattern = hook::pattern((uintptr_t)(EEMainMemoryStart) + base, (uintptr_t)(EEMainMemoryStart) + 0x2000000 - base, "28 0C 00 70 28 14 00 70 28 1C 00 70");
            if (!pattern.count_hint(1).empty())
            {
                *(uint32_t*)&s_elf_entry_point = uint32_t((uintptr_t)pattern.get_first(0) - (uintptr_t)EEMainMemoryStart);
                FallbackEntryPointChecker = s_elf_entry_point;
                break;
            }

            pattern = hook::pattern((uintptr_t)(EEMainMemoryStart) + base, (uintptr_t)(EEMainMemoryStart) + 0x2000000 - base, "3C 00 03 24 0C 00 00 00");
            if (!pattern.count_hint(1).empty())
            {
                *(uint32_t*)&s_elf_entry_point = uint32_t((uintptr_t)pattern.get_first(0) - (uintptr_t)EEMainMemoryStart);
                FallbackEntryPointChecker = s_elf_entry_point;
                break;
            }
        }

        auto ei_lookup = hook::pattern((uintptr_t)(EEMainMemoryStart) + s_elf_entry_point, (uintptr_t)(EEMainMemoryStart) + s_elf_entry_point + 2000, "38 00 00 42");
        if (!ei_lookup.count_hint(1).empty())
        {
            ei_hook = ei_lookup.count_hint(1).get_first<uint32_t>();
            ei_data = mips::jal(invoker.EntryPoint);
            WriteMemory32(uint32_t((uintptr_t)ei_hook - (uintptr_t)EEMainMemoryStart), ei_data);
            patched = true;
        }

        auto syscall7F_lookup = hook::pattern((uintptr_t)EEMainMemoryStart, (uintptr_t)(EEMainMemoryStart + 0x2000000), "7F 00 03 24 0C 00 00 00 08 00 E0 03 00 00 00 00");
        if (syscall7F_lookup.count_hint(2).size() >= 2)
        {
            auto syscall_hook = syscall7F_lookup.get(1).get<uint32_t>(0);
            auto syscall_data1 = mips::lui(mips::v0, HIWORD(0x2000000));
            auto syscall_data2 = mips::addiu(mips::v0, mips::v0, LOWORD(0x2000000));
            WriteMemory32(uint32_t((uintptr_t)syscall_hook - (uintptr_t)EEMainMemoryStart), syscall_data1);
            WriteMemory32(uint32_t((uintptr_t)syscall_hook + 4 - (uintptr_t)EEMainMemoryStart), syscall_data2);
            spd::log()->info("Syscall::GetMemorySize switched to return 0x{:X}", 0x2000000);
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
                    WriteMemoryRaw(mod.Base, buffer.data() + mod.SegmentFileOffset, static_cast<uint32_t>(buffer.size()) - mod.SegmentFileOffset);
                    WriteMemoryRaw(invoker.PluginDataAddr + (sizeof(PluginInfoInvoker) * count), &mod, sizeof(PluginInfoInvoker));
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
                            WriteMemory32(mod.PluginDataAddr, static_cast<uint32_t>(ini.size()));
                            WriteMemoryRaw(mod.PluginDataAddr + sizeof(uint32_t), ini.data(), static_cast<uint32_t>(ini.size()));
                            spd::log()->info("{} was successfully injected", iniPath.filename().string());
                        }
                    }

                    if (mod.PCSX2DataAddr)
                    {
                        spd::log()->info("Writing PCSX2 Data to {}", plugin_path.filename().string());
                        auto [DesktopSizeX, DesktopSizeY] = GetDesktopRes();
                        WriteMemory32(mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_DesktopSizeX), (uint32_t)DesktopSizeX);
                        WriteMemory32(mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_DesktopSizeY), (uint32_t)DesktopSizeY);
                        WriteMemory32(mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_WindowSizeX), (uint32_t)WindowSizeX);
                        WriteMemory32(mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_WindowSizeY), (uint32_t)WindowSizeY);
                        WriteMemory32(mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_IsFullscreen), (uint32_t)IsFullscreen);
                        WriteMemory32(mod.PCSX2DataAddr + (sizeof(uint32_t) * (uint32_t)PCSX2DataType::PCSX2Data_AspectRatioSetting), (uint32_t)AspectRatioSetting);
                    }

                    if (mod.KeyboardStateAddr)
                    {
                        spd::log()->info("{} requests keyboard state", plugin_path.filename().string());
                        InputData.emplace_back(PtrType::KeyboardData, (uintptr_t)(EEMainMemoryStart + mod.KeyboardStateAddr), mod.KeyboardStateSize);
                        MemoryFill(mod.KeyboardStateAddr, 0, mod.KeyboardStateSize);
                    }

                    if (mod.MouseStateAddr)
                    {
                        spd::log()->info("{} requests mouse state", plugin_path.filename().string());
                        InputData.emplace_back(PtrType::MouseData, (uintptr_t)(EEMainMemoryStart + mod.MouseStateAddr), mod.MouseStateSize);
                        MemoryFill(mod.MouseStateAddr, 0, mod.MouseStateSize);
                    }

                    if (mod.CheatStringAddr)
                    {
                        spd::log()->info("{} requests cheat string access", plugin_path.filename().string());
                        InputData.emplace_back(PtrType::CheatStringData, (uintptr_t)(EEMainMemoryStart + mod.CheatStringAddr), mod.CheatStringSize);
                        MemoryFill(mod.CheatStringAddr, 0, mod.CheatStringSize);
                    }

                    if (mod.OSDTextAddr)
                    {
                        spd::log()->info("{} requests OSD drawings", plugin_path.filename().string());
                        for (uint32_t i = 0; i < mod.OSDTextSize / OSDStringSize; i++)
                        {
                            MemoryFill(mod.OSDTextAddr + (OSDStringSize * i), 0, OSDStringSize);
                            auto block = (char*)(EEMainMemoryStart + mod.OSDTextAddr + (OSDStringSize * i));
                            GetOSDVector().emplace_back(std::string_view(block, OSDStringSize));
                        }
                    }

                    if (mod.FrameLimitUnthrottleAddr)
                    {
                        if (!fl_thread_created)
                        {
                            spd::log()->info("Some plugins can manage emulator's speed, creating thread to handle it");
                            MemoryFill(mod.FrameLimitUnthrottleAddr, 0x00, mod.FrameLimitUnthrottleSize);
                            std::future<void> futureObj = exitSignal.get_future();
                            std::thread th(&UnthrottleWatcher, std::move(futureObj), (uint8_t*)(EEMainMemoryStart + mod.FrameLimitUnthrottleAddr), std::ref(s_current_crc));
                            th.detach();
                            fl_thread_created = true;
                        }
                    }

                    if (mod.CLEOScriptsAddr)
                    {
                        spd::log()->info("CLEO Plugin detected, injecting CLEO Scripts");
                        MemoryFill(mod.CLEOScriptsAddr, 0x00, mod.CLEOScriptsSize);
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
                                        WriteMemory32(script_offset, static_cast<uint32_t>(script.size()));
                                        script_offset += sizeof(uint32_t);
                                        WriteMemoryRaw(script_offset, script.data(), static_cast<uint32_t>(script.size()));
                                        script_offset += static_cast<uint32_t>(script.size());
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
            if (!gWindowHandle)
            {
                EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(s_title));
                gWindowHandle = &FallbackWindowHandle;
            }

            auto hwnd = GetAncestor(*(HWND*)gWindowHandle, GA_ROOT);
            if (hwnd)
            {
                RegisterInputDevices(hwnd);
                auto wp = (LRESULT(WINAPI*)(HWND, UINT, WPARAM, LPARAM))GetWindowLongPtr(hwnd, GWLP_WNDPROC);
                if (wp != CustomWndProc)
                {
                    spd::log()->info("Keyboard and mouse data requested by plugins, replacing WndProc for HWND {}", reinterpret_cast<uint64_t>(hwnd));
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

void ExitSignal()
{
    exitSignal.set_value();
    std::promise<void>().swap(exitSignal);
}

CEXP void InitializeASI()
{
    std::call_once(CallbackHandler::flag, []()
    {
        WriteBytes = (tWriteBytes)GetProcAddress(GetModuleHandle(NULL), "WriteBytes");
        GetIsThrottlerTempDisabled = (tGetIsThrottlerTempDisabled)GetProcAddress(GetModuleHandle(NULL), "GetIsThrottlerTempDisabled");
        SetIsThrottlerTempDisabled = (tSetIsThrottlerTempDisabled)GetProcAddress(GetModuleHandle(NULL), "SetIsThrottlerTempDisabled");
        GetVMState = (tGetVMState)GetProcAddress(GetModuleHandle(NULL), "GetVMState");
        AddOnGameElfInitCallback = (tAddOnGameElfInitCallback)GetProcAddress(GetModuleHandle(NULL), "AddOnGameElfInitCallback");
        AddOnGameShutdownCallback = (tAddOnGameShutdownCallback)GetProcAddress(GetModuleHandle(NULL), "AddOnGameShutdownCallback");
        
        if (WriteBytes && GetIsThrottlerTempDisabled && SetIsThrottlerTempDisabled && GetVMState && AddOnGameElfInitCallback && AddOnGameShutdownCallback)
        {
            AddOnGameElfInitCallback(LoadPlugins);
            AddOnGameShutdownCallback(ExitSignal);
        }
        else
        {
            // cringe
            ipc = new PINE::PCSX2();

            std::thread([]()
            {
                static bool bElfChanged = false;
                static std::string s_title("UNAVAILABLE");
                auto start = std::chrono::high_resolution_clock::now();

                while (true)
                {
                    auto status = ipc->GetError();

                    if (status == PINE::Shared::IPCStatus::NoConnection)
                    {
                        constexpr auto err = "Enable PINE option in Settings -> Advanced(Port 28011) and restart the emulator. Plugins will not be loaded at this time.";
                        spd::log()->error(err);
                        MessageBoxA(NULL, err, "PCSX2PluginInjector", MB_ICONERROR);
                        break;
                    }

                    if (ipc->Status() == PINE::Shared::EmuStatus::Running && FallbackEntryPointChecker)
                    {
                        auto EEmem = (uint8_t**)GetProcAddress(GetModuleHandle(NULL), "EEmem");
                        auto curData = *(uint32_t*)(*EEmem + FallbackEntryPointChecker);
                        static auto oldData = *(uint32_t*)(*EEmem + FallbackEntryPointChecker);
                        if (curData != oldData)
                        {
                            if (oldData == 0x70000C28 || oldData == 0x2403003C) {
                                bElfChanged = true;
                                //spd::log()->info("ELF Switch detected, trying to load plugins...");
                            }
                        }
                        oldData = curData;
                    }

                    static auto old = ipc->Status();
                    auto cur = ipc->Status();
                    if (cur != old || bElfChanged)
                    {
                        if (bElfChanged)
                        {
                            bElfChanged = false;
                            ExitSignal();
                            FallbackWindowHandle = {};
                            FallbackEntryPointChecker = {};
                        }

                        if (ipc->Status() == PINE::Shared::EmuStatus::Running)
                        {
                            std::string s_disc_serial("UNAVAILABLE");
                            std::string s_disc_elf("UNAVAILABLE");
                            std::string s_disc_version("UNAVAILABLE");
                            //std::string s_title("UNAVAILABLE");
                            std::string s_elf_path("UNAVAILABLE");
                            uint32_t s_disc_crc = 0;
                            uint32_t s_current_crc = 0;
                            uint32_t s_elf_entry_point = 0;
                            uint8_t* EEMainMemoryStart = 0;
                            size_t EEMainMemorySize = 0;
                            void* WindowHandle = nullptr;
                            uint32_t WindowSizeX = 1280;
                            uint32_t WindowSizeY = 720;
                            bool IsFullscreen = false;
                            uint8_t AspectRatioSetting = uint8_t(Stretch);

                            auto EEmem = (uint8_t**)GetProcAddress(GetModuleHandle(NULL), "EEmem");
                            if (EEmem)
                            {
                                EEMainMemoryStart = *EEmem;
                                EEMainMemorySize = 0x8000000;

                                // not really needed but whatever
                                while (ipc->Read<uint8_t>(0x3200000) != 77)
                                {
                                    ipc->Write<uint8_t>(0x3200000, 77);

                                    auto now = std::chrono::high_resolution_clock::now();
                                    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start);

                                    if (duration.count() >= 5)
                                    {
                                        EEMainMemorySize = 0x2000000;
                                        start = std::chrono::high_resolution_clock::now();
                                    }
                                    std::this_thread::yield();
                                }
                                ipc->Write<uint8_t>(0x3200000, 0);
                            }

                            auto Title = ipc->GetGameTitle();
                            s_title = Title;
                            auto GameID = ipc->GetGameID();
                            s_disc_serial = GameID;
                            auto GameUUID = ipc->GetGameUUID();
                            s_disc_crc = std::stoul(GameUUID, nullptr, 16);
                            s_current_crc = s_disc_crc; // INVALID, s_current_crc is not exposed because exposing it will break all threads, set your house on fire, and kill your dog
                            auto GameVersion = ipc->GetGameVersion();
                            s_disc_version = GameVersion;

                            EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(s_title.c_str()));

                            if (FallbackWindowHandle)
                            {
                                WindowHandle = &FallbackWindowHandle;
                                RECT ClientRect = {};
                                GetClientRect(FallbackWindowHandle, &ClientRect);
                                WindowSizeX = ClientRect.right;
                                WindowSizeY = ClientRect.bottom;
                            }

                            AspectRatioSetting = uint8_t(Stretch); // not exposed
                            s_elf_entry_point = 0; // not exposed

                            delete[] Title;
                            delete[] GameID;
                            delete[] GameUUID;
                            delete[] GameVersion;

                            LoadPlugins( // race condition, let's set them threads on fire
                                s_disc_serial.c_str(),
                                s_disc_elf.c_str(),
                                s_disc_version.c_str(),
                                s_title.c_str(),
                                s_elf_path.c_str(),
                                s_disc_crc,
                                s_current_crc,
                                s_elf_entry_point,
                                EEMainMemoryStart,
                                EEMainMemorySize,
                                WindowHandle,
                                WindowSizeX,
                                WindowSizeY,
                                IsFullscreen,
                                AspectRatioSetting);
                        }
                        else
                        {
                            ExitSignal();
                            FallbackWindowHandle = {};
                            FallbackEntryPointChecker = {};
                        }
                    }
                    old = cur;

                    auto now = std::chrono::high_resolution_clock::now();
                    auto duration = std::chrono::duration_cast<std::chrono::seconds>(now - start);

                    if (duration.count() >= 1)
                    {
                        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(s_title.c_str()));
                        start = std::chrono::high_resolution_clock::now();
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(500));
                }

                delete ipc;
            }).detach();
        }
    });
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        if (!IsUALPresent()) { InitializeASI(); }
    }

    if (reason == DLL_PROCESS_DETACH)
    {

    }
    return TRUE;
}
