#include <elfio/elfio.hpp>
#include "stdafx.h"
#include <atomic>
#include <thread>
#include <iostream>
#include <chrono>
#include <future>
#include <filesystem>
#include <tlhelp32.h>
#include "safetyhook.hpp"
#include <pcsx2/mips.hpp>

#include <utility/Scan.hpp>

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
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }
    }();
}

// ---------------------------------------------------------------------------
// Keyboard and mouse
//
// The window the game renders into is not a stable one. PCSX2 destroys and
// recreates its display widget whenever the render window changes (on Windows a
// fullscreen switch always creates a new one), so a raw input device that was
// registered against that handle, and a window procedure that was installed on
// it, end up attached to a window that no longer exists and the plugins stop
// seeing input.
//
// The devices are therefore registered against a window of our own that never
// changes, and the window of the game is looked up again for every event. Input
// only reaches the game while that window is the one in the foreground, so an
// open dialog of the emulator, its own window while the game renders into another
// one, or another application never feeds it.
//
// The window is created on the thread that owns the windows of the emulator, so
// the events are taken out of the queue and handed over by the thread the system
// favours for input while the game is in the foreground, and nothing has to wait
// for a thread of our own to be scheduled. A message hook of that thread is what
// creates it there, see EnsureInput.
// ---------------------------------------------------------------------------

struct InputDataT
{
    uint8_t Type;
    uint32_t GuestAddr;
    uintptr_t Addr;
    size_t Size;
};

static std::mutex s_inputMutex;
static std::vector<InputDataT> InputData;

// only ever touched by the thread that owns the raw input window
static bool s_gameInputActive = false;

// The address PCSX2 hands over points at the window handle of the renderer that
// was loaded when the game started, and the variable behind it is updated
// whenever the window is replaced, so the handle is read back instead of being
// remembered. The address itself belongs to that renderer: switching the
// renderer while a game runs frees it, hence the guard. No local objects in
// here, a __try cannot unwind them.
static HWND GetRenderWindowFromDevice()
{
    if (!gWindowHandle)
        return nullptr;

    __try
    {
        return *reinterpret_cast<HWND*>(const_cast<void*>(gWindowHandle));
    } __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return nullptr;
    }
}

// Is this one of the windows the game can be drawn into? The windows a dialog of
// the emulator is made of are owned, which is what leaves them out, and so is
// everything of another process. A window that is gone fails the first check,
// which is what a handle that was read from an address that does not belong to
// us any more has to fail as well.
static bool IsOwnGameWindow(HWND window)
{
    if (!window || !IsWindow(window) || !IsWindowVisible(window))
        return false;

    if (GetWindow(window, GW_OWNER) != nullptr)
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(window, &processId);
    return processId == GetCurrentProcessId();
}

// The game is drawn into the main window of the emulator, and the windows it
// opens on top of it, the settings for example, are owned, which is what leaves
// them out. Rendering into a window of its own is an option of the emulator, and
// that window is what the address above hands over.
static BOOL CALLBACK FindMainWindowProc(HWND hwnd, LPARAM lParam)
{
    auto& found = *reinterpret_cast<HWND*>(lParam);

    if (!IsOwnGameWindow(hwnd))
        return TRUE;

    RECT rect = {};
    GetClientRect(hwnd, &rect);

    RECT foundRect = {};
    if (found)
        GetClientRect(found, &foundRect);

    const auto area = static_cast<int64_t>(rect.right) * rect.bottom;
    const auto foundArea = static_cast<int64_t>(foundRect.right) * foundRect.bottom;
    if (!found || area > foundArea)
        found = hwnd;

    return TRUE;
}

// the window the game is drawn into at this very moment
static HWND GetRenderWindow(HWND foreground)
{
    if (const HWND window = GetRenderWindowFromDevice(); IsOwnGameWindow(window))
        return window;

    // The address above is only good while the renderer it came from lives, the
    // window is looked for instead when it is not. The search walks every window
    // of the system, so its result is kept, but only for the window that is in
    // the foreground: the emulator replaces the window it draws into whenever it
    // changes its window, and the window of before stays alive while that
    // happens, so a window that merely still exists is not the answer any more.
    static HWND found = nullptr;
    static HWND foundFor = nullptr;
    static UINT64 lastSearch = 0;

    const HWND root = foreground ? GetAncestor(foreground, GA_ROOT) : nullptr;

    if (IsOwnGameWindow(found) && foundFor == root)
        return found;

    const UINT64 now = GetTickCount64();
    if (foundFor == root && now - lastSearch < 1000)
        return IsOwnGameWindow(found) ? found : nullptr;

    lastSearch = now;
    foundFor = root;
    found = nullptr;
    EnumWindows(FindMainWindowProc, reinterpret_cast<LPARAM>(&found));

    return found;
}

// The game only sees input while the window it is drawn into is the one the user
// works in, which is either that window itself or the window that holds it. An
// open dialog of the emulator is an owned window and never matches, and neither
// does a window of another application.
static bool IsGameWindowInForeground(HWND foreground)
{
    if (!foreground)
        return false;

    DWORD processId = 0;
    GetWindowThreadProcessId(foreground, &processId);
    if (processId != GetCurrentProcessId())
        return false;

    const HWND root = GetAncestor(foreground, GA_ROOT);
    if (!IsOwnGameWindow(root))
        return false;

    const HWND game = GetRenderWindowFromDevice();

    // The renderer that handed the address over was switched out, and what is
    // left where it pointed is not a window of ours any more, so where the game
    // is drawn cannot be told apart from the windows of the emulator that only
    // hold its menu. The window the user works in is taken for the answer then:
    // without it the game would see no input at all until the renderer that
    // wrote the address is there again.
    if (!IsOwnGameWindow(game))
        return true;

    return foreground == game || root == GetAncestor(game, GA_ROOT);
}

