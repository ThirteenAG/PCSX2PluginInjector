#include "stdafx.h"
#include <thread>
#include <iostream>
#include <chrono>
#include <future>
#include <filesystem>


#include <pcsx2f_api.h>
#include "GuestModuleLoader.h"
#include "EmulatorHost.h"
#include "PluginSettingsService.h"

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

tWriteBytes WriteBytes = nullptr;
tGetIsThrottlerTempDisabled GetIsThrottlerTempDisabled = nullptr;
tSetIsThrottlerTempDisabled SetIsThrottlerTempDisabled = nullptr;
tGetVMState GetVMState = nullptr;
tAddOnGameElfInitCallback AddOnGameElfInitCallback = nullptr;
tAddOnGameShutdownCallback AddOnGameShutdownCallback = nullptr;

uintptr_t gEEMainMemoryStart;
size_t gEEMainMemorySize;

// Whether the emulator answers the render phase syscall of a guest plugin: an
// emulator without that hands an unknown syscall to its BIOS, so the plugins are
// told not to call it (see PCSX2Data_GuestRenderPhase).
static bool s_hostSupportsGuestRenderPhase = false;
static bool s_hostSupportsSettings = false;

void MemoryFill(uint32_t addr, uint8_t value, uint32_t size)
{
    if (size == 0) return;
    std::vector<uint8_t> bytes(size, value);
    WriteBytes(addr, bytes.data(), size);
}
void WriteMemory32(uint32_t addr, uint32_t value)
{
    WriteBytes(addr, &value, sizeof(value));
}
void WriteMemoryRaw(uint32_t addr, const void* value, uint32_t size)
{
    if (size) WriteBytes(addr, value, size);
}

std::promise<void> exitSignal;
std::thread s_unthrottleThread;
static GetGuestHostApi s_getGuestHostApi = nullptr;
static const PCSX2FGuestHostV1* s_guestHostApi = nullptr;
struct LoadedGuestModule { std::filesystem::path path; GuestModule::Image image; };
static std::vector<LoadedGuestModule> s_loadedGuestModules;
static std::mutex s_guestModuleMutex;
CEXP uint32_t InvokeGuestPluginSettings(uint32_t requestAddress, uint32_t caller)
{
    std::lock_guard lock(s_guestModuleMutex);
    for (const auto& module : s_loadedGuestModules)
    {
        const auto& image = module.image;
        if (!image.contains(caller, 4, 4)) continue;
        if ((requestAddress & 3) || requestAddress < image.info.Base ||
            uint64_t(requestAddress) + sizeof(PCSX2FIniRequest) > uint64_t(image.info.Base) + image.info.Size)
            return PCSX2F_SETTINGS_INVALID;
        PCSX2FIniRequest request;
        auto* memory = reinterpret_cast<uint8_t*>(gEEMainMemoryStart) + requestAddress;
        std::memcpy(&request, memory, sizeof(request));
        try
        {
            auto path = module.path;
            path.replace_extension(L".ini");
            const uint32_t status = PluginSettings::Process(path, request);
            if (status == PCSX2F_SETTINGS_OK && request.operation == PCSX2F_SETTINGS_READ)
                std::memcpy(memory, &request, sizeof(request));
            return status;
        }
        catch (const std::exception&)
        {
            return PCSX2F_SETTINGS_IO_ERROR;
        }
    }
    return PCSX2F_SETTINGS_INVALID;
}
void ExitSignal();
const void* gWindowHandle;
static std::mutex s_osdMutex;

std::vector<std::string_view>& GetOSDVector()
{
    static std::vector<std::string_view> osd;
    return osd;
}

CEXP size_t GetOSDVectorSize()
{
    std::lock_guard lock(s_osdMutex);
    return GetOSDVector().size();
}

CEXP const char* GetOSDVectorData(size_t index)
{
    std::lock_guard lock(s_osdMutex);
    // A reset may remove a row between the renderer's count and data calls.
    static const char empty[OSDStringSize] = {};
    if (index >= GetOSDVector().size())
        return empty;
    else
        return GetOSDVector()[index].data();
}