static void ClearGameInput();

// Raw input is a stream of hundreds of events per second, so the state is not
// looked up for every single one of them: the change of the foreground window is
// reported by the event hook, and this is only the safety net for a report that
// was missed. Everything in here is a call into the window manager, and those are
// not allowed to pile up in front of the events of the game.
static void RefreshGameInput(HWND foreground)
{
    const bool active = IsGameWindowInForeground(foreground);

    if (!active && s_gameInputActive)
        ClearGameInput();

    s_gameInputActive = active;
}

// Zeroes the state of one plugin, the address is not looked up before the write,
// see WriteInputState.
//
// No local objects in here, a __try cannot unwind them.
static bool ClearInputState(const InputDataT& data)
{
    __try
    {
        MemoryFill(data.GuestAddr, 0x00, static_cast<uint32_t>(data.Size));
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// The keys and buttons the game still sees as held have to be released, otherwise
// it keeps walking, or firing, while the user is somewhere else entirely.
static void ClearGameInput()
{
    std::lock_guard<std::mutex> lock(s_inputMutex);

    for (auto it = InputData.begin(); it != InputData.end();)
    {
        // the cheat string is what the user typed, it is not an input state
        if (it->Type == PtrType::CheatStringData)
        {
            ++it;
            continue;
        }

        if (ClearInputState(*it))
            ++it;
        else
        {
            spd::log()->warn("The state of a plugin is gone, dropping it");
            it = InputData.erase(it);
        }
    }
}

static void UpdateKeyboardState(const InputDataT& data, const RAWKEYBOARD& keyboard)
{
    auto keys = reinterpret_cast<char*>(data.Addr);
    auto previousKeys = reinterpret_cast<char*>(data.Addr + KeyboardBufState::StateSize);

    const char pressed = (keyboard.Flags & RI_KEY_BREAK) ? 0 : 1;

    switch (keyboard.VKey)
    {
        case VK_CONTROL:
            if (keyboard.Flags & RI_KEY_E0)
            {
                previousKeys[VK_RCONTROL] = keys[VK_RCONTROL];
                keys[VK_RCONTROL] = pressed;
            }
            else
            {
                previousKeys[VK_LCONTROL] = keys[VK_LCONTROL];
                keys[VK_LCONTROL] = pressed;
            }
            break;
        case VK_MENU:
            if (keyboard.Flags & RI_KEY_E0)
            {
                previousKeys[VK_RMENU] = keys[VK_RMENU];
                keys[VK_RMENU] = pressed;
            }
            else
            {
                previousKeys[VK_LMENU] = keys[VK_LMENU];
                keys[VK_LMENU] = pressed;
            }
            break;
        case VK_SHIFT:
            if (keyboard.MakeCode == 0x36)
            {
                previousKeys[VK_RSHIFT] = keys[VK_RSHIFT];
                keys[VK_RSHIFT] = pressed;
            }
            else
            {
                previousKeys[VK_LSHIFT] = keys[VK_LSHIFT];
                keys[VK_LSHIFT] = pressed;
            }
            break;
        default:
            previousKeys[keyboard.VKey] = keys[keyboard.VKey];
            keys[keyboard.VKey] = pressed;
            break;
    }
}

// the newest character of the cheat string is the first one of the buffer
static void UpdateCheatString(const InputDataT& data, const RAWKEYBOARD& keyboard)
{
    if ((keyboard.Flags & RI_KEY_BREAK) == 0 || data.Size < 2)
        return;

    auto text = reinterpret_cast<char*>(data.Addr);
    const auto keycode = keyboard.VKey;

    // number or letter keys
    if ((keycode > 47 && keycode < 58) || (keycode > 64 && keycode < 91))
    {
        std::memcpy(&text[1], &text[0], data.Size - 2);
        text[0] = static_cast<char>(keycode);
        text[data.Size - 1] = 0;
    }
    else
    {
        text[0] = 0;
    }
}

static void UpdateMouseState(const InputDataT& data, const RAWMOUSE& mouse)
{
    if (data.Size < (sizeof(CMouseControllerState) * KeyboardBufState::StateNum))
        return;

    CMouseControllerState& state = *reinterpret_cast<CMouseControllerState*>(data.Addr);
    CMouseControllerState& previousState = *reinterpret_cast<CMouseControllerState*>(data.Addr + sizeof(CMouseControllerState));

    previousState = state;

    // Movement
    state.X += static_cast<float>(mouse.lLastX);
    state.Y += static_cast<float>(mouse.lLastY);

    // A button stays held until the event that releases it arrives, the events in
    // between only carry the movement and may not be read as a release.
    if (!state.lmb)
        state.lmb = (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_DOWN) != false;
    else
        state.lmb = (mouse.usButtonFlags & RI_MOUSE_LEFT_BUTTON_UP) == false;

    if (!state.rmb)
        state.rmb = (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_DOWN) != false;
    else
        state.rmb = (mouse.usButtonFlags & RI_MOUSE_RIGHT_BUTTON_UP) == false;

    if (!state.mmb)
        state.mmb = (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_DOWN) != false;
    else
        state.mmb = (mouse.usButtonFlags & RI_MOUSE_MIDDLE_BUTTON_UP) == false;

    if (!state.bmx1)
        state.bmx1 = (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_DOWN) != false;
    else
        state.bmx1 = (mouse.usButtonFlags & RI_MOUSE_BUTTON_4_UP) == false;

    if (!state.bmx2)
        state.bmx2 = (mouse.usButtonFlags & RI_MOUSE_BUTTON_5_DOWN) != false;
    else
        state.bmx2 = (mouse.usButtonFlags & RI_MOUSE_BUTTON_5_UP) == false;

    // Scroll
    if (mouse.usButtonFlags & RI_MOUSE_WHEEL)
    {
        state.Z += static_cast<signed short>(mouse.usButtonData);
        if (state.Z < 0.0f)
            state.wheelDown = true;
        else if (state.Z > 0.0f)
            state.wheelUp = true;
    }
}

// Writes one event into the state of one plugin.
//
// The address is not looked up before the write: a VirtualQuery crosses into the
// kernel, and this is the busiest path there is, while the address belongs to the
// guest memory of the emulator and only goes away when the game does. An address
// that is gone raises the fault instead, which is what the handler is there for,
// and the caller drops the buffer when that happens.
//
// No local objects in here, a __try cannot unwind them.
static bool WriteInputState(const InputDataT& data, const RAWINPUT& raw)
{
    __try
    {
        if (raw.header.dwType == RIM_TYPEKEYBOARD)
        {
            if (data.Type == PtrType::KeyboardData)
                UpdateKeyboardState(data, raw.data.keyboard);
            else if (data.Type == PtrType::CheatStringData)
                UpdateCheatString(data, raw.data.keyboard);
        }
        else if (data.Type == PtrType::MouseData)
        {
            UpdateMouseState(data, raw.data.mouse);
        }

        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

// hands one event to every plugin that asked for that kind of data, the lock is
// held by the caller
static void ApplyRawInput(const RAWINPUT& raw)
{
    // the input of a device, and of the kind that is handed to the plugins. Input
    // that was made up by another program has no device behind it.
    if (!raw.header.hDevice)
        return;

    if (raw.header.dwType != RIM_TYPEKEYBOARD && raw.header.dwType != RIM_TYPEMOUSE)
        return;

    for (auto it = InputData.begin(); it != InputData.end();)
    {
        if (WriteInputState(*it, raw))
            ++it;
        else
        {
            spd::log()->warn("The state of a plugin is gone, dropping it");
            it = InputData.erase(it);
        }
    }
}

// Reads the one event the message carries and hands it over.
//
// The bulk read (GetRawInputBuffer) is not an option here: it only returns data
// when the input is not delivered to a window, and a window is what has to be
// registered for the input to arrive while the emulator is not in the foreground.
static void ProcessRawInput(LPARAM lParam)
{
    alignas(RAWINPUT) uint8_t buffer[sizeof(RAWINPUT)] = {};
    UINT size = sizeof(buffer);
    const UINT result = GetRawInputData(reinterpret_cast<HRAWINPUT>(lParam), RID_INPUT, buffer, &size, sizeof(RAWINPUTHEADER));

    // The return value is the size of the data that was copied, which is the size
    // of the header plus the union member of the one device that reported the
    // event, while the size that comes back in the parameter is the size of the
    // whole union. The two are never equal, so only a failure is a failure here.
    if (result == static_cast<UINT>(-1) || result < sizeof(RAWINPUTHEADER))
        return;

    std::lock_guard<std::mutex> lock(s_inputMutex);
    ApplyRawInput(*reinterpret_cast<const RAWINPUT*>(buffer));
}

static LRESULT CALLBACK RawInputWndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (msg == WM_INPUT)
    {
        // The events are the input of the user, they are passed on before anything
        // else in here; whether they are meant for the game is looked up now and
        // then only, see RefreshGameInput.
        static auto eventCount = 0;
        if ((eventCount++ & 0x3F) == 0)
            RefreshGameInput(GetForegroundWindow());

        if (s_gameInputActive)
            ProcessRawInput(lParam);

        // the message still has to be handed on, that is what releases the buffer
        // of the event
    }

    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void CALLBACK ForegroundEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG idObject, LONG idChild, DWORD threadId, DWORD time)
{
    UNREFERENCED_PARAMETER(hook);
    UNREFERENCED_PARAMETER(threadId);
    UNREFERENCED_PARAMETER(time);

    if (event == EVENT_SYSTEM_FOREGROUND && idObject == OBJID_WINDOW && idChild == CHILDID_SELF)
        RefreshGameInput(hwnd);
}

// the window the devices are registered against, owned by the thread below
static HWND s_inputWindow = nullptr;
static HWINEVENTHOOK s_foregroundHook = nullptr;
static HHOOK s_windowHook = nullptr;

// Creates the window the devices are registered against and registers them. This
// runs on the thread that owns the windows of the emulator: that is the thread
// the system favours for input while the game is in the foreground, and its
// message loop dispatches the messages of this window, so the events never wait
// for a thread of our own to be scheduled.
static void CreateInputWindow()
{
    constexpr auto windowClassName = L"PCSX2PluginInjectorRawInput";

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&CreateInputWindow), &module);

    WNDCLASSEXW windowClass = {};
    windowClass.cbSize = sizeof(windowClass);
    windowClass.lpfnWndProc = RawInputWndProc;
    windowClass.hInstance = module;
    windowClass.lpszClassName = windowClassName;
    RegisterClassExW(&windowClass);

    // a window that is never shown, the devices stay registered against it however
    // often the emulator replaces the window it draws into
    const HWND window = CreateWindowExW(WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, windowClassName, L"", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, module, nullptr);
    if (!window)
    {
        spd::log()->error("Raw input window could not be created, keyboard and mouse data will not be injected.");
        return;
    }

    s_inputWindow = window;

    constexpr auto HID_USAGE_PAGE_GENERIC = 0x01;
    constexpr auto HID_USAGE_GENERIC_MOUSE = 0x02;
    constexpr auto HID_USAGE_GENERIC_KEYBOARD = 0x06;

    RAWINPUTDEVICE devices[2] = {};
    devices[0].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[0].usUsage = HID_USAGE_GENERIC_KEYBOARD;
    // RIDEV_INPUTSINK reports the input even when the emulator is in the
    // background, which is what makes it possible to see a key that is let go of
    // while the user is in another window, and the input of the game itself is
    // left alone either way
    devices[0].dwFlags = RIDEV_INPUTSINK;
    devices[0].hwndTarget = window;
    devices[1].usUsagePage = HID_USAGE_PAGE_GENERIC;
    devices[1].usUsage = HID_USAGE_GENERIC_MOUSE;
    devices[1].dwFlags = RIDEV_INPUTSINK;
    devices[1].hwndTarget = window;

    if (RegisterRawInputDevices(devices, _countof(devices), sizeof(RAWINPUTDEVICE)))
        spd::log()->info("Keyboard and mouse data requested by plugins, raw input registered");
    else
        spd::log()->error("Raw input could not be registered, error {}", static_cast<uint32_t>(GetLastError()));

    // the state is dropped the moment the window of the game loses the foreground,
    // which is what this event reports
    s_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundEventProc, 0, 0, WINEVENT_OUTOFCONTEXT);
}