// ---------------------------------------------------------------------------
// Drawing into the frame of the game, before its UI
//
// A plugin that knows where its game is between the world and the UI exports
// PCSX2F_OnGuestRenderPhase, see pcsx2f_api.h. The emulator asks for it with the
// frame the game is drawing into when a guest plugin reports that phase, and this
// is where the call of every plugin is collected: one export of this module is
// what the emulator invokes, so it does not have to know which plugins exist.
// ---------------------------------------------------------------------------

static std::vector<PCSX2FGuestRenderPhaseCallback>& GetGuestRenderPhaseCallbacks()
{
    static std::vector<PCSX2FGuestRenderPhaseCallback> callbacks;
    return callbacks;
}

static void RegisterGuestRenderPhasePlugin(HMODULE module)
{
    if (!module)
        return;

    auto callback = reinterpret_cast<PCSX2FGuestRenderPhaseCallback>(GetProcAddress(module, "PCSX2F_OnGuestRenderPhase"));
    if (!callback)
        return;

    if (!s_hostSupportsGuestRenderPhase)
    {
        spd::log()->info("Host has no guest before-UI render phase; native plugin uses its presentation fallback");
        return;
    }

    auto& callbacks = GetGuestRenderPhaseCallbacks();
    if (std::find(callbacks.begin(), callbacks.end(), callback) != callbacks.end())
        return;

    char modulePath[MAX_PATH] = {};
    GetModuleFileNameA(module, modulePath, MAX_PATH);
    spd::log()->info("{} draws into the frame of the game before its UI", modulePath);
    callbacks.push_back(callback);
}

// Whether there is a plugin that draws into the frame of the game at all: the
// emulator asks for this before a call, because the frame has to be drained for
// every one of them.
CEXP size_t GetGuestRenderPhaseCallbackCount()
{
    return GetGuestRenderPhaseCallbacks().size();
}

// Called by the emulator for the rendering phase a guest plugin reports.
CEXP void InvokeGuestRenderPhase(uint32_t phase, const PCSX2FRenderTargetInfo* target)
{
    for (auto& callback : GetGuestRenderPhaseCallbacks())
        callback(phase, target);
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
    if (GetVMState)
        return GetVMState() == VMState::Running;
    return false;
}

void UnthrottleWatcher(std::future<void> futureObj, uint8_t* addr)
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
    if (keyboard.VKey >= KeyboardBufState::StateSize || data.Size < StateNum * StateSize) return;
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
std::vector<char> LoadFileToBuffer(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) return {};
    const auto size = file.tellg();
    if (size <= 0 || size > GuestModule::MaxFileSize) return {};
    std::vector<char> bytes(static_cast<size_t>(size));
    file.seekg(0, std::ios::beg);
    if (!file.read(bytes.data(), static_cast<std::streamsize>(size))) return {};
    return bytes;
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

    ExitSignal();
    ResetInput();
    gEEMainMemoryStart = reinterpret_cast<uintptr_t>(EEMainMemoryStart);
    gEEMainMemorySize = EEMainMemorySize;
    gWindowHandle = pWindowHandle;
    s_guestHostApi = s_getGuestHostApi ? s_getGuestHostApi(PCSX2F_HOST_VERSION, sizeof(PCSX2FGuestHostV1)) : nullptr;
    if (!s_guestHostApi)
    {
        spd::log()->error("Update PCSX2-Fork-With-Plugins: relocatable module host API v1 is required");
        return;
    }
    std::error_code ec;
    const auto modulePath = std::filesystem::path(GetThisModulePath<std::wstring>());
    const auto pluginsPath = modulePath.parent_path() / L"PLUGINS";
    if (std::filesystem::exists(pluginsPath, ec))
    {
        const uint32_t arenaBegin = s_guestHostApi->arena_begin, arenaEnd = s_guestHostApi->arena_end;
        std::string error;
        struct PendingPlugin
        {
            std::filesystem::path path;
            GuestModule::Image image;
            std::vector<char> ini;
            bool hasIni = false;
            std::vector<char> cleo;
            uint32_t legacyContext = 0, legacyStack = 0; // start-up block of an old fixed-address plugin
        };
        std::vector<PendingPlugin> pending;
        std::vector<std::filesystem::path> candidates;
        for (std::filesystem::recursive_directory_iterator it(pluginsPath,
            std::filesystem::directory_options::skip_permission_denied, ec), end;
            it != end; it.increment(ec))
        {
            if (ec) break;
            if (it->is_regular_file(ec) && !iequals(it->path().filename().wstring(), L"PCSX2PluginInvoker.elf") && iequals(it->path().extension().wstring(), L".elf"))
                candidates.push_back(it->path());
        }
        if (ec) spd::log()->warn("Plugin directory scan: {}", ec.message());
        std::sort(candidates.begin(), candidates.end());

        // Modules are placed from the top of the guest arena downwards, so the extended RAM
        // right above the game's own 32 MB stays free as long as possible (a game patched to
        // use more memory grows its heap upwards from there). Old fixed-address plugins keep
        // the addresses they were linked to; everything else is placed around them.
        struct Range { uint32_t begin, end; std::string name; };
        std::vector<Range> fixedRanges;
        uint32_t top = arenaEnd;
        // Finds the highest 128-aligned base below `top` where `size` bytes fit without
        // touching a fixed range. `limit` is the end of the free space the base was found in.
        auto place = [&](uint32_t size, uint32_t& base, uint32_t& limit) -> bool
        {
            uint64_t ceiling = top;
            while (ceiling >= uint64_t(arenaBegin) + size)
            {
                const uint32_t candidate = static_cast<uint32_t>((ceiling - size) & ~uint64_t(127));
                if (candidate < arenaBegin) return false;
                const Range* hit = nullptr;
                for (const auto& range : fixedRanges)
                    if (candidate < range.end && range.begin < uint64_t(candidate) + size && (!hit || range.begin < hit->begin))
                        hit = &range;
                if (!hit)
                {
                    base = candidate;
                    limit = static_cast<uint32_t>(ceiling);
                    return true;
                }
                ceiling = hit->begin;
            }
            return false;
        };
        auto compatible = [&](const GuestModule::Image& image)
        {
            const auto& mod = image.info;
            return image.matchesCRC(mod.CompatibleCRCListAddr, mod.CompatibleCRCListSize, s_disc_crc) &&
                (!mod.CompatibleElfCRCListAddr || image.matchesCRC(mod.CompatibleElfCRCListAddr, mod.CompatibleElfCRCListSize, s_current_crc));
        };
        // Reads the plugin's ini and CLEO scripts into its pending entry.
        auto accept = [&](PendingPlugin&& plugin) -> bool
        {
            const auto& path = plugin.path;
            const auto& mod = plugin.image.info;
            if (pending.size() >= 256)
            {
                spd::log()->error("{} skipped: module limit is 256", path.filename().string());
                return false;
            }
            auto iniPath = std::filesystem::path(path).replace_extension(L".ini");
            if (mod.PluginDataAddr && std::filesystem::exists(iniPath, ec))
            {
                plugin.ini = LoadFileToBuffer(iniPath);
                const uint32_t capacity = mod.PluginDataSize - 4;
                if (plugin.ini.size() > capacity)
                {
                    spd::log()->warn("{} truncated to {} bytes", iniPath.filename().string(), capacity);
                    plugin.ini.resize(capacity);
                }
                plugin.hasIni = true;
            }
            if (mod.CLEOScriptsAddr)
            {
                const auto cleoPath = pluginsPath / L"CLEO";
                std::vector<std::filesystem::path> scripts;
                if (std::filesystem::exists(cleoPath, ec))
                {
                    for (std::filesystem::recursive_directory_iterator it(cleoPath,
                        std::filesystem::directory_options::skip_permission_denied, ec), end;
                        it != end; it.increment(ec))
                    {
                        if (ec) break;
                        auto ext = it->path().extension().wstring();
                        if (it->is_regular_file(ec) && (iequals(ext, L".fxt") || iequals(ext, L".csa") || iequals(ext, L".csi")))
                            scripts.push_back(it->path());
                    }
                    if (ec) spd::log()->warn("CLEO directory scan: {}", ec.message());
                }
                std::sort(scripts.begin(), scripts.end());
                for (const auto& scriptPath : scripts)
                {
                    auto script = LoadFileToBuffer(scriptPath);
                    if (script.empty()) continue;
                    auto name = scriptPath.lexically_relative(cleoPath).string();
                    const uint64_t recordSize = name.size() + 1 + sizeof(uint32_t) + script.size();
                    // Include the path, length, contents, and a final empty-name sentinel.
                    if (plugin.cleo.size() + recordSize + 1 > mod.CLEOScriptsSize)
                    {
                        spd::log()->warn("CLEO script {} skipped: buffer is full", name);
                        continue;
                    }
                    plugin.cleo.insert(plugin.cleo.end(), name.c_str(), name.c_str() + name.size() + 1);
                    const uint32_t size = static_cast<uint32_t>(script.size());
                    for (unsigned shift = 0; shift < 32; shift += 8)
                        plugin.cleo.push_back(static_cast<char>(size >> shift));
                    plugin.cleo.insert(plugin.cleo.end(), script.begin(), script.end());
                }
            }
            pending.push_back(std::move(plugin));
            return true;
        };

        // 1. Old fixed-address plugins, at the addresses they were linked to.
        std::vector<std::pair<std::filesystem::path, std::vector<char>>> relocatable;
        bool legacyLoaded = false;
        for (const auto& path : candidates)
        {
            auto file = LoadFileToBuffer(path);
            if (!GuestModule::IsLegacy(file))
            {
                relocatable.emplace_back(path, std::move(file));
                continue;
            }
            PendingPlugin plugin;
            plugin.path = path;
            if (!GuestModule::LoadLegacy(file, plugin.image, error))
            {
                spd::log()->warn("{} rejected: old fixed-address plugin, {}", path.filename().string(), error);
                s_guestHostApi->warn("Some PS2 plugins need updating. See PCSX2PluginInjector.log for filenames and details.");
                continue;
            }
            if (!compatible(plugin.image)) continue;
            const Range range{plugin.image.info.Base, plugin.image.info.Base + plugin.image.info.Size, path.filename().string()};
            if (range.begin < arenaBegin || range.end > arenaEnd)
            {
                spd::log()->warn("{} rejected: its fixed address range 0x{:08X}-0x{:08X} is outside the guest arena",
                    range.name, range.begin, range.end);
                continue;
            }
            const auto overlap = std::find_if(fixedRanges.begin(), fixedRanges.end(),
                [&](const Range& other) { return range.begin < other.end && other.begin < range.end; });
            if (overlap != fixedRanges.end())
            {
                spd::log()->warn("{} skipped: its fixed address range 0x{:08X}-0x{:08X} overlaps {}",
                    range.name, range.begin, range.end, overlap->name);
                continue;
            }
            spd::log()->warn("{} is an old fixed-address plugin; loading it at 0x{:08X} for compatibility. "
                "Update it, or rebuild it for the relocatable module ABI.", range.name, range.begin);
            if (!accept(std::move(plugin))) continue;
            fixedRanges.push_back(range);
            legacyLoaded = true;
        }

        // 2. Relocatable modules, from the top down. The layout depends on the base, so a
        // module is placed by the size it has at a probe base and loaded again at its own.
        for (auto& [path, file] : relocatable)
        {
            PendingPlugin plugin;
            plugin.path = path;
            if (!GuestModule::Load(file, arenaBegin, arenaEnd, plugin.image, error))
            {
                spd::log()->warn("{} rejected: {}", path.filename().string(), error);
                s_guestHostApi->warn("Some PS2 plugins need updating. See PCSX2PluginInjector.log for filenames and details.");
                continue;
            }
            if (!compatible(plugin.image)) continue;
            uint32_t size = plugin.image.info.Size, base = 0, limit = 0;
            bool placed = false;
            for (int attempt = 0; attempt < 8 && place(size, base, limit); ++attempt)
            {
                if (!GuestModule::Load(file, base, arenaEnd, plugin.image, error)) break;
                if (uint64_t(base) + plugin.image.info.Size <= limit) { placed = true; break; }
                size = plugin.image.info.Size;
            }
            if (!placed)
            {
                spd::log()->error("{} skipped: no room left in the guest arena{}", path.filename().string(),
                    error.empty() ? "" : " (" + error + ")");
                continue;
            }
            if (accept(std::move(plugin))) top = base;
        }

        // 3. Start-up blocks of the old plugins: the module context the runtime fills in,
        // and a stack of their own (the old invoker ran them on the game's stack).
        constexpr uint32_t LegacyBlockSize = 32 + 64 * 1024;
        for (auto it = pending.begin(); it != pending.end();)
        {
            uint32_t base = 0, limit = 0;
            if (!it->image.legacy) { ++it; continue; }
            if (!place(LegacyBlockSize, base, limit))
            {
                spd::log()->error("{} skipped: no room left in the guest arena for its stack", it->path.filename().string());
                it = pending.erase(it);
                continue;
            }
            it->legacyContext = base;
            it->legacyStack = base + LegacyBlockSize;
            top = base;
            ++it;
        }
        if (legacyLoaded)
            s_guestHostApi->warn("Some PS2 plugins use the old format and were loaded for compatibility. Update them; see PCSX2PluginInjector.log.");
        // Start in file name order, whatever the format.
        std::stable_sort(pending.begin(), pending.end(),
            [](const PendingPlugin& a, const PendingPlugin& b) { return a.path < b.path; });

        if (!pending.empty())
        {
            // All ELF/CRC/buffer/arena validation completed before any guest write.
            for (auto& plugin : pending)
            {
                const auto& image = plugin.image;
                bool armed = s_guestHostApi->write(image.info.Base, image.bytes.data(), image.info.Size);
                if (armed && image.legacy)
                {
                    // gp 0 keeps the game's gp, which the old invoker ran plugins with.
                    static const std::array<char, 32> emptyContext{};
                    armed = s_guestHostApi->write(plugin.legacyContext, emptyContext.data(), uint32_t(emptyContext.size())) &&
                        (!image.legacyInit || s_guestHostApi->queue(image.legacyInit, plugin.legacyStack, 0, plugin.legacyContext)) &&
                        s_guestHostApi->queue(image.info.EntryPoint, plugin.legacyStack, 0, plugin.legacyContext);
                }
                else if (armed)
                    armed = s_guestHostApi->queue(image.info.EntryPoint, image.stackTop, 0, image.context);
                if (!armed)
                {
                    s_guestHostApi->abort();
                    spd::log()->error("Module transaction rejected by the fork; startup was not armed");
                    return;
                }
            }
            bool fl_thread_created = false;
            for (auto& plugin : pending)
            {
                auto plugin_path = plugin.path;
                auto& mod = plugin.image.info;
                if (const auto* supported = plugin.image.find("PCSX2FSettingsVersion"); supported &&
                    supported->size == sizeof(uint32_t) && plugin.image.contains(supported->address, sizeof(uint32_t), 1))
                    WriteMemory32(supported->address, s_hostSupportsSettings ? 1u : 0u);
                if (plugin.hasIni)
                {
                    MemoryFill(mod.PluginDataAddr, 0, mod.PluginDataSize);
                    WriteMemory32(mod.PluginDataAddr, static_cast<uint32_t>(plugin.ini.size()));
                    if (!plugin.ini.empty())
                        WriteMemoryRaw(mod.PluginDataAddr + 4, plugin.ini.data(), static_cast<uint32_t>(plugin.ini.size()));
                }
                if (mod.PCSX2DataAddr)
                {
                    spd::log()->info("Writing PCSX2 Data to {}", plugin_path.filename().string());
                    auto [DesktopSizeX, DesktopSizeY] = GetDesktopRes();

                    // A plugin built against an older API has a shorter PCSX2Data, and what
                    // follows it in its memory (usually PluginData, the ini) must not be overwritten.
                    auto WritePCSX2Data = [&mod](PCSX2DataType type, uint32_t value)
                    {
                        const uint32_t offset = 4 * static_cast<uint32_t>(type);
                        if (offset + sizeof(uint32_t) <= mod.PCSX2DataSize)
                            WriteMemory32(mod.PCSX2DataAddr + offset, value);
                    };

                    WritePCSX2Data(PCSX2DataType::PCSX2Data_DesktopSizeX, (uint32_t)DesktopSizeX);
                    WritePCSX2Data(PCSX2DataType::PCSX2Data_DesktopSizeY, (uint32_t)DesktopSizeY);
                    WritePCSX2Data(PCSX2DataType::PCSX2Data_WindowSizeX, (uint32_t)WindowSizeX);
                    WritePCSX2Data(PCSX2DataType::PCSX2Data_WindowSizeY, (uint32_t)WindowSizeY);
                    WritePCSX2Data(PCSX2DataType::PCSX2Data_IsFullscreen, (uint32_t)IsFullscreen);
                    WritePCSX2Data(PCSX2DataType::PCSX2Data_AspectRatioSetting, (uint32_t)AspectRatioSetting);
                    WritePCSX2Data(PCSX2DataType::PCSX2Data_GuestRenderPhase, (uint32_t)s_hostSupportsGuestRenderPhase);
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
                    std::lock_guard lock(s_osdMutex);
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
                        s_unthrottleThread = std::thread(&UnthrottleWatcher, std::move(futureObj), (uint8_t*)(EEMainMemoryStart + mod.FrameLimitUnthrottleAddr));
                        fl_thread_created = true;
                    }
                }
                if (mod.CLEOScriptsAddr)
                {
                    MemoryFill(mod.CLEOScriptsAddr, 0, mod.CLEOScriptsSize);
                    if (!plugin.cleo.empty())
                        WriteMemoryRaw(mod.CLEOScriptsAddr, plugin.cleo.data(), static_cast<uint32_t>(plugin.cleo.size()));
                }
            }
            if (!s_guestHostApi->commit())
            {
                s_guestHostApi->abort();
                ExitSignal();
                ResetInput();
                spd::log()->error("Module commit failed; startup was not armed");
                return;
            }
            {
                std::lock_guard lock(s_guestModuleMutex);
                for (auto& plugin : pending)
                {
                    spd::log()->info("Loaded {} {} at 0x{:08X}, entry 0x{:08X}, generation {}",
                        plugin.path.filename().string(), plugin.image.legacy ? "(old format, fixed address)" : "dynamically",
                        plugin.image.info.Base, plugin.image.info.EntryPoint, s_guestHostApi->generation);
                    s_loadedGuestModules.push_back({plugin.path, std::move(plugin.image)});
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
        }
        else
            spd::log()->info("No compatible relocatable modules were loaded");
    }
    else
        spd::log()->info("No PLUGINS directory");

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

            RegisterGuestRenderPhasePlugin(h);
        }
    }
    else
    {
        // the module is already there, what it exports has to be picked up as well
        RegisterGuestRenderPhasePlugin(GetModuleHandle(XboxRainDroplets));
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

// The path of a plugin, from the caller of GetPluginSymbolAddr. A caller only knows which
// plugin it wants to look a symbol up in, and the working directory of the emulator is not
// where the plugins are (it is not even stable: opening a file dialog of the emulator
// changes it), so a path that is not absolute is taken relative to the directory of this
// module first, which is where the plugins folder of the emulator is, and relative to the
// plugins folder itself then, which is what a caller that only names the file means.
static std::filesystem::path GetPluginPath(const char* path)
{
    std::filesystem::path given(path ? path : "");

    if (given.empty() || given.is_absolute())
        return given;

    std::filesystem::path modulePath(GetThisModulePath<std::wstring>());
    modulePath.remove_filename();

    std::error_code ec;

    const std::filesystem::path beside = modulePath / given;
    if (std::filesystem::exists(beside, ec))
        return beside;

    const std::filesystem::path inside = modulePath / L"PLUGINS" / given;
    if (std::filesystem::exists(inside, ec))
        return inside;

    return given;
}

CEXP uintptr_t GetPluginSymbolAddr(const char* path, const char* sym_name)
{
    if (!path || !sym_name) return 0;
    const auto wanted = GetPluginPath(path).lexically_normal();
    std::lock_guard lock(s_guestModuleMutex);
    for (const auto& module : s_loadedGuestModules)
        if (iequals(module.path.lexically_normal().wstring(), wanted.wstring()))
            if (const auto* symbol = module.image.find(sym_name)) return symbol->address;
    return 0;
}

void ExitSignal()
{
    exitSignal.set_value();
    if (s_unthrottleThread.joinable()) s_unthrottleThread.join();
    std::promise<void>().swap(exitSignal);
    {
        std::lock_guard lock(s_osdMutex);
        GetOSDVector().clear();
    }
    std::lock_guard lock(s_guestModuleMutex);
    s_loadedGuestModules.clear();
}

CEXP void InitializeASI()
{
    static std::once_flag flag;
    std::call_once(flag, []()
    {
        s_getGuestHostApi = reinterpret_cast<GetGuestHostApi>(GetProcAddress(GetModuleHandle(nullptr), "GetGuestPluginHostApi"));
        s_guestHostApi = s_getGuestHostApi ? s_getGuestHostApi(PCSX2F_HOST_VERSION, sizeof(PCSX2FGuestHostV1)) : nullptr;
        WriteBytes = reinterpret_cast<tWriteBytes>(GetProcAddress(GetModuleHandle(nullptr), "WriteBytes"));
        GetIsThrottlerTempDisabled = reinterpret_cast<tGetIsThrottlerTempDisabled>(GetProcAddress(GetModuleHandle(nullptr), "GetIsThrottlerTempDisabled"));
        SetIsThrottlerTempDisabled = reinterpret_cast<tSetIsThrottlerTempDisabled>(GetProcAddress(GetModuleHandle(nullptr), "SetIsThrottlerTempDisabled"));
        GetVMState = reinterpret_cast<tGetVMState>(GetProcAddress(GetModuleHandle(nullptr), "GetVMState"));
        AddOnGameElfInitCallback = reinterpret_cast<tAddOnGameElfInitCallback>(GetProcAddress(GetModuleHandle(nullptr), "AddOnGameElfInitCallback"));
        AddOnGameShutdownCallback = reinterpret_cast<tAddOnGameShutdownCallback>(GetProcAddress(GetModuleHandle(nullptr), "AddOnGameShutdownCallback"));
        bool stock = false;
        if (!s_guestHostApi || !WriteBytes || !GetIsThrottlerTempDisabled || !SetIsThrottlerTempDisabled ||
            !GetVMState || !AddOnGameElfInitCallback || !AddOnGameShutdownCallback)
        {
            // A fork advertising the API must not silently fall back to hooks.
            if (s_getGuestHostApi)
            {
                MessageBoxA(nullptr, "Update PCSX2-Fork-With-Plugins. Its guest plugin API is incomplete or incompatible.", "PCSX2PluginInjector", MB_ICONWARNING);
                return;
            }
            StockPCSX2::Bindings bindings{};
            if (!StockPCSX2::Initialize(bindings)) return;
            s_getGuestHostApi = bindings.guest_api;
            s_guestHostApi = s_getGuestHostApi(PCSX2F_HOST_VERSION, sizeof(PCSX2FGuestHostV1));
            WriteBytes = bindings.write;
            GetIsThrottlerTempDisabled = bindings.get_unthrottle;
            SetIsThrottlerTempDisabled = bindings.set_unthrottle;
            GetVMState = bindings.state;
            AddOnGameElfInitCallback = bindings.add_init;
            AddOnGameShutdownCallback = bindings.add_shutdown;
            stock = true;
        }
        s_hostSupportsGuestRenderPhase = GetProcAddress(GetModuleHandle(nullptr), "PCSX2F_GuestRenderPhaseSupported") != nullptr;
        s_hostSupportsSettings = stock || GetProcAddress(GetModuleHandle(nullptr), "PCSX2F_PluginSettingsSupported") != nullptr;
        AddOnGameElfInitCallback(LoadPlugins);
        AddOnGameShutdownCallback(ExitSignal);
        AddOnGameShutdownCallback(InputShutdown);
        AddOnGameShutdownCallback([] { FallbackWindowHandle = {}; });
        if (stock) StockPCSX2::Activate();
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