// Runs on the thread of the emulator for the first message it takes out of its
// queue, which is the moment the window can be created on it.
static LRESULT CALLBACK WindowHookProc(int code, WPARAM wParam, LPARAM lParam)
{
    if (code == HC_ACTION && !s_inputWindow)
    {
        if (s_windowHook)
        {
            const HHOOK hook = s_windowHook;
            s_windowHook = nullptr;
            UnhookWindowsHookEx(hook);
        }

        CreateInputWindow();
    }

    return CallNextHookEx(nullptr, code, wParam, lParam);
}

// The devices are registered for the whole process and the window they belong to
// never changes, so this runs once per session, wherever the game is started from.
static void EnsureInput()
{
    if (s_inputWindow || s_windowHook)
        return;

    const HWND game = GetRenderWindow(GetForegroundWindow());
    if (!game)
        return;

    const DWORD threadId = GetWindowThreadProcessId(game, nullptr);
    if (!threadId || threadId == GetCurrentThreadId())
        return;

    HMODULE module = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&WindowHookProc), &module);

    s_windowHook = SetWindowsHookExW(WH_GETMESSAGE, WindowHookProc, module, threadId);
    if (!s_windowHook)
        spd::log()->error("The message hook of the emulator could not be installed, error {}", static_cast<uint32_t>(GetLastError()));
}

static bool HasInputPlugins()
{
    std::lock_guard<std::mutex> lock(s_inputMutex);
    return !InputData.empty();
}

static void ResetInput()
{
    ClearGameInput();

    std::lock_guard<std::mutex> lock(s_inputMutex);
    InputData.clear();
}

// the game is gone, nothing of its input state may be written any more
static void InputShutdown()
{
    ResetInput();
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
        for (int i = 0; i < sec_num; ++i)
        {
            section* psec = reader.sections[i];
            info.Size += static_cast<uint32_t>(psec->get_size());
        }

        Elf_Half seg_num = reader.segments.size();
        for (int i = 0; i < seg_num; ++i)
        {
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

        for (int i = 0; i < sec_num; ++i)
        {
            section* psec = reader.sections[i];
            if (psec->get_type() == SHT_SYMTAB)
            {
                const symbol_section_accessor symbols(reader, psec);
                for (unsigned int j = 0; j < symbols.get_symbols_num(); ++j)
                {
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
                    else if (name == "_init")
                    {
                        info.ps2sdk_libcpp_init = static_cast<uint32_t>(value);
                    }
                    else if (name == "__cxa_atexit")
                    {
                        info.__cxa_atexit = static_cast<uint32_t>(value);
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
    ResetInput();

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
        WriteMemoryRaw(invoker.PluginDataAddr + (sizeof(PluginInfo) * count), &invoker, sizeof(PluginInfo));
        spd::log()->info("Finished injecting {}, {} bytes written at 0x{:X}", invokerPath.filename().string(), invoker.Size, invoker.Base);
        PluginRegions.emplace_back(invoker.Base, invoker.Base + invoker.Size);

        spd::log()->info("Hooking game's entry point function...", invokerPath.filename().string());
        auto patched = false;

        while (!s_elf_entry_point)
        {
            constexpr auto base = 0x100000;
            auto pattern = hook::pattern((uintptr_t)(EEMainMemoryStart)+base, (uintptr_t)(EEMainMemoryStart)+0x2000000 - base, "28 0C 00 70 28 14 00 70 28 1C 00 70");
            if (!pattern.count_hint(1).empty())
            {
                *(uint32_t*)&s_elf_entry_point = uint32_t((uintptr_t)pattern.get_first(0) - (uintptr_t)EEMainMemoryStart);
                FallbackEntryPointChecker = s_elf_entry_point;
                break;
            }

            pattern = hook::pattern((uintptr_t)(EEMainMemoryStart)+base, (uintptr_t)(EEMainMemoryStart)+0x2000000 - base, "3C 00 03 24 0C 00 00 00");
            if (!pattern.count_hint(1).empty())
            {
                *(uint32_t*)&s_elf_entry_point = uint32_t((uintptr_t)pattern.get_first(0) - (uintptr_t)EEMainMemoryStart);
                FallbackEntryPointChecker = s_elf_entry_point;
                break;
            }
        }

        auto ei_lookup = hook::pattern((uintptr_t)(EEMainMemoryStart)+s_elf_entry_point, (uintptr_t)(EEMainMemoryStart)+s_elf_entry_point + 2000, "38 00 00 42");
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

                    auto BaseCheck = std::find_if(PluginRegions.begin(), PluginRegions.end(), [&mod](auto x)
                    {
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
                    WriteMemoryRaw(invoker.PluginDataAddr + (sizeof(PluginInfo) * count), &mod, sizeof(PluginInfo));
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
                        {
                            std::lock_guard<std::mutex> lock(s_inputMutex);
                            InputData.emplace_back(PtrType::KeyboardData, mod.KeyboardStateAddr, (uintptr_t)(EEMainMemoryStart + mod.KeyboardStateAddr), mod.KeyboardStateSize);
                        }
                        MemoryFill(mod.KeyboardStateAddr, 0, mod.KeyboardStateSize);
                    }

                    if (mod.MouseStateAddr)
                    {
                        spd::log()->info("{} requests mouse state", plugin_path.filename().string());
                        {
                            std::lock_guard<std::mutex> lock(s_inputMutex);
                            InputData.emplace_back(PtrType::MouseData, mod.MouseStateAddr, (uintptr_t)(EEMainMemoryStart + mod.MouseStateAddr), mod.MouseStateSize);
                        }
                        MemoryFill(mod.MouseStateAddr, 0, mod.MouseStateSize);
                    }

                    if (mod.CheatStringAddr)
                    {
                        spd::log()->info("{} requests cheat string access", plugin_path.filename().string());
                        {
                            std::lock_guard<std::mutex> lock(s_inputMutex);
                            InputData.emplace_back(PtrType::CheatStringData, mod.CheatStringAddr, (uintptr_t)(EEMainMemoryStart + mod.CheatStringAddr), mod.CheatStringSize);
                        }
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
                            for (const auto& entry : std::filesystem::recursive_directory_iterator(cleo_path, std::filesystem::directory_options::skip_permission_denied, ec))
                            {
                                auto ext = entry.path().extension().wstring();
                                if (iequals(ext, L".fxt") || iequals(ext, L".csa") || iequals(ext, L".csi"))
                                {
                                    auto script = LoadFileToBuffer(entry.path());
                                    if (script_offset + sizeof(uint32_t) + script.size() <= mod.CLEOScriptsAddr + mod.CLEOScriptsSize)
                                    {
                                        spd::log()->info("Injecting {}", entry.path().filename().string());
                                        auto name = entry.path().lexically_relative(cleo_path).string();
                                        WriteMemoryRaw(script_offset, name.data(), name.size() + 1);
                                        script_offset += name.size() + 1;
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

        if (HasInputPlugins())
        {
            if (!gWindowHandle)
            {
                EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(s_title));
                gWindowHandle = &FallbackWindowHandle;
            }

            EnsureInput();
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
            if (procedure != NULL)
            {
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
        for (int i = 0; i < sec_num; ++i)
        {
            section* psec = reader.sections[i];
            if (psec->get_type() == SHT_SYMTAB)
            {
                const symbol_section_accessor symbols(reader, psec);
                for (unsigned int j = 0; j < symbols.get_symbols_num(); ++j)
                {
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

namespace PCSX2F
{
    class VMEvent
    {
    public:
        template <typename... Args>
        class Event : public std::function<void(Args...)>
        {
        public:
            using std::function<void(Args...)>::function;

        private:
            std::vector<std::function<void(Args...)>> handlers;

        public:
            void operator+=(std::function<void(Args...)>&& handler)
            {
                handlers.push_back(handler);
            }

            void executeAll(Args... args) const
            {
                if (!handlers.empty())
                {
                    for (auto& handler : handlers)
                    {
                        handler(args...);
                    }
                }
            }
        };

    public:
        static auto& onGameElfInit()
        {
            static Event<const char*, const char*, const char*, const char*, const char*,
                const uint32_t, const uint32_t, const uint32_t, uint8_t*, size_t, const void*,
                const uint32_t, const uint32_t, const bool, const uint8_t> eventEntryPointCompilingOnCPUThread;
            return eventEntryPointCompilingOnCPUThread;
        }
        static auto& onGameShutdown()
        {
            static Event<> eventShutdown;
            return eventShutdown;
        }
    };

    static volatile bool s_is_throttler_temp_disabled = false;

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
        const void* WindowHandle,
        const uint32_t WindowSizeX,
        const uint32_t WindowSizeY,
        const bool IsFullscreen,
        const uint8_t AspectRatioSetting);
    using ShutdownCB = void (*)();

    void(__fastcall* vtlb_memWrite8)(uint32_t addr, uint8_t data);
    void WriteBytes(uint32_t mem, const void* src, uint32_t size)
    {
        auto src8 = (uint8_t*)src;
        for (uint32_t i = 0; i < size; i++)
        {
            vtlb_memWrite8(mem + i, src8[i]);
        }
    }

    bool GetIsThrottlerTempDisabled()
    {
        return s_is_throttler_temp_disabled;
    }

    void SetIsThrottlerTempDisabled(bool disable)
    {
        s_is_throttler_temp_disabled = disable;
    }

    VMState* pVMSate = nullptr;
    VMState GetVMState()
    {
        if (pVMSate)
            return *pVMSate;
        return VMState::Running;
    }

    void AddOnGameElfInitCallback(InitCB callback)
    {
        VMEvent::onGameElfInit() += callback;
    }

    void AddOnGameShutdownCallback(ShutdownCB callback)
    {
        VMEvent::onGameShutdown() += callback;
    }

    std::string s_disc_serial("UNAVAILABLE");
    std::string s_disc_elf("UNAVAILABLE");
    std::string s_disc_version("UNAVAILABLE");
    const char** s_title;
    std::string s_elf_path("UNAVAILABLE");
    const uint32_t* s_disc_crc;
    const uint32_t* s_current_crc;
    const uint32_t* s_elf_entry_point;
    uint8_t* EEMainMemoryStart;
    const uint32_t* EEMainMemorySize;
    const uint8_t* AspectRatioSetting;

    bool Find_memWrite8()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                const auto candidate_string = utility::scan_string(current_module, "Warning: GetOsdConfigParam2 Reading extended language/version configs, may be incorrect!");
                if (candidate_string)
                {
                    auto candidate_stringref = utility::scan_displacement_reference(current_module, *candidate_string);
                    if (candidate_stringref)
                    {
                        auto ip = *candidate_stringref + 4;
                        auto next_call = utility::scan_mnemonic(ip, 100, "CALL");
                        if (next_call)
                        {
                            next_call = utility::scan_mnemonic(*next_call + 5, 100, "CALL");

                            const auto disp = utility::resolve_displacement(*next_call);

                            if (disp.has_value())
                            {
                                vtlb_memWrite8 = (void(__fastcall*)(uint32_t, uint8_t))disp.value();
                                result = true;
                                return;
                            }
                        }
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("memWrite8 could not be located.");

        return result;
    }

    bool Find_VMState()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                const auto candidate_string = utility::scan_string(current_module, "Applying settings...");
                if (candidate_string)
                {
                    auto candidate_stringref = utility::scan_displacement_reference(current_module, *candidate_string);
                    if (candidate_stringref)
                    {
                        auto ip = *candidate_stringref + 4;
                        const auto next_mov = utility::scan_mnemonic(ip, 100, "MOV");
                        if (next_mov)
                        {
                            const auto disp = utility::resolve_displacement(*next_mov);

                            if (disp.has_value())
                            {
                                pVMSate = (VMState*)disp.value();
                                result = true;
                                return;
                            }
                        }
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("VMState could not be located.");

        return result;
    }

    bool Find_AspectRatioSetting()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                auto candidate_stringref = utility::find_function_from_string_ref(current_module, "Patch: Setting aspect ratio to {} by patch request.", true);
                if (candidate_stringref)
                {
                    auto ip = *candidate_stringref;
                    for (size_t i = 0; i < 4000; ++i)
                    {
                        INSTRUX ix{};
                        const auto status = NdDecodeEx(&ix, (uint8_t*)ip, 1000, ND_CODE_64, ND_DATA_64);

                        if (!ND_SUCCESS(status))
                        {
                            break;
                        }

                        if (std::string_view{ ix.Mnemonic } == "CMP")
                        {
                            if (injector::ReadMemory<uint8_t>(ip + 6, true) == 0x1)
                            {
                                const auto disp = utility::resolve_displacement(ip);

                                if (disp.has_value())
                                {
                                    AspectRatioSetting = (const uint8_t*)disp.value();
                                    result = true;
                                    return;
                                }
                            }
                        }

                        ip += ix.Length;
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("AspectRatioSetting could not be located.");

        return result;
    }

    bool Find_ExposedRam()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                auto candidate_stringref = utility::find_function_from_string_ref(current_module, "MTVU: SPR Accessing VU1 Memory", true);
                if (candidate_stringref)
                {
                    auto ip = *candidate_stringref;
                    for (size_t i = 0; i < 4000; ++i)
                    {
                        INSTRUX ix{};
                        const auto status = NdDecodeEx(&ix, (uint8_t*)ip, 1000, ND_CODE_64, ND_DATA_64);

                        if (!ND_SUCCESS(status))
                        {
                            break;
                        }

                        if (std::string_view{ ix.Mnemonic } == "AND")
                        {
                            if (injector::ReadMemory<uint32_t>(ip + 1, true) == 0x1FFFFFF0)
                            {
                                auto counter = 0;
                                auto disp = std::optional<uintptr_t>();
                                do
                                {
                                    counter++;
                                    ip += ix.Length;
                                    disp = utility::resolve_displacement(ip);
                                    if (counter > 50)
                                        break;

                                } while (!disp.has_value());

                                if (disp.has_value())
                                {
                                    EEMainMemorySize = (const uint32_t*)disp.value();
                                    result = true;
                                    return;
                                }
                            }
                        }

                        ip += ix.Length;
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("ExposedRam could not be located.");

        return result;
    }

    bool Find_s_disc_crc()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                const auto candidate_string = utility::scan_string(current_module, "PS2 BIOS ({})");
                if (candidate_string)
                {
                    auto candidate_stringref = utility::scan_displacement_reference(current_module, *candidate_string);
                    if (candidate_stringref)
                    {
                        auto ip = *candidate_stringref + 4;
                        for (auto i = 0; i < 4; i++)
                        {
                            do
                            {
                                ip -= 1;
                            } while (injector::ReadMemory<uint32_t>(ip, true) != 0);
                            ip -= 6;

                            INSTRUX ix{};
                            const auto status = NdDecodeEx(&ix, (uint8_t*)ip, 1000, ND_CODE_64, ND_DATA_64);

                            if (ND_SUCCESS(status))
                            {
                                if (std::string_view{ ix.Mnemonic } == "MOV")
                                {
                                    const auto disp = utility::resolve_displacement(ip);

                                    if (disp.has_value())
                                    {
                                        s_disc_crc = (const uint32_t*)disp.value();
                                        result = true;
                                        return;
                                    }
                                }
                            }
                        }
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("s_disc_crc could not be located.");

        return result;
    }

    bool Find_s_current_crc()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                const auto candidate_string = utility::scan_string(current_module, "Failed to read ELF being loaded: {}: {}");
                if (candidate_string)
                {
                    auto candidate_stringref = utility::scan_displacement_reference(current_module, *candidate_string);
                    if (candidate_stringref)
                    {
                        auto ip = *candidate_stringref + 4;
                        for (size_t i = 0; i < 4000; ++i)
                        {
                            INSTRUX ix{};
                            const auto status = NdDecodeEx(&ix, (uint8_t*)ip, 1000, ND_CODE_64, ND_DATA_64);

                            if (!ND_SUCCESS(status))
                            {
                                break;
                            }

                            if (std::string_view{ ix.Mnemonic } == "MOV")
                            {
                                if (injector::ReadMemory<uint32_t>(ip + 6, true) == 0xFFFFFFFF)
                                {
                                    const auto disp = utility::resolve_displacement(ip);
                                    const auto disp2 = utility::resolve_displacement(ip + ix.Length);

                                    if (disp.has_value() && disp2.has_value())
                                    {
                                        s_elf_entry_point = (const uint32_t*)disp.value();
                                        s_current_crc = (const uint32_t*)disp2.value();
                                        result = true;
                                        return;
                                    }
                                }
                            }

                            ip += ix.Length;
                        }
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("s_current_crc could not be located.");

        return result;
    }

    bool Find_s_title()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                for (auto s : { "PCSX2 PS2 Emulator", "PCSX2 Emulator" })
                {
                    const auto current_module = GetModuleHandleW(NULL);
                    const auto candidate_string = utility::scan_string(current_module, s);
                    if (candidate_string)
                    {
                        auto candidate_stringref = utility::scan_displacement_reference(current_module, *candidate_string);
                        if (candidate_stringref)
                        {
                            auto ip = *candidate_stringref + 4;
                            auto k = 0;
                            for (size_t i = 0; i < 4000; ++i)
                            {
                                INSTRUX ix{};
                                const auto status = NdDecodeEx(&ix, (uint8_t*)ip, 1000, ND_CODE_64, ND_DATA_64);

                                if (!ND_SUCCESS(status))
                                {
                                    break;
                                }

                                if (std::string_view{ ix.Mnemonic } == "MOV")
                                {
                                    if (k >= 2)
                                    {
                                        const auto disp = utility::resolve_displacement(ip);

                                        if (disp.has_value())
                                        {
                                            s_title = (const char**)disp.value();
                                            result = true;
                                            return;
                                        }
                                    }
                                    k++;
                                }

                                ip += ix.Length;
                            }
                        }
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("s_title could not be located.");

        return result;
    }

    bool Hook__VMManager__Internal__EntryPointCompilingOnCPUThread()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                const auto candidate_string = utility::scan_string(current_module, "ELF {} with entry point at 0x{:08X} is executing.");
                if (candidate_string)
                {
                    std::optional<uintptr_t> prev_call;
                    auto candidate_stringref = utility::scan_displacement_reference(current_module, *candidate_string);
                    if (candidate_stringref)
                    {
                        uintptr_t call = 0;
                        auto ip = *candidate_stringref + 4;
                        for (size_t i = 0; i < 4000; ++i)
                        {
                            INSTRUX ix{};
                            const auto status = NdDecodeEx(&ix, (uint8_t*)ip, 1000, ND_CODE_64, ND_DATA_64);

                            if (!ND_SUCCESS(status))
                            {
                                break;
                            }

                            if (std::string_view{ ix.Mnemonic } == "CALL")
                            {
                                call = ip;
                            }
                            else if (std::string_view{ ix.Mnemonic } == "MOV")
                            {
                                if (injector::ReadMemory<uint32_t>(ip + 2, true) == 0x40000)
                                {
                                    static auto EntryPointCompilingOnCPUThreadHook = safetyhook::create_mid(call,
                                    [](SafetyHookContext& ctx)
                                    {
                                        auto EEmem = (uint8_t**)GetProcAddress(GetModuleHandle(NULL), "EEmem");
                                        auto ExposedRam = *PCSX2F::EEMainMemorySize;
                                        void* WindowHandle = nullptr;
                                        uint32_t WindowSizeX = 1280;
                                        uint32_t WindowSizeY = 720;

                                        EnumWindows(EnumWindowsProc, reinterpret_cast<LPARAM>(*PCSX2F::s_title));

                                        if (FallbackWindowHandle)
                                        {
                                            WindowHandle = &FallbackWindowHandle;
                                            RECT ClientRect = {};
                                            GetClientRect(FallbackWindowHandle, &ClientRect);
                                            WindowSizeX = ClientRect.right;
                                            WindowSizeY = ClientRect.bottom;
                                        }

                                        VMEvent::onGameElfInit().executeAll(
                                            PCSX2F::s_disc_serial.data(), PCSX2F::s_disc_elf.data(), PCSX2F::s_disc_version.data(), *PCSX2F::s_title,
                                            PCSX2F::s_elf_path.data(), *PCSX2F::s_disc_crc, *PCSX2F::s_current_crc, *PCSX2F::s_elf_entry_point,
                                            *EEmem, ExposedRam, WindowHandle,
                                            WindowSizeX, WindowSizeY,
                                            false, *PCSX2F::AspectRatioSetting
                                        );
                                    });
                                    result = true;
                                    return;
                                }
                            }

                            ip += ix.Length;
                        }
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("VMManager::Internal::EntryPointCompilingOnCPUThread could not be located.");

        return result;
    }

    SafetyHookInline g_ThrottleHook{};
    void __fastcall Throttle(void* vmmanager, void* edx)
    {
        if (GetIsThrottlerTempDisabled())
            return;
        return g_ThrottleHook.fastcall(vmmanager, edx);
    }

    bool Hook__VMManager__Internal__Throttle()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                const auto candidate_string = utility::scan_string(current_module, "    ================  EE COUNTER VSYNC START (frame: %d)  =======");
                if (candidate_string)
                {
                    std::optional<uintptr_t> prev_call;
                    auto candidate_stringref = utility::scan_displacement_reference(current_module, *candidate_string);
                    if (candidate_stringref)
                    {
                        auto counter = 0;
                        auto ip = *candidate_stringref;
                        do
                        {
                            ip -= 1;

                            INSTRUX ix{};
                            const auto status = NdDecodeEx(&ix, (uint8_t*)ip, 1000, ND_CODE_64, ND_DATA_64);

                            if (ND_SUCCESS(status))
                            {
                                if (std::string_view{ ix.Mnemonic } == "CALL")
                                {
                                    const auto disp = utility::resolve_displacement(ip);

                                    if (disp.has_value())
                                    {
                                        counter++;
                                    }
                                }
                            }
                        } while (counter < 3);

                        const auto disp = utility::resolve_displacement(ip);

                        if (disp)
                        {
                            g_ThrottleHook = safetyhook::create_inline(*disp, Throttle);
                            result = true;
                            return;
                        }
                    }
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("VMManager::Internal::Throttle could not be located.");

        return result;
    }

    bool Hook__VMManager__Shutdown()
    {
        bool result = false;
        __try
        {
            [&result]()
            {
                const auto current_module = GetModuleHandleW(NULL);
                auto candidate_stringref = utility::find_function_from_string_ref(current_module, "Failed to save resume state", true);
                if (candidate_stringref)
                {
                    static auto ShutdownHook = safetyhook::create_mid(*candidate_stringref,
                    [](SafetyHookContext& ctx)
                    {
                        VMEvent::onGameShutdown().executeAll();
                    });
                    result = true;
                    return;
                }
            }();
        } __except ((GetExceptionCode() == EXCEPTION_ACCESS_VIOLATION) ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH)
        {
        }

        if (!result)
            spd::log()->error("VMManager::Shutdown could not be located.");

        return result;
    }
}

CEXP void InitializeASI()
{
    static std::once_flag flag;
    std::call_once(flag, []()
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
            AddOnGameShutdownCallback(InputShutdown);
        }
        else
        {
            if (PCSX2F::Find_s_current_crc() && PCSX2F::Find_s_disc_crc() &&
                PCSX2F::Find_ExposedRam() && PCSX2F::Find_AspectRatioSetting() &&
                PCSX2F::Find_VMState() && PCSX2F::Find_memWrite8() && PCSX2F::Find_s_title() &&
                PCSX2F::Hook__VMManager__Internal__EntryPointCompilingOnCPUThread() &&
                PCSX2F::Hook__VMManager__Internal__Throttle() &&
                PCSX2F::Hook__VMManager__Shutdown())
            {
                WriteBytes = PCSX2F::WriteBytes;
                GetIsThrottlerTempDisabled = PCSX2F::GetIsThrottlerTempDisabled;
                SetIsThrottlerTempDisabled = PCSX2F::SetIsThrottlerTempDisabled;
                GetVMState = PCSX2F::GetVMState;
                AddOnGameElfInitCallback = PCSX2F::AddOnGameElfInitCallback;
                AddOnGameShutdownCallback = PCSX2F::AddOnGameShutdownCallback;

                AddOnGameElfInitCallback(LoadPlugins);
                AddOnGameShutdownCallback(ExitSignal);
                AddOnGameShutdownCallback(InputShutdown);
                AddOnGameShutdownCallback([] { FallbackWindowHandle = {}; });
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
                                if (oldData == 0x70000C28 || oldData == 0x2403003C)
                                {
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
        }
    });
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID lpReserved)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        if (!IsUALPresent()) { InitializeASI(); }
    }

    return TRUE;
}
